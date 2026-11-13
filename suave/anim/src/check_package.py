"""check_package.py - independent check of a built data zip with suave_anims=on (reads the zip like the engine).

  python check_package.py ZIP [--base-game "C:\GOG Games\Total Overdose"]
1 the zip's overdose.main is found by the engine's lookup port (todblock.Archive.find);
2 every Suave clip record decodes, re-encodes byte-identically and equals the clip JSON as encoded by the codec;
3 decoded clips: FK on their model skeleton, the loop clips close (frame N-1 == frame 0);
4 every slot PlayerAnims/specials/Suave/<clip> exists, names its clip, and the clip is a record of the block;
5 the block's record count = vanilla + N clips (clips.json) and the AnimDict create list = vanilla's + N + 1 appended (save rule).
"""
import argparse, json, os, struct, sys
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(ROOT)), "data", "lib"))
import todblock as tb
import animcodec as ac
import clipjson as cj
import todfrag as tf
import fragedit as fe
import fragtool as ft
import numpy as np
import retarget as rt
import todclips

OD = "data_pc/overdose_the_game/overdose/shared/overdose.main"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--base-game", default=None, help="game folder whose own lookup gives the base overdose.main "
                    "(e.g. a TOD HD game folder for an hd_override base); default: the GOG blocks.naz cache")
    a = ap.parse_args()
    arc = tb.Archive(a.zip)
    hit = arc.find("/" + OD)
    assert hit, "overdose.main not found by the engine lookup"
    with open(a.zip, "rb") as f:
        f.seek(hit[0]); raw = f.read(hit[1])
    b = tb.Block(raw, ".main")
    if a.base_game:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(ROOT)), "data"))
        from gameplay import gamefs
        van = tb.Block(gamefs.GameFS(a.base_game, skip=("todgameplay_data.zip",)).read(OD)[0], ".main")
    else:
        van = tb.Block(todclips.block_bytes(), ".main")
    info = bytes(b.info)
    starts = tb.record_starts(info, b.n)
    recs = {tb.record_name(info, R).lower(): R for R in starts}
    cat = json.load(open(os.path.join(ROOT, "clips", "game", "clips.json")))
    out = {"records": [van.n, b.n]}
    assert b.n == van.n + len(cat), "record count"
    template = None
    for R in starts:
        if tb.u32(b.info, R) == 9:
            template = ac.decode_record(info, R); break
    clips = {}
    for name, meta in cat.items():
        R = recs[meta["clip"].lower()]
        h = ac.decode_record(info, R)
        rec = ac.encode_record(h)
        assert bytes(info[R:R + len(rec)]) == rec, name
        want = ac.encode_record(cj.clip_to_record(json.load(open(os.path.join(ROOT, "clips", "game", name + ".json"))), template))
        assert rec == want, name + " differs from its JSON"
        c = cj.record_to_clip(h)
        sk = rt.TodSkel(meta["character"] if meta["character"] in ("ram", "soldier", "civil", "dea_taskforce") else "soldier")
        F = c["frames"]
        by = {x["name"]: x for x in c["channels"]}
        L = np.zeros((F, len(sk.names), 3, 3)); pos = np.tile(sk.pos[None], (F, 1, 1))
        for i, nm in enumerate(sk.names):
            x = by.get(nm)
            if x and x["ori"]:
                qs = [x["ori"]["static"]] * F if "static" in x["ori"] else x["ori"]["anim"]
                L[:, i] = np.array([rt.qmat_engine(q) for q in qs])
            else:
                L[:, i] = sk.Lbind[i]
            if x and x["pos"]:
                pos[:, i] = np.array([x["pos"]["static"]] * F if "static" in x["pos"] else x["pos"]["anim"])
        G, P = sk.fk_local(L, pos)
        keep = [i for i in range(len(sk.names)) if sk.names[i] not in ("", "GamePivot")]
        close = float(np.linalg.norm(P[-1, keep] - P[0, keep], axis=1).max())
        if meta["loop_mode"] == 0:
            assert close < 1e-3, name + " does not close"
        clips[name] = {"record": starts.index(R), "frames": F, "bytes": len(rec), "loop_close_mm": round(1000 * close, 3)}
    out["clips"] = clips
    T = ft.types()
    frs, _ = ft.fragments(b)
    # the slots live at the end of GameCamera.fragment (the last fragment instanced in the block)
    # so no existing node changes its creation index (saves); AnimDict.fragment must be untouched.
    k, _ = ft.pick_fragment(frs, "/GameCamera.fragment")
    d = fe.FragDoc(b.payloads[k], T, "GameCamera")
    vfrs, _ = ft.fragments(van)
    vk, _ = ft.pick_fragment(vfrs, "/GameCamera.fragment")
    ka, _ = ft.pick_fragment(frs, "/AnimDict.fragment")
    vka, _ = ft.pick_fragment(vfrs, "/AnimDict.fragment")
    assert b.payloads[ka] == van.payloads[vka], "AnimDict.fragment changed (creates there shift every later node)"
    oc = [(r.node, r.raw) for r in tf.Fragment(van.payloads[vk], T).records if r.op == tf.CREATE]
    nc = [(r.node, r.raw) for r in tf.Fragment(b.payloads[k], T).records if r.op == tf.CREATE]
    assert nc[:len(oc)] == oc and len(nc) == len(oc) + len(cat) + 1, "GameCamera creates not appended-only"
    out["gamecamera_creates"] = [len(oc), len(nc)]
    slots = {}
    for name, meta in cat.items():
        n = d.find("Suave/" + name)
        clip = d.get(n, "target1")
        assert ("/data" + clip).lower() == meta["clip"].lower() and ("/data" + clip).lower() in recs, name
        slots[name] = {"node": "0x%08X" % n, "clip": clip, "cps": d.get(n, "speed"), "loopmode1": d.get(n, "loopmode1")}
    out["slots"] = slots
    print(json.dumps(out, indent=1))
    print("CHECK_PACKAGE PASS")


if __name__ == "__main__":
    main()
