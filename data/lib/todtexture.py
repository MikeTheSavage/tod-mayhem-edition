"""TOD TEXTURE assets inside an asset block: read the record, and replace a texture with a bigger one in place.

Ported from TOD HD's texture writer (TOD HD's app/tod_hd_game.py: Block._texture, texture_patches, rewrite_block),
which was checked against the GOG TOD.exe and every shipped block and installed TOD HD's HUD set (256 x 256 icons,
512 x 256 weapon cards) into the game. Kept self-contained here, stdlib only; the block layout is todblock.Block.

Texture record (type 0) in the block's INFO, offsets from the record start R (pointers are self-relative):
  +0x20 -> TextureInfo T (0x54 bytes), +0x28 -> GfxTexture G (0x2C bytes)
  G +0x08 -> R (surface bits)      G +0x0C/+0x10 resolution      G +0x14/+0x18 surface size (powers of two)
  G +0x1C format (1 A8R8G8B8, 7 DXT1)   G +0x20 descriptor (byte +0x22 = bits per pixel)
  G +0x24 bitfield (bit 0 DXT sizing, bits 1-8 level count, bit 9 engine flag)   G +0x28 mip mode (1 full chain, 0 one)
  T +0x4C pair kept equal to the resolution where the shipped data has it so.
Payload k of the block = the texture's mip levels, largest first, no header: A8R8G8B8 is B, G, R, A per texel.
Engine size (Texture::GetSizeForLevel 0x4633D0): DXT -> 8 * max(1, w/4) * max(1, h/4); else (bpp & 0x3F) * w * h >> 3,
summed over the levels, aligned to 4.

Hard limits (TOD HD, measured in game): only DXT1 and A8R8G8B8 load from an archive; one texture's payload stays at or
below TEXTURE_CAP (the largest maximumAssetSize of any shipped block); the new size keeps the record's aspect and is a
power of two (so a whole power-of-two multiple of the old size); a DXT1 record stays DXT1.
"""
import struct

import todblock as tb

FMT_A8R8G8B8, FMT_DXT1 = 1, 7
FMT_NAME = {FMT_A8R8G8B8: 'RGBA32', FMT_DXT1: 'DXT1'}
FMT_CODE = {'RGBA32': FMT_A8R8G8B8, 'DXT1': FMT_DXT1}
DESCRIPTOR = {FMT_A8R8G8B8: 0x208888, FMT_DXT1: 0x40000}
TEXTURE_CAP = 1_351_384
MAX_DIM = 4096
NULLPTR = 0x80000000


class TextureError(Exception):
    pass


u32, i32, a4, a16 = tb.u32, tb.i32, tb.a4, tb.a16


def is_pow2(n):
    return n > 0 and n & (n - 1) == 0


def full_chain(w, h):
    return max(w, h).bit_length()


def size_for_level(w, h, level, compressed, bpp):
    ww, hh = max(w >> level, 1), max(h >> level, 1)
    if compressed:
        return 8 * max(ww // 4, 1) * max(hh // 4, 1)
    return ((bpp & 0x3F) * hh * ww) >> 3


def engine_data_size(w, h, levels, compressed, bpp):
    return a4(sum(size_for_level(w, h, L, compressed, bpp) for L in range(levels)))


def payload_size(fmt, w, h, levels):
    code = FMT_CODE.get(fmt, fmt)
    return engine_data_size(w, h, levels, code == FMT_DXT1, 4 if code == FMT_DXT1 else 32)


def selfptr(buf, off):
    v = u32(buf, off)
    if v == NULLPTR:
        return None
    return (off + v) & 0xFFFFFFFC


def cstring(buf, o, limit=1024):
    e = buf.find(b'\0', o, min(len(buf), o + limit))
    if e < 0:
        raise TextureError('unterminated string at 0x%x' % o)
    return bytes(buf[o:e])


def records(block):
    """[{index, offset, end, type, name}] for every record of a todblock.Block, in order."""
    info, n = block.info, len(block.payloads)
    try:
        starts = tb.record_starts(info, n)
    except ValueError as e:
        raise TextureError(str(e))
    out = []
    for k, R in enumerate(starts):
        out.append(dict(index=k, offset=R, end=starts[k + 1] if k + 1 < n else len(info), type=u32(info, R),
                        name=tb.record_name(info, R)))
    return out


def texture_record(block, rec):
    """The texture fields of one type-0 record (strict: refuses anything the engine would not load the same way)."""
    info = block.info
    R, nxt = rec['offset'], rec['end']
    if rec['type'] != 0:
        raise TextureError('%s is not a texture record' % rec['name'])
    name_ptr = (R + 4 + u32(info, R + 4)) & 0xFFFFFFFC
    name_len = len(cstring(info, name_ptr))
    T, G = selfptr(info, R + 0x20), selfptr(info, R + 0x28)
    if T is None or G is None:
        raise TextureError('texture record 0x%x has a null TextureInfo/GfxTexture' % R)
    if not (R + 0x30 <= G and G + 0x2C <= nxt and R + 0x30 <= T and T + 0x54 <= nxt):
        raise TextureError('texture record 0x%x: TextureInfo/GfxTexture outside its span' % R)
    far = max(a4(name_ptr + name_len + 1), G + 0x2C, T + 0x54)
    for so in (T, T + 0x10):
        slen, sp = u32(info, so), selfptr(info, so + 4)
        if sp is not None and sp >= T + 0x54:
            if info[sp + slen] != 0:
                raise TextureError('texture record 0x%x: string not NUL-terminated' % R)
            far = max(far, a4(sp + slen + 1))
    if a16(far) != nxt:
        raise TextureError('texture record 0x%x: exact end 0x%x != next record 0x%x' % (R, a16(far), nxt))
    lw = u32(info, G + 0x24)
    t = dict(rec, gfx=G, texinfo=T, res_w=u32(info, G + 0x0C), res_h=u32(info, G + 0x10), w=u32(info, G + 0x14),
             h=u32(info, G + 0x18), fmt=i32(info, G + 0x1C), descriptor=u32(info, G + 0x20), bpp=info[G + 0x22],
             levels_word=lw, compressed=lw & 1, levels=(lw >> 1) & 0xFF, mip_mode=u32(info, G + 0x28),
             ti_pair4=struct.unpack_from('<II', info, T + 0x4C))
    if selfptr(info, G + 0x08) != R:
        raise TextureError('texture record 0x%x: surface-bits pointer does not resolve to the record' % R)
    if t['fmt'] not in (FMT_A8R8G8B8, FMT_DXT1):
        raise TextureError('texture record 0x%x: format %d is not supported' % (R, t['fmt']))
    if bool(t['compressed']) != (t['fmt'] == FMT_DXT1):
        raise TextureError('texture record 0x%x: DXT sizing bit disagrees with the format' % R)
    if t['mip_mode'] not in (0, 1):
        raise TextureError('texture record 0x%x: mip mode %d' % (R, t['mip_mode']))
    if not (is_pow2(t['w']) and is_pow2(t['h'])):
        raise TextureError('texture record 0x%x: surface %dx%d not a power of two' % (R, t['w'], t['h']))
    want = full_chain(t['w'], t['h']) if t['mip_mode'] == 1 else 1
    if t['levels'] != want:
        raise TextureError('texture record 0x%x: %d levels with mip mode %d' % (R, t['levels'], t['mip_mode']))
    t['engine_size'] = engine_data_size(t['w'], t['h'], t['levels'], t['compressed'], t['bpp'])
    if len(block.payloads[rec['index']]) != t['engine_size']:
        raise TextureError('texture %s: payload %d != engine size %d' % (rec['name'], len(block.payloads[rec['index']]),
                                                                         t['engine_size']))
    t['fmt_name'] = FMT_NAME[t['fmt']]
    return t


def textures(block):
    """{lower-case resource name: texture record} for every texture of the block."""
    out = {}
    for r in records(block):
        if r['type'] == 0:
            out[r['name'].lower()] = texture_record(block, r)
    return out


def texture_patches(rec, new_fmt, new_w, new_h, new_levels):
    """Same-length INFO patches [(offset, bytes)] that make the record describe the new texture."""
    G, R = rec['gfx'], rec['offset']
    w, h = rec['w'], rec['h']
    if not (is_pow2(new_w) and is_pow2(new_h)) or new_w > MAX_DIM or new_h > MAX_DIM:
        raise TextureError('%s: new size %dx%d must be powers of two up to %d' % (rec['name'], new_w, new_h, MAX_DIM))
    if new_w * h != new_h * w:
        raise TextureError('%s: shape changed (%dx%d -> %dx%d)' % (rec['name'], w, h, new_w, new_h))
    new_code = FMT_CODE[new_fmt]
    if new_code == FMT_DXT1 and (new_w < 4 or new_h < 4):
        raise TextureError('%s: DXT1 needs at least 4x4' % rec['name'])
    if rec['fmt'] == FMT_DXT1 and new_code != FMT_DXT1:
        raise TextureError('%s: DXT1 textures stay DXT1' % rec['name'])
    want = full_chain(new_w, new_h) if rec['mip_mode'] == 1 else 1
    if new_levels != want:
        raise TextureError('%s: %d levels, the record\'s mip mode needs %d' % (rec['name'], new_levels, want))
    res_w, res_h = rec['res_w'] * new_w // w, rec['res_h'] * new_h // h
    if res_w * w != rec['res_w'] * new_w or res_h * h != rec['res_h'] * new_h:
        raise TextureError('%s: resolution does not scale to whole pixels' % rec['name'])
    lw = (rec['levels_word'] & ~0x1FF) | (new_levels << 1) | (1 if new_code == FMT_DXT1 else 0)
    patches = [(G + 0x0C, struct.pack('<II', res_w, res_h)), (G + 0x14, struct.pack('<II', new_w, new_h)),
               (G + 0x24, struct.pack('<I', lw)), (G + 0x28, struct.pack('<I', 1 if new_levels > 1 else 0))]
    if new_code != rec['fmt']:
        patches.append((G + 0x1C, struct.pack('<i', new_code)))
        patches.append((G + 0x20, struct.pack('<I', DESCRIPTOR[new_code])))
    if rec['ti_pair4'] == (rec['res_w'], rec['res_h']):
        patches.append((rec['texinfo'] + 0x4C, struct.pack('<II', res_w, res_h)))
    for off, data in patches:
        if not (R <= off and off + len(data) <= rec['end']):
            raise TextureError('%s: patch at 0x%x outside its record' % (rec['name'], off))
    return patches


def check_replacement(rec, fmt, w, h, levels, payload):
    """Raise TextureError when this replacement would break a rule; returns the patches otherwise."""
    want = payload_size(fmt, w, h, levels)
    if len(payload) != want:
        raise TextureError('%s: payload %d bytes, the engine will copy %d' % (rec['name'], len(payload), want))
    if len(payload) > TEXTURE_CAP:
        raise TextureError('%s: %d bytes is over the per-texture cap %d' % (rec['name'], len(payload), TEXTURE_CAP))
    return texture_patches(rec, fmt, w, h, levels)


def rewrite_block(data, ext, replacements):
    """replacements: {resource index: (fmt 'RGBA32'|'DXT1', w, h, levels, payload)} -> (new bytes, report).
    Refuses on any rule; re-parses the result and checks that nothing else changed."""
    old = tb.Block(data, ext)
    new = tb.Block(data, ext)
    by_index = {}
    for r in records(old):
        if r['type'] == 0 and r['index'] in replacements:
            by_index[r['index']] = texture_record(old, r)
    touched = []
    for idx, (fmt, w, h, levels, payload) in replacements.items():
        rec = by_index.get(idx)
        if rec is None:
            raise TextureError('resource %d is not a texture' % idx)
        for off, patch in check_replacement(rec, fmt, w, h, levels, payload):
            new.info[off:off + len(patch)] = patch
            touched.append((off, off + len(patch)))
        new.payloads[idx] = bytes(payload)
    out = new.serialize()

    chk = tb.Block(out, ext)
    problems = []
    if chk.prefix != old.prefix or (chk.timestamp, chk.prop_ck, chk.cmd_ck) != (old.timestamp, old.prop_ck, old.cmd_ck):
        problems.append('prefix, timestamp or checksums changed')
    if len(chk.info) != len(old.info) or len(chk.payloads) != len(old.payloads):
        problems.append('info size or resource count changed')
    if chk.max_asset > max(old.max_asset, TEXTURE_CAP):
        problems.append('maximumAssetSize %d over the cap' % chk.max_asset)
    for k, (a, b) in enumerate(zip(old.payloads, chk.payloads)):
        if k not in replacements and a != b:
            problems.append('payload %d changed' % k)
    for k in range(len(old.info)):
        if old.info[k] != chk.info[k] and not any(lo <= k < hi for lo, hi in touched):
            problems.append('info byte 0x%x changed outside the patched fields' % k)
            break
    recs = records(chk)
    if [r['name'] for r in recs] != [r['name'] for r in records(old)]:
        problems.append('record names changed')
    for idx, (fmt, w, h, levels, payload) in replacements.items():
        r = texture_record(chk, recs[idx])
        if (r['fmt_name'], r['w'], r['h'], r['levels']) != (fmt, w, h, levels) or chk.payloads[idx] != payload:
            problems.append('%s did not read back as written' % r['name'])
    if problems:
        raise TextureError('verification failed: ' + '; '.join(problems[:5]))
    return out, dict(textures=len(replacements), max_asset_before=old.max_asset, max_asset_after=chk.max_asset,
                     bytes_before=len(data), bytes_after=len(out))


def decode_rgba_level0(rec, payload):
    """Level 0 of an A8R8G8B8 texture as (w, h, RGBA bytes, row-major, top row first)."""
    if rec['fmt'] != FMT_A8R8G8B8:
        raise TextureError('%s: only A8R8G8B8 is decoded here' % rec['name'])
    w, h = rec['w'], rec['h']
    src = payload[:w * h * 4]
    out = bytearray(len(src))
    out[0::4], out[1::4], out[2::4], out[3::4] = src[2::4], src[1::4], src[0::4], src[3::4]
    return w, h, bytes(out)
