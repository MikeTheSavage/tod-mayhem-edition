"""Example edits on Ram's run (ram_run.animation) and their decode checks.

1. JSON round trip: record -> clip JSON -> record is byte-identical.
2. Arm swing x1.6: L/R UpperArm and L/R Forearm rotations scaled away from their cycle mean, renormalised and quantised.
   The encoded clip is decoded again and compared with the intended float quaternions.
3. Longer clip: the run resampled to 2F-1 frames (slerp midpoints) with fps raised so the cycle time is unchanged
   (engine rate = fps / FramesTotal, FUN_009058c0). Used to test block growth.
Writes ../out/ram_run.json, ram_run_armswing.json, ram_run_2x.json and ../out/modify_report.json."""
import copy
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import todblock as tb
import animcodec as ac
import clipjson as cj

OD = "data_pc/overdose_the_game/overdose/shared/overdose.main"
CLIP = "/data/characters/ram/ram_run.animation"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out")
os.makedirs(OUT, exist_ok=True)
ARM = ["L UpperArm", "R UpperArm", "L Forearm", "R Forearm"]
FACTOR = 1.6

b = tb.load_block(tb.open_naz(), OD)
info = b.info
starts = tb.record_starts(info, b.n)
R = next(r for r in starts if tb.u32(info, r) == 9 and tb.record_name(info, r) == CLIP)
k = starts.index(R)
orig = bytes(info[R:starts[k + 1]])
h = ac.decode_record(info, R)
rep = {"clip": CLIP, "frames": h["frames"], "f64": h["f64"], "f66": h["f66"], "channels": len(h["chans"]),
       "events": [(e["kind"], e["values"]) for e in h["events"]], "record_bytes": len(orig)}

# 1. JSON round trip
clip = cj.record_to_clip(h)
cj.save(clip, os.path.join(OUT, "ram_run.json"))
clip2 = cj.load(os.path.join(OUT, "ram_run.json"))
rt = ac.encode_record(cj.clip_to_record(clip2, h))
rep["json_roundtrip_identical"] = rt == orig

# 2. arm swing
mod = copy.deepcopy(clip2)
swing = cj.scale_swing(mod, ARM, FACTOR)
mod["note"] = "ram_run with L/R UpperArm + Forearm swing scaled x%.2f about the cycle mean" % FACTOR
cj.save(mod, os.path.join(OUT, "ram_run_armswing.json"))
rec = ac.encode_record(cj.clip_to_record(mod, h))
buf = bytearray(rec)
h2 = ac.decode_record(buf, 0)
back = cj.record_to_clip(h2)
chk = {"record_bytes": len(rec), "same_size": len(rec) == len(orig)}
unchanged_identical = 0
changed = {}
worst_unit = 0.0
for c0, cm, cb in zip(clip2["channels"], mod["channels"], back["channels"]):
    if cb["ori"] and "anim" in cb["ori"]:
        for q in cb["ori"]["anim"]:
            worst_unit = max(worst_unit, abs(math.sqrt(sum(v * v for v in q)) - 1))
    if c0["name"] not in swing:
        unchanged_identical += (c0["ori"] == cb["ori"] and c0["pos"] == cb["pos"])
        continue
    errs = [cj.qangle(a, b2) for a, b2 in zip(cm["ori"]["anim"], cb["ori"]["anim"])]
    q0 = [cj.qnorm(q) for q in c0["ori"]["anim"]]
    qb = [cj.qnorm(q) for q in cb["ori"]["anim"]]
    m0 = cj.qmean(q0)
    amp0 = [cj.qangle(m0, q) for q in q0]
    ampb = [cj.qangle(m0, q) for q in qb]
    ratio = [b2 / a for a, b2 in zip(amp0, ampb) if a > 0.5]
    changed[c0["name"]] = {"max_angle_err_deg": round(max(errs), 5), "peak_dev_before_deg": round(max(amp0), 2),
                           "peak_dev_after_deg": round(max(ampb), 2),
                           "ratio_min": round(min(ratio), 4), "ratio_max": round(max(ratio), 4)}
chk["unchanged_channels_identical"] = "%d/%d" % (unchanged_identical, len(clip2["channels"]) - len(swing))
chk["changed"] = changed
chk["max_unit_error"] = worst_unit
rep["armswing"] = chk


# 3. longer clip: 2F-1 frames, same cycle time
def lerp(a, b2, t):
    return [x + (y - x) * t for x, y in zip(a, b2)]


def slerp(a, b2, t):
    a, b2 = cj.qnorm(a), cj.qnorm(b2)
    d = sum(x * y for x, y in zip(a, b2))
    if d < 0:
        b2, d = tuple(-v for v in b2), -d
    if d > 0.9995:
        return list(cj.qnorm(lerp(a, b2, t)))
    th = math.acos(d)
    s0, s1 = math.sin((1 - t) * th) / math.sin(th), math.sin(t * th) / math.sin(th)
    return [s0 * x + s1 * y for x, y in zip(a, b2)]


F = clip2["frames"]
F2 = 2 * F - 1
long = copy.deepcopy(clip2)
long["frames"] = F2
long["fps"] = clip2["fps"] * F2 / F
long["note"] = "ram_run resampled to %d frames (slerp midpoints), fps %.4f keeps the cycle time" % (F2, long["fps"])
for e in long["events"]:                       # List_1 entries are frame indices: keep footsteps on the same pose
    e["frame"] = int(round(e["frame"] * (F2 - 1) / (F - 1)))
for c in long["channels"]:
    for key, fn in (("pos", lerp), ("ori", slerp)):
        if c[key] and "anim" in c[key]:
            src = c[key]["anim"]
            c[key]["anim"] = [src[j // 2] if j % 2 == 0 else fn(src[j // 2], src[j // 2 + 1], 0.5) for j in range(F2)]
cj.save(long, os.path.join(OUT, "ram_run_2x.json"))
rec2 = ac.encode_record(cj.clip_to_record(long, h))
h3 = ac.decode_record(bytearray(rec2), 0)
back2 = cj.record_to_clip(h3)
even_ok = all(cb["ori"]["anim"][2 * i] == c0["ori"]["anim"][i]
              for c0, cb in zip(clip2["channels"], back2["channels"]) if c0["ori"] and "anim" in c0["ori"]
              for i in range(F))
rep["longer"] = {"frames": F2, "fps": long["fps"], "event_frames": [e["kind"] for e in h3["events"]],
                 "record_bytes": len(rec2), "growth": len(rec2) - len(orig),
                 "growth_mod16": (len(rec2) - len(orig)) % 16, "even_frames_equal_original": even_ok,
                 "duration_s_before": F / clip2["fps"], "duration_s_after": F2 / long["fps"]}
json.dump(rep, open(os.path.join(OUT, "modify_report.json"), "w"), indent=1)
print(json.dumps(rep, indent=1))
