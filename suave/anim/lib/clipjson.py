"""Clip <-> JSON interchange for the anim codec, plus quaternion helpers and the arm-swing example modifier.

JSON clip (one file per clip):
  {"name": "/data/characters/ram/ram_run.animation", "frames": 17, "fps": 25.0, "field_1c": 24, "loop_mode": 0,
   "events": [{"frame": 6, "f14": 1, "values": [1474]}...],   # List_1: per-frame event lists (frame index, event ids)
   "channels": [{"name": "Bip", "builtin": 1,                    # builtin = index in the Biped table, or null
                 "pos": null | {"static": [x,y,z]} | {"anim": [[x,y,z], ...frames]},
                 "ori": null | {"static": [w,x,y,z]} | {"anim": [[w,x,y,z], ...frames]}}, ...]}
  Quaternions are the ENGINE's raw values (w,x,y,z order, as stored; dequantised /32767). They are NOT conjugated:
  to get a standard rotation (glTF/UE convention) conjugate them (negate x,y,z).
  Positions are model units (metres), float32. Channel order = keyframe entry order; channels bind to model pivots
  by NAME (FUN_009032d0), so order does not matter to the engine, but new channels must name real pivots.
"""
import json
import math
import struct

import animcodec as ac

F32 = struct.Struct("<f")


def f32(x):
    return F32.unpack(F32.pack(x))[0]


def record_to_clip(h):
    ch = []
    for c in h["chans"]:
        d = {"name": c["name"], "builtin": c["builtin"], "static_flags": c["static_flags"]}
        for key in ("pos", "ori"):
            t = c[key]
            if t is None:
                d[key] = None
            elif key == "pos":
                d[key] = {t[0]: list(t[1]) if t[0] == "static" else [list(p) for p in t[1]]}
            else:
                conv = lambda q: [v / ac.QSCALE for v in q]
                d[key] = {t[0]: conv(t[1]) if t[0] == "static" else [conv(q) for q in t[1]]}
        ch.append(d)
    return {"name": h["name"], "frames": struct.unpack("<i", struct.pack("<I", h["frames"]))[0],
            "fps": F32.unpack(struct.pack("<I", h["f20"]))[0], "field_1c": h["f1c"], "loop_mode": h["loop"],
            "events": [{"frame": e["kind"], "f14": e["f14"], "values": e["values"]} for e in h["events"]],
            "fov": [F32.unpack(struct.pack("<I", v))[0] for v in h["fov"]],
            "channels": ch}


def clip_to_record(clip, template):
    """JSON clip -> record dict for animcodec.encode_record. template: a decoded record (decode_record) supplying the
    fields the clip does not carry (global id, flags, list flag words)."""
    h = dict(template)
    h["name"] = clip["name"]
    h["frames"] = struct.unpack("<I", struct.pack("<i", int(clip["frames"])))[0]
    h["f20"] = struct.unpack("<I", F32.pack(float(clip["fps"])))[0]
    h["f1c"] = int(clip.get("field_1c", template["f1c"]))
    h["loop"] = int(clip.get("loop_mode", template["loop"]))
    h["events"] = [{"kind": e.get("frame", e.get("kind")), "f14": e.get("f14", 1), "values": list(e["values"])}
                   for e in clip["events"]]
    fr = int(clip["frames"])
    ks = [e["kind"] for e in h["events"]]
    if any(not 0 <= k < fr for k in ks) or ks != sorted(set(ks)):
        raise ValueError("event frames must be unique, ascending and < frames: %s" % ks)
    h["fov"] = [struct.unpack("<I", F32.pack(v))[0] for v in clip.get("fov", [])]
    h["lists_meta"] = [(len(h["events"]), len(h["events"]), ac.VEC_FLAGS), (len(h["fov"]), len(h["fov"]), ac.VEC_FLAGS),
                       template["lists_meta"][2]]
    chans = []
    for c in clip["channels"]:
        d = {"name": c["name"], "builtin": c.get("builtin")}
        if d["builtin"] is None and c["name"] in ac.BIPED:
            d["builtin"] = ac.BIPED.index(c["name"])
        if "static_flags" in c:
            d["static_flags"] = c["static_flags"]
        p = c.get("pos")
        if p is None:
            d["pos"] = None
        elif "static" in p:
            d["pos"] = ("static", tuple(f32(v) for v in p["static"]))
        else:
            d["pos"] = ("anim", [tuple(f32(v) for v in q) for q in p["anim"]])
        o = c.get("ori")
        if o is None:
            d["ori"] = None
        elif "static" in o:
            d["ori"] = ("static", quant(o["static"]))
        else:
            prev = None
            seq = []
            for q in o["anim"]:
                qi = quant(q, prev)
                seq.append(qi)
                prev = qi
            d["ori"] = ("anim", seq)
        chans.append(d)
    h["chans"] = chans
    return h


def quant(q, ref=None):
    """float (w,x,y,z) -> int16. Exact inverse of /32767 for values that came from int16 (no renormalisation when the
    input is already within 1e-6 of a dequantised int16 quaternion); otherwise normalised then rounded, keeping the
    hemisphere of ref (the previous frame) so interpolation never takes the long way round."""
    r = [v * ac.QSCALE for v in q]
    if all(abs(x - round(x)) < 1e-3 for x in r):
        return tuple(int(round(x)) for x in r)
    return ac.q_to_i16(q, ref)


# ------------------------------------------------------------------------------------------------ quaternion maths
# Plain Hamilton algebra on (w,x,y,z) tuples. The arm-swing modifier only uses expressions of the form
# m * (m^-1 q)^s, which equal (q m^-1)^s * m, so the result does not depend on the engine's transposed convention.

def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw)


def qconj(a):
    return (a[0], -a[1], -a[2], -a[3])


def qnorm(a):
    m = math.sqrt(sum(v * v for v in a))
    return tuple(v / m for v in a)


def qpow(a, s):
    a = qnorm(a)
    if a[0] < 0:
        a = tuple(-v for v in a)
    w = max(-1.0, min(1.0, a[0]))
    ang = math.acos(w)
    sn = math.sin(ang)
    if sn < 1e-12:
        return (1.0, 0.0, 0.0, 0.0)
    ax = (a[1] / sn, a[2] / sn, a[3] / sn)
    na = ang * s
    return (math.cos(na), ax[0] * math.sin(na), ax[1] * math.sin(na), ax[2] * math.sin(na))


def qangle(a, b):
    """rotation angle (degrees) between two unit quaternions"""
    d = abs(sum(x * y for x, y in zip(qnorm(a), qnorm(b))))
    return math.degrees(2 * math.acos(min(1.0, d)))


def qmean(qs):
    ref = qs[0]
    acc = [0.0] * 4
    for q in qs:
        s = 1 if sum(x * y for x, y in zip(q, ref)) >= 0 else -1
        for i in range(4):
            acc[i] += s * q[i]
    return qnorm(acc)


def scale_swing(clip, bones, factor):
    """Scale each listed bone's rotation away from its cycle-mean orientation by factor (in place on a JSON clip).
    Returns {bone: (peak deviation before, after intended)} in degrees."""
    report = {}
    for c in clip["channels"]:
        if c["name"] not in bones or not c["ori"] or "anim" not in c["ori"]:
            continue
        qs = [qnorm(q) for q in c["ori"]["anim"]]
        m = qmean(qs)
        new = []
        for q in qs:
            dev = qmul(qconj(m), q)
            n = qnorm(qmul(m, qpow(dev, factor)))
            if sum(x * y for x, y in zip(n, q)) < 0:
                n = tuple(-v for v in n)
            new.append(list(n))
        before = max(qangle(m, q) for q in qs)
        after = max(qangle(m, q) for q in new)
        c["ori"]["anim"] = new
        c.pop("static_flags", None)
        report[c["name"]] = (round(before, 3), round(after, 3))
    return report


def save(clip, path):
    with open(path, "w") as f:
        json.dump(clip, f, indent=0)


def load(path):
    with open(path) as f:
        return json.load(f)
