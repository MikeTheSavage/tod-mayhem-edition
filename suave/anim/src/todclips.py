"""todclips.py - read the game's animation clips (GOG blocks.naz, read only) as clipjson dicts.

  python todclips.py list [substr]          names of the ANIMATION records
  python todclips.py export NAME OUT.json   one clip (clipjson format, engine quaternions)
"""
import json, os, struct, sys
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import todblock as tb
import animcodec as ac
import clipjson as cj

OD = "data_pc/overdose_the_game/overdose/shared/overdose.main"
CACHE = os.path.join(os.path.dirname(HERE), "work", "overdose.main.gog")


def block_bytes():
    if os.path.exists(CACHE):
        return open(CACHE, "rb").read()
    data = tb.open_naz().read(OD)
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    open(CACHE, "wb").write(data)
    return data


def anim_records(data=None):
    data = data or block_bytes()
    S = struct.unpack_from("<I", data, 16)[0]
    N = struct.unpack_from("<I", data, 12)[0]
    info = data[24:24 + S]
    out = {}
    for R in tb.record_starts(info, N):
        if struct.unpack_from("<I", info, R)[0] != 9:
            continue
        h = ac.decode_record(info, R)
        out[h["name"]] = (R, h)
    return out


def get_clip(name, recs=None):
    recs = recs or anim_records()
    for n, (R, h) in recs.items():
        if n.lower().endswith(name.lower()) or os.path.basename(n).lower() == name.lower() + ".animation":
            return cj.record_to_clip(h)
    raise KeyError(name)


if __name__ == "__main__":
    if sys.argv[1] == "list":
        sub = sys.argv[2].lower() if len(sys.argv) > 2 else ""
        for n, (R, h) in anim_records().items():
            if sub in n.lower():
                print(n, h["frames"])
    elif sys.argv[1] == "export":
        json.dump(get_clip(sys.argv[2]), open(sys.argv[3], "w"))
