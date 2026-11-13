"""Round-trip proof: decode every ANIMATION record in overdose.main (GOG blocks.naz) and encode it again.
Checks (1) keyframe block identical, (2) whole record identical incl. padding up to the next record,
(3) a float path (int16 -> float -> round(x*32767)) is lossless, (4) the whole block rebuilt from re-encoded records is
byte-identical to the shipped block. Writes ../roundtrip.json."""
import hashlib
import json
import os
import struct
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import todblock as tb
import animcodec as ac

OD = "data_pc/overdose_the_game/overdose/shared/overdose.main"
t0 = time.time()
arch = tb.open_naz()
raw = arch.read(OD)
b = tb.Block(raw, ".main")
info = b.info
starts = tb.record_starts(info, b.n)
res = Counter()
per_char = Counter()
fails = []
new_info = bytearray()
for k, R in enumerate(starts):
    end = starts[k + 1] if k + 1 < len(starts) else len(info)
    if tb.u32(info, R) != 9:
        new_info += info[R:end]
        continue
    h = ac.decode_record(info, R)
    who = h["name"].split("/")[3] if h["name"].startswith("/data/characters/") else h["name"].split("/")[2]
    # (1) keyframe block
    kf = R + 0x5C + tb.u32(info, R + 0x5C)
    blk0 = bytes(info[kf:kf + 4 * h["f60"]])
    frames = h["frames"]
    blk1, f64, f66 = ac.encode_kf(h["chans"], frames)
    ok1 = blk1 == blk0 and (f64, f66) == (h["f64"], h["f66"])
    # (3) float path: dequantise every quat and requantise with plain rounding; positions through float64
    fchans = []
    for c in h["chans"]:
        c2 = dict(c)
        if c["ori"]:
            kind, v = c["ori"]
            deq = (lambda q: tuple(int(round(x / 32767.0 * 32767.0)) for x in q))
            c2["ori"] = (kind, deq(v)) if kind == "static" else (kind, [deq(q) for q in v])
        if c["pos"]:
            kind, v = c["pos"]
            c2["pos"] = (kind, tuple(float(x) for x in v)) if kind == "static" else (kind, [tuple(float(x) for x in p) for p in v])
        fchans.append(c2)
    blk2, _, _ = ac.encode_kf(fchans, frames)
    ok3 = blk2 == blk0
    # (2) record
    rec1 = ac.encode_record(h)
    ok2 = rec1 == bytes(info[R:end])
    res["clips"] += 1
    res["kf_identical"] += ok1
    res["record_identical"] += ok2
    res["float_path_identical"] += ok3
    per_char[who] += 1
    per_char[who + "_ok"] += ok1 and ok2 and ok3
    if not (ok1 and ok2 and ok3):
        fails.append((h["name"], ok1, ok2, ok3, len(rec1), end - R))
    new_info += rec1
# (4) whole block
nb = tb.Block(raw, ".main")
nb.info = new_info
out = nb.serialize()
res["block_identical"] = int(out == raw)
print(dict(res))
print("ram clips:", per_char["ram"], "ok:", per_char["ram_ok"])
print({k: v for k, v in per_char.items() if not k.endswith("_ok")})
print("fails:", fails[:10])
print("block sha256 %s rebuilt %s  %.1fs" % (hashlib.sha256(raw).hexdigest()[:16], hashlib.sha256(out).hexdigest()[:16],
                                              time.time() - t0))
json.dump({"result": dict(res), "per_character": dict(per_char), "fails": fails,
           "block_sha256": hashlib.sha256(raw).hexdigest(), "rebuilt_sha256": hashlib.sha256(out).hexdigest()},
          open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "roundtrip.json"), "w"), indent=1)
