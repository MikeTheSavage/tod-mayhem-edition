"""TOD (Kapow) AnimationAsset codec: decode a compiled ANIMATION record (asset type 9) into plain data and encode it
back. encode(decode(x)) == x byte-for-byte on all 447 shipped clips (see roundtrip.py).

Record layout (all offsets from the record start R; self-relative pointers: target = field + u32, 0 never used here):
  +0x00 u32 type = 9            +0x04 name ptr -> +0x68       +0x08 u32 global id (0)    +0x0C 0xBAADF00D
  +0x10 u64 timestamp (0)       +0x18 u32 flags 0xBA610000
  +0x1C i32 field_1C (24 in every clip)     +0x20 f32 field_20 (25.0 = frames per second in every clip)
  +0x24 i32 LoopMode (0/1)      +0x28 i32 FramesTotal
  +0x2C vector List_1 {ptr, count, cap, 0xBAAFFA00}   24-byte frame events {vector<u32> event ids, i32 frame index,
                                                      i32 1}; entries strictly ascending by frame, frame < FramesTotal
  +0x3C vector List_2 {ptr, count, cap, 0xBAAFFA00}   floats (FOV list; empty in every shipped clip)
  +0x4C vector List_3 {0,   count, cap, 0xBAAFFB00}   empty, null pointer in every shipped clip
  +0x5C ptr keyframe block      +0x60 u32 block size in 4-byte units
  +0x64 u16 f64 = number of ANIMATED position tracks   +0x66 u16 f66 = number of ANIMATED orientation tracks
  +0x68 name string, NUL, 0xAB fill to 4
  keyframe block (f60*4 bytes)
  List_1 entries (24*c1), then each entry's u32 values in entry order, then List_2 floats
  zero pad to 16 (the engine advances to the next record by align16(size pass))

Keyframe block (base B, offsets in 4-byte units from B):
  +0 u32 channel count n; n x 12-byte entries {posOff|0<<24, oriOff|0<<24, nameOff(12 bits) | 0x1000 pos static |
  0x2000 ori static | 0x4000 name is an index into the builtin Biped table @0xA12FF0}
  [inline channel names, NUL-terminated, entry order, zero pad to 4]
  [static positions: 3 x f32 per channel, entry order]
  [static orientations: 4 x i16 (w,x,y,z) per channel, entry order]
  [animated positions: frame-major, f64 tracks x 3 f32 each, entry order]
  [animated orientations: frame-major, f66 tracks x 4 i16 each, entry order]
  posOff/oriOff 0 = channel has no such track (engine: pos (0,0,0), quat identity).
  Engine reads (FUN_00904C30 / FUN_00904CC0): pos at B + 4*(posOff + (static ? 0 : f64*F*3));
  quat at B + 4*(oriOff + (static ? 0 : f66*F*2)), int16 * (1/32767), memory order w,x,y,z.
"""
import math
import struct

BIPED = ["GamePivot", "Bip", "Pelvis", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Head", "Chin", "ChinR", "ChinL",
         "Eyelid", "EyeBR", "EyeBL", "EyeR", "EyeL", "L Clavicle", "L UpperArm", "L Forearm", "L Hand", "L Finger0",
         "L Finger01", "L Finger1", "L Finger11", "L Finger2", "L Finger21", "L ForeTwist", "L ForeTwist1",
         "R Clavicle", "R UpperArm", "R Forearm", "R Hand", "R Finger0", "R Finger01", "R Finger1", "R Finger11",
         "R Finger2", "R Finger21", "R ForeTwist", "R ForeTwist1", "L Thigh", "L Calf", "L Foot", "L Toe0", "R Thigh",
         "R Calf", "R Foot", "R Toe0"]
F_POS_STATIC, F_ORI_STATIC, F_BUILTIN = 0x1000, 0x2000, 0x4000
SENT = 0xBAADF00D
VEC_FLAGS = 0xBAAFFA00
VEC_FLAGS_L3 = 0xBAAFFB00
HEADER = 0x68
QSCALE = 32767.0


class CodecError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def a4(x):
    return (x + 3) & ~3


def a16(x):
    return (x + 15) & ~15


# ------------------------------------------------------------------------------------------------ keyframe block

def decode_kf(blk, frames, f64, f66):
    """-> list of channels {name, builtin, pos, ori}. pos: None | ('static', (x,y,z)) | ('anim', [(x,y,z)]*frames)
    with float32 values; ori: None | ('static', (w,x,y,z) int16) | ('anim', [(w,x,y,z)]*frames) int16 (engine raw)."""
    n = u32(blk, 0)
    chans = []
    for e in range(n):
        p0, p1, p2 = struct.unpack_from("<3I", blk, 4 + 12 * e)
        if p0 >> 24 or p1 >> 24 or p2 & ~0x7FFF:
            raise CodecError("channel %d: unknown flag bits %08x %08x %08x" % (e, p0, p1, p2))
        noff = p2 & 0xFFF
        if p2 & F_BUILTIN:
            name, builtin = BIPED[noff], noff
        else:
            z = blk.index(b"\0", noff)
            name, builtin = blk[noff:z].decode("latin-1"), None
        po, oo = p0 & 0xFFFFFF, p1 & 0xFFFFFF
        pos = ori = None
        if po:
            if p2 & F_POS_STATIC:
                pos = ("static", struct.unpack_from("<3f", blk, 4 * po))
            else:
                pos = ("anim", [struct.unpack_from("<3f", blk, 4 * (po + f64 * F * 3)) for F in range(frames)])
        if oo:
            if p2 & F_ORI_STATIC:
                ori = ("static", struct.unpack_from("<4h", blk, 4 * oo))
            else:
                ori = ("anim", [struct.unpack_from("<4h", blk, 4 * (oo + f66 * F * 2)) for F in range(frames)])
        chans.append({"name": name, "builtin": builtin, "pos": pos, "ori": ori,
                      "name_off": noff, "static_flags": p2 & (F_POS_STATIC | F_ORI_STATIC)})
    return chans


def encode_kf(chans, frames):
    """Inverse of decode_kf. -> (block bytes, f64, f66). Layout in the module docstring.
    Static flag bits are written only for tracks that exist, except where a channel carries 'static_flags' from a
    decode (kept verbatim so an unchanged clip re-encodes byte-identically)."""
    n = len(chans)
    for c in chans:
        for key in ("pos", "ori"):
            t = c[key]
            if t is not None and t[0] == "anim" and len(t[1]) != frames:
                raise CodecError("channel %s: %s track has %d frames, clip has %d" % (c["name"], key, len(t[1]), frames))
    cur = 4 + 12 * n
    names = bytearray()
    name_off = []
    for c in chans:
        if c["builtin"] is not None:
            if BIPED[c["builtin"]] != c["name"]:
                raise CodecError("builtin index %d is %r, not %r" % (c["builtin"], BIPED[c["builtin"]], c["name"]))
            name_off.append(c["builtin"])
        else:
            off = cur + len(names)
            name_off.append(off)
            names += c["name"].encode("latin-1") + b"\0"
    cur = a4(cur + len(names))
    names += b"\0" * (cur - 4 - 12 * n - len(names))
    if cur // 4 > 0xFFFFFF or max([0] + [o for o, c in zip(name_off, chans) if c["builtin"] is None]) > 0xFFF:
        raise CodecError("name offsets exceed 12 bits")
    ps = [i for i, c in enumerate(chans) if c["pos"] and c["pos"][0] == "static"]
    os_ = [i for i, c in enumerate(chans) if c["ori"] and c["ori"][0] == "static"]
    pa = [i for i, c in enumerate(chans) if c["pos"] and c["pos"][0] == "anim"]
    oa = [i for i, c in enumerate(chans) if c["ori"] and c["ori"][0] == "anim"]
    posoff, orioff = [0] * n, [0] * n
    body = bytearray()
    base = cur
    for i in ps:
        posoff[i] = (base + len(body)) // 4
        body += struct.pack("<3f", *chans[i]["pos"][1])
    for i in os_:
        orioff[i] = (base + len(body)) // 4
        body += struct.pack("<4h", *chans[i]["ori"][1])
    pa_base = base + len(body)
    for t, i in enumerate(pa):
        posoff[i] = pa_base // 4 + 3 * t
    for F in range(frames):
        for i in pa:
            body += struct.pack("<3f", *chans[i]["pos"][1][F])
    oa_base = base + len(body)
    for t, i in enumerate(oa):
        orioff[i] = oa_base // 4 + 2 * t
    for F in range(frames):
        for i in oa:
            body += struct.pack("<4h", *chans[i]["ori"][1][F])
    out = bytearray(struct.pack("<I", n))
    for i, c in enumerate(chans):
        flags = c.get("static_flags")
        if flags is None:
            flags = (F_POS_STATIC if i in ps else 0) | (F_ORI_STATIC if i in os_ else 0)
        else:
            # a decoded flag is only meaningful for a present track; keep it, but a static track needs its bit
            flags |= (F_POS_STATIC if i in ps else 0) | (F_ORI_STATIC if i in os_ else 0)
            if i in pa:
                flags &= ~F_POS_STATIC
            if i in oa:
                flags &= ~F_ORI_STATIC
        p2 = name_off[i] | flags | (F_BUILTIN if c["builtin"] is not None else 0)
        out += struct.pack("<3I", posoff[i], orioff[i], p2)
    out += names + body
    if len(pa) > 0xFFFF or len(oa) > 0xFFFF:
        raise CodecError("too many animated tracks")
    if len(out) // 4 > 0xFFFFFF:
        raise CodecError("keyframe block over 2^24 units (64 MiB); offsets would overflow 24 bits")
    return bytes(out), len(pa), len(oa)


# ------------------------------------------------------------------------------------------------ record

def decode_record(info, R):
    """-> dict with every field of the ANIMATION record at info[R]. Raises CodecError on anything unexpected."""
    if u32(info, R) != 9:
        raise CodecError("not an ANIMATION record")
    h = {}
    (h["type"], name_raw, h["gid"], h["sent"], h["ts_lo"], h["ts_hi"], h["flags"], h["f1c"], h["f20"], h["loop"],
     h["frames"]) = struct.unpack_from("<11I", info, R)
    if name_raw != HEADER - 4:
        raise CodecError("name pointer %x" % name_raw)
    z = info.index(b"\0", R + HEADER)
    h["name"] = bytes(info[R + HEADER:z]).decode("latin-1")
    lists = []
    for lo in (0x2C, 0x3C, 0x4C):
        lists.append(struct.unpack_from("<4I", info, R + lo))
    kraw, f60 = struct.unpack_from("<2I", info, R + 0x5C)
    f64, f66 = struct.unpack_from("<2H", info, R + 0x64)
    kf = (R + 0x5C + kraw) & 0xFFFFFFFF
    if kraw & 3:
        raise CodecError("keyframe pointer tag bits %d" % (kraw & 3))
    blk = bytes(info[kf:kf + 4 * f60])
    frames = struct.unpack("<i", struct.pack("<I", h["frames"]))[0]
    h["chans"] = decode_kf(blk, frames, f64, f66)
    h["kf_pre_pad"] = kf - a4(z + 1)            # bytes between the name and the block (0 on all shipped clips)
    # List_1: event tracks
    p1, c1, cap1, fl1 = lists[0]
    events = []
    l1 = R + 0x2C + p1
    for i in range(c1):
        e = l1 + 24 * i
        vp, vc, vcap, vfl, kind, one = struct.unpack_from("<6I", info, e)
        vals = list(struct.unpack_from("<%dI" % vc, info, e + vp)) if vc else []
        events.append({"values": vals, "cap": vcap, "vflags": vfl, "kind": kind, "f14": one, "ptr": vp})
    h["events"] = events
    p2_, c2, cap2, fl2 = lists[1]
    h["fov"] = list(struct.unpack_from("<%dI" % c2, info, R + 0x3C + p2_)) if c2 else []
    h["lists_meta"] = [(c1, cap1, fl1), (c2, cap2, fl2), lists[2]]
    h["f60"], h["f64"], h["f66"] = f60, f64, f66
    return h


def encode_record(h):
    """Inverse of decode_record: the record bytes from R up to align16 of the size-pass end (zero padded)."""
    frames = struct.unpack("<i", struct.pack("<I", h["frames"]))[0]
    blk, f64, f66 = encode_kf(h["chans"], frames)
    name = h["name"].encode("latin-1") + b"\0"
    cur = a4(HEADER + len(name))
    kf_at = cur + h.get("kf_pre_pad", 0)
    cur = kf_at + len(blk)
    ev = h["events"]
    l1_at = cur
    cur += 24 * len(ev)
    val_at = []
    for e in ev:
        val_at.append(cur)
        cur += 4 * len(e["values"])
    l2_at = cur
    cur += 4 * len(h["fov"])
    end = a16(cur)
    out = bytearray(end)
    (c1m, c2m, l3m) = h["lists_meta"]
    struct.pack_into("<11I", out, 0, 9, HEADER - 4, h["gid"], h["sent"], h["ts_lo"], h["ts_hi"], h["flags"], h["f1c"],
                     h["f20"], h["loop"], h["frames"])
    struct.pack_into("<4I", out, 0x2C, l1_at - 0x2C, len(ev), len(ev) if c1m[1] == c1m[0] else c1m[1], c1m[2])
    struct.pack_into("<4I", out, 0x3C, l2_at - 0x3C, len(h["fov"]), len(h["fov"]) if c2m[1] == c2m[0] else c2m[1], c2m[2])
    struct.pack_into("<4I", out, 0x4C, *l3m)
    struct.pack_into("<2I2H", out, 0x5C, kf_at - 0x5C, len(blk) // 4, f64, f66)
    out[HEADER:HEADER + len(name)] = name
    out[HEADER + len(name):kf_at] = b"\xab" * (kf_at - HEADER - len(name))   # the tool chain's 0xAB fill
    out[kf_at:kf_at + len(blk)] = blk
    for i, e in enumerate(ev):
        at = l1_at + 24 * i
        cap = len(e["values"]) if e.get("cap", len(e["values"])) == len(e["values"]) else e["cap"]
        struct.pack_into("<6I", out, at, val_at[i] - at, len(e["values"]), cap, e.get("vflags", VEC_FLAGS),
                         e["kind"], e["f14"])
        if e["values"]:
            struct.pack_into("<%dI" % len(e["values"]), out, val_at[i], *e["values"])
    if h["fov"]:
        struct.pack_into("<%dI" % len(h["fov"]), out, l2_at, *h["fov"])
    return bytes(out)


def record_size_pass(info, R):
    """Where the engine's size pass for this record ends (unaligned): the furthest object."""
    h = decode_record(info, R)
    return len(encode_record(h))


# ------------------------------------------------------------------------------------------------ float helpers

def q_from_i16(q):
    """engine int16 (w,x,y,z) -> float (w,x,y,z)"""
    return tuple(v / QSCALE for v in q)


def q_to_i16(q, ref=None):
    """float (w,x,y,z) -> normalised, quantised int16 (w,x,y,z). ref: int16 quat whose hemisphere to keep."""
    w, x, y, z = q
    m = math.sqrt(w * w + x * x + y * y + z * z)
    if m == 0:
        raise CodecError("zero quaternion")
    w, x, y, z = w / m, x / m, y / m, z / m
    if ref is not None and (w * ref[0] + x * ref[1] + y * ref[2] + z * ref[3]) < 0:
        w, x, y, z = -w, -x, -y, -z
    return tuple(max(-32767, min(32767, int(round(v * QSCALE)))) for v in (w, x, y, z))
