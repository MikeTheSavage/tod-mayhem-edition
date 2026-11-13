"""skeletons.py - bind skeletons of the Suave characters from the GOG blocks.naz (read only).

  python skeletons.py            -> ../clips/skel_<id>.json  (ram, soldier, civil, dea_taskforce)
Format like tools/anim_replace/clips/ram_skeleton.json: pivots {i, name, parent (pivot+0x58), pos, quat_xyzw}.
Searches overdose.main first, then every .mission/.submap block until each model is found.
"""
import json, os, struct, sys
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import todblock as tb
import tod_model_extract as me

OUT = os.path.join(os.path.dirname(HERE), "clips")
WANT = {"ram": "/characters/ram/ram.model", "soldier": "/characters/military/soldier.model",
        "civil": "/characters/civil/civil.model", "dea_taskforce": "/characters/dea/dea_taskforce.model"}


def dump(info, b, n):
    model = me.extract_model(info, b, n)
    pp = me.deref(info, b + 0x30)
    out = []
    for i, p in enumerate(model["pivots"]):
        parent = struct.unpack_from("<i", info, pp + i * 0x7C + 0x58)[0]
        out.append({"i": i, "name": p["name"], "parent": parent, "pos": p["pos"], "quat_xyzw": p["quat"]})
    return out


def main():
    arch = tb.open_naz()
    names = [e[2] if isinstance(e, tuple) else e for e in (v for v in arch.table.values())]
    names = [n for n in names if n.endswith((".main", ".mission", ".submap"))]
    names.sort(key=lambda n: (not n.endswith("overdose.main"), n))
    todo = dict(WANT)
    for bn in names:
        if not todo:
            break
        data = arch.read(bn)
        infosize = struct.unpack_from("<I", data, 16)[0]
        info = data[24:24 + infosize]
        for b, n in me.find_model_records(info):
            for k, want in list(todo.items()):
                if n.lower().endswith(want.lower()):
                    piv = dump(info, b, n)
                    json.dump({"model": n, "block": bn, "pivots": piv}, open(os.path.join(OUT, "skel_%s.json" % k), "w"), indent=0)
                    print(k, n, bn, len(piv), "pivots")
                    del todo[k]
    if todo:
        print("NOT FOUND:", todo)


if __name__ == "__main__":
    main()


