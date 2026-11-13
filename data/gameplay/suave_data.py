"""The `suave` data option: what the Suave loco move (power-up slot 11, TODGameplay.dll [suave]) needs in
the game's data. Everything is appended or set; no create record changes (the save rule of builder.verify).

  set ops (gen_suave, verified by the builder like every option):
    TextureDict HUDTextures/Goodies/Goodie_Tequilla.texture -> /Textures/Hud/OneShot_TimeExtend.bmp   (HUD icon:
        STAND-IN - a distinct icon no reachable power-up shows: the Time Stop goodie's; Tequila shared the small
        health icon. A real Suave icon needs a new texture record, not built)
    level on_start2: PlayerStatusCtrl Default_Tequillas 0 -> 2 (2 Suave charges on a NEW game; maximum stays 2)
  block step (apply_uk, after the set options; overdose_uk.main):
    uk_HUD.txt #28 HUD_Power_up_tequilla  -> "SUAVE\\n..." (the name the power-up selector shows, first line)
    uk_HUD.txt #231 HUD_Power_up_tequilla_Name -> "Suave"
    + one STREAMEDSOUNDINFO record per music segment (copy of Mariachi_02.stream, renamed Suave_Mus_<k>, the
      segment's exact sample count / rate / channels; appended after the last record, payload size 0)
  raw zip entries: data/sounds/streamedpowermoves/suave_mus_<k>.ogg (built by suave/audio/song_import.py from the
    player's own copy of the song, or by make_segments.py from the mp3; the manifest's sha256 and sample counts are
    checked here, the oggs are re-read with ogginfo). Option suave_music = off: neither the records nor the oggs.

The DLL points SoundDict entry 110 (El Mariachi's SoundSlot) at /Sounds/StreamedPowerMoves/Suave_Mus_<k>.wav for the
move and back afterwards; no SoundSlot is added (SoundDict creates would shift later overdose.main nodes = saves).
"""
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), 'lib')
if LIB not in sys.path:
    sys.path.insert(0, LIB)
import todblock as tb   # noqa: E402
import texlib as tl     # noqa: E402
import ogginfo          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
UK = 'data_pc/overdose_the_game/overdose/uk/overdose_uk.main'
MANIFEST = os.path.join(ROOT, 'suave', 'audio', 'build', 'manifest.json')
HUD_TXT = '/data/Localize/Localization_HUD/uk_HUD.txt'
TEXTS = {28: ('HUD_Power_up_tequilla', 'STRONG TEQUILLA', 'SUAVE\\nDance them close. Leave them a little gift.'),
         231: ('HUD_Power_up_tequilla_Name', 'Strong Tequilla', 'Suave')}
# level on_slot10 (in-game test on the Pinata's slot; DLL [suave] slot = 10): HUD_power_up.Menu_Power_up_pinata = 27
TEXTS_SLOT10 = {27: ('HUD_Power_up_pinata', 'PINATA', 'SUAVE\\nDance them close. Leave them a little gift.'),
                230: ('HUD_Power_up_pinata_Name', 'Pinata', 'Suave')}
TEMPLATE = '/data/Sounds/StreamedPowerMoves/Mariachi_02.stream'
ICON_ENTITY = 'HUDTextures/Goodies/Goodie_Tequilla'
ICON_OLD = '/Textures/Hud/OneShot_Health_Small.bmp'
ICON_NEW = '/Textures/Hud/OneShot_TimeExtend.bmp'


class SuaveDataError(Exception):
    pass


def sha(b):
    return hashlib.sha256(b).hexdigest()


def load_manifest(path=None):
    """path: a manifest.json of suave/audio/make_segments.py or song_import.py (default: suave/audio/build)."""
    path = path or MANIFEST
    if not os.path.exists(path):
        raise SuaveDataError('no music build at %s: run suave/audio/song_import.py with your copy of the song first '
                             '(or suave/audio/make_segments.py)' % path)
    with open(path) as f:
        man = json.load(f)
    segs = man.get('segments') or []
    if not segs:
        raise SuaveDataError('the music manifest lists no segment')
    out = []
    for k, s in enumerate(segs, 1):
        p = os.path.join(os.path.dirname(path), s['name'] + '.ogg')
        with open(p, 'rb') as f:
            data = f.read()
        if sha(data) != s['sha256']:
            raise SuaveDataError('%s changed since the manifest was written' % p)
        oi = ogginfo.info(data)
        if (oi['channels'], oi['rate'], oi['samples'], oi['bad_crc_pages'], oi['streams']) != (1, s['rate'], s['samples'], 0, 1):
            raise SuaveDataError('%s: ogg check failed (%s)' % (p, {x: oi[x] for x in ('channels', 'rate', 'samples', 'bad_crc_pages')}))
        if s['name'] != 'suave_mus_%d' % k:
            raise SuaveDataError('segment %d is named %s (expected suave_mus_%d: the DLL builds the path from k)' % (k, s['name'], k))
        out.append(dict(k=k, name=s['name'], samples=s['samples'], rate=s['rate'], data=data,
                        entry='data/sounds/streamedpowermoves/%s.ogg' % s['name'], seconds=s['seconds']))
    return out


# ------------------------------------------------------------------ the stream records
def soundfile_offset(info, R):
    return (R + 0x20 + tb.u32(info, R + 0x20)) & 0xFFFFFFFF


def read_stream(info, R):
    F = soundfile_offset(info, R)
    w = struct.unpack_from('<IIIfIIIIII', info, F)
    slen, srel = struct.unpack_from('<II', info, F + 0x28)
    n0 = F + 0x2C + (srel & ~3)
    e = bytes(info).find(b'\0', n0)
    return dict(tag=w[0], pcm_a=w[1], samples=w[2], rate=w[6], pcm=w[7], fmt=w[8], channels=(w[9] >> 5) & 0x3FFFFFF,
                file=bytes(info[n0:e]).decode('latin-1'), name=tb.record_name(info, R))


def make_stream_record(template, R, k, samples, rate, channels):
    """template = the Mariachi_02.stream record bytes (record start R in its block, the pointers are self-relative)."""
    rec = bytearray(template)
    for old, new in ((b'Mariachi_02', b'Suave_Mus_%d' % k), (b'mariachi_02', b'suave_mus_%d' % k)):
        if len(old) != len(new):
            raise SuaveDataError('segment number %d does not keep the record layout' % k)
        if rec.count(old) != 1:
            raise SuaveDataError('template record holds %r %d times' % (old, rec.count(old)))
        rec = rec.replace(old, new)
    F = soundfile_offset(rec, 0)
    pcm = samples * channels * 2
    struct.pack_into('<II', rec, F + 4, pcm, samples)
    struct.pack_into('<I', rec, F + 0x18, rate)
    struct.pack_into('<I', rec, F + 0x1C, pcm)
    fl = tb.u32(rec, F + 0x24)
    struct.pack_into('<I', rec, F + 0x24, (fl & ~(0x3FFFFFF << 5)) | ((channels & 0x3FFFFFF) << 5))
    return bytes(rec)


def apply_uk(raw, segments, texts=None):
    """overdose_uk.main bytes -> (new bytes, report). Verified: every old record byte-identical except the HUD text
    record, the texts read back, the new stream records read back, the block re-parses with N + len(segments).
    texts: TEXTS (slot 11, default) or TEXTS_SLOT10."""
    texts = TEXTS if texts is None else texts
    b = tb.Block(raw, '.main')
    info = bytes(b.info)
    starts = tb.record_starts(info, b.n)
    ends = starts[1:] + [len(info)]
    names = [tb.record_name(info, R) for R in starts]
    if any('/suave_mus_' in n.lower() for n in names):
        raise SuaveDataError('overdose_uk.main already holds Suave stream records (built on top of a built block?)')
    # 1. HUD texts
    hits = [(i, R, E, t) for i, R, E, t in tl.find_text_records(info, starts) if t.name == HUD_TXT]
    if len(hits) != 1:
        raise SuaveDataError('%s found %d times' % (HUD_TXT, len(hits)))
    ti, R, E, trec = hits[0]
    pairs = trec.pairs()
    text_rep = []
    for o, (key, old_prefix, new) in texts.items():
        k_, v_ = tl.codes_to_str(pairs[o][0]), tl.codes_to_str(pairs[o][1])
        if k_ != key or not v_.startswith(old_prefix):
            raise SuaveDataError('%s #%d is %r = %r, expected %s = %s...' % (HUD_TXT, o, k_, v_, key, old_prefix))
        err, _ = tl.check_entry(pairs[o][0], tl.str_to_codes(new))
        if err:
            raise SuaveDataError('%s #%d: %s' % (HUD_TXT, o, err[0]))
        pairs[o] = (pairs[o][0], tl.str_to_codes(new))
        text_rep.append(dict(ordinal=o, key=key, old=v_, new=new))
    new_text, note = tl.build_record(trec, pairs)
    # 2. stream records
    tk = [i for i, n in enumerate(names) if n == TEMPLATE and tb.u32(info, starts[i]) == 8]
    if len(tk) != 1:
        raise SuaveDataError('%s found %d times' % (TEMPLATE, len(tk)))
    tR = starts[tk[0]]
    template = info[tR:ends[tk[0]]]
    if (len(template) % 16) or b.sizes[tk[0]] != 0:
        raise SuaveDataError('template record is not a 16-aligned, payload-free type-8 record')
    new_recs = [make_stream_record(template, tR, s['k'], s['samples'], s['rate'], 1) for s in segments]
    # 3. rebuild: the text record replaced, the stream records appended (payload size 0 each)
    out = bytearray()
    for k, (R0, E0) in enumerate(zip(starts, ends)):
        out += new_text if k == ti else info[R0:E0]
    app_at = len(out)
    for r in new_recs:
        out += r
    nb = tb.Block(b.serialize(), '.main')
    nb.info = bytearray(out)
    nb.payloads = list(b.payloads) + [b''] * len(new_recs)
    nb.n = b.n + len(new_recs)
    new = nb.serialize()
    # 4. verify
    chk = tb.Block(new, '.main')
    cinfo = bytes(chk.info)
    cst = tb.record_starts(cinfo, chk.n)
    if chk.n != b.n + len(new_recs) or chk.prop_ck != b.prop_ck or chk.cmd_ck != b.cmd_ck or chk.timestamp != b.timestamp:
        raise SuaveDataError('header check failed')
    cends = cst[1:] + [len(cinfo)]
    for k in range(b.n):
        same = cinfo[cst[k]:cends[k]] == info[starts[k]:ends[k]]
        if k == ti:
            if same:
                raise SuaveDataError('the HUD text record did not change')
        elif not same:
            raise SuaveDataError('record %d (%s) changed' % (k, names[k]))
        if chk.payloads[k] != b.payloads[k]:
            raise SuaveDataError('payload %d changed' % k)
    t2 = [t for i, R0, E0, t in tl.find_text_records(cinfo, cst) if t.name == HUD_TXT][0]
    p2 = t2.pairs()
    for o, (key, _, newv) in texts.items():
        if tl.codes_to_str(p2[o][1]) != newv:
            raise SuaveDataError('text #%d reads back %r' % (o, tl.codes_to_str(p2[o][1])))
    for o in range(len(pairs)):
        if o not in texts and p2[o] != trec.pairs()[o]:
            raise SuaveDataError('text #%d changed' % o)
    rec_rep = []
    for j, s in enumerate(segments):
        r = read_stream(cinfo, cst[b.n + j])
        want_name = '/data/Sounds/StreamedPowerMoves/Suave_Mus_%d.stream' % s['k']
        want_file = '/data/sounds/streamedpowermoves/suave_mus_%d.ogg' % s['k']
        if (r['name'], r['file'], r['samples'], r['rate'], r['channels'], r['pcm'], r['pcm_a'], r['fmt']) != \
                (want_name, want_file, s['samples'], s['rate'], 1, s['samples'] * 2, s['samples'] * 2, 1):
            raise SuaveDataError('stream record %d reads back %s' % (s['k'], r))
        rec_rep.append(dict(record=b.n + j, name=r['name'], file=r['file'], samples=r['samples'], rate=r['rate'],
                            seconds=round(r['samples'] / r['rate'], 3)))
    rep = dict(entry=UK, records_before=b.n, records_after=chk.n, info_before=len(info), info_after=len(cinfo),
               appended_at=app_at, text_record=ti, text_note=note, texts=text_rep, stream_records=rec_rep,
               old_records_identical=b.n - 1, payloads_identical=b.n)
    return new, rep


def sound_collisions(fs, entries):
    """name-hash collisions of the new zip entries with the sound archives (the builder checks blocks.naz)."""
    out = []
    for arch in ('sounds.naz', 'uk_sounds.naz'):
        a = fs.archive(arch)
        if a is None:
            continue
        for e in entries:
            hit = a.find('/' + e)
            if hit is not None:
                out.append('%s: %s collides with %s' % (arch, e, hit[2]))
    return out
