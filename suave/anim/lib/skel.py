"""skel.py - Ram's skeleton + forward kinematics in the engine's own convention.

  python skel.py export          -> ../clips/ram_skeleton.json (from ram.model in overdose.main, read only)
  python skel.py feet CLIP.json  -> foot/toe positions per frame in clip space (checks foot planting)

Convention (docs/animation_format.md + memory: parents at pivot+0x58, every quaternion conjugated):
  local_b  = T(p_b) * R(conj(q_b))   q_b engine raw (w,x,y,z in clip JSON / x,y,z,w in the model), p_b clip pos
                                     if the clip has a position track, else the bind position
  world_b  = world_parent * local_b  (column vectors, standard Hamilton quaternions)
A clip channel's rotation REPLACES the bind rotation; bones without a channel keep the bind rotation.
"""
import json, math, os, sys
import numpy as np

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
CLIPS = os.path.join(os.path.dirname(HERE), "clips")
SKEL = os.path.join(CLIPS, "ram_skeleton.json")


def export():
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "codec"))
    sys.path.insert(0, os.environ.get("TOD_PROJECT_TOOLS", ""))  # the author's model tools (not included)
    import struct
    import todblock as tb
    import tod_model_extract as me
    data = tb.open_naz(os.environ.get("TOD_GAME", r"C:\GOG Games\Total Overdose")).read("data_pc/overdose_the_game/overdose/shared/overdose.main")
    infosize = struct.unpack_from("<I", data, 16)[0]
    info = data[24:24 + infosize]
    recs = [(b, n) for b, n in me.find_model_records(info) if n.lower().endswith("/ram.model")]
    b, n = recs[0]
    model = me.extract_model(info, b, n)
    pp = me.deref(info, b + 0x30)
    out = []
    for i, p in enumerate(model["pivots"]):
        parent = struct.unpack_from("<i", info, pp + i * 0x7C + 0x58)[0]
        out.append({"i": i, "name": p["name"], "parent": parent, "pos": p["pos"], "quat_xyzw": p["quat"]})
    os.makedirs(CLIPS, exist_ok=True)
    json.dump({"model": n, "pivots": out}, open(SKEL, "w"), indent=0)
    print(n, len(out), "pivots ->", SKEL)
    for p in out:
        print(p["i"], p["name"], p["parent"], [round(v, 3) for v in p["pos"]])


def qmat(w, x, y, z):
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def load_skel():
    return json.load(open(SKEL))["pivots"]


def clip_track(c, key, F):
    t = c.get(key)
    if t is None:
        return None
    if "static" in t:
        return [t["static"]] * F
    return [t["anim"][min(i, len(t["anim"]) - 1)] for i in range(F)]


def fk(clip, skel=None):
    """returns dict name -> (F,4,4) world matrices in clip space, GamePivot moving"""
    skel = skel or load_skel()
    F = int(clip["frames"])
    by = {c["name"]: c for c in clip["channels"]}
    order, done = [], set()
    while len(order) < len(skel):
        for p in skel:
            if p["i"] in done:
                continue
            if p["parent"] < 0 or p["parent"] in done or p["parent"] == p["i"]:
                order.append(p); done.add(p["i"])
    W = {}
    for p in order:
        c = by.get(p["name"])
        pos = clip_track(c, "pos", F) if c else None
        rot = clip_track(c, "ori", F) if c else None
        bq = p["quat_xyzw"]
        mats = np.zeros((F, 4, 4))
        for f in range(F):
            if rot:
                w, x, y, z = rot[f]
            else:
                x, y, z, w = bq
            L = np.eye(4)
            L[:3, :3] = qmat(w, -x, -y, -z)          # conjugate
            L[:3, 3] = pos[f] if pos else p["pos"]
            par = p["parent"]
            mats[f] = (W[skel[par]["name"]][f] @ L) if (par >= 0 and par != p["i"]) else L
        W[p["name"]] = mats
    return W


def feet(path):
    clip = json.load(open(path))
    W = fk(clip)
    F = clip["frames"]
    gp = W["GamePivot"][:, :3, 3]
    print("GamePivot z per frame:", np.round(gp[:, 2], 3).tolist())
    for b in ("L Foot", "R Foot", "L Toe0", "R Toe0"):
        P = W[b][:, :3, 3]
        print(b, "y:", np.round(P[:, 1], 3).tolist())
        print(b, "z:", np.round(P[:, 2], 3).tolist())
        print(b, "x:", np.round(P[:, 0], 3).tolist())


if __name__ == "__main__":
    if sys.argv[1] == "export":
        export()
    elif sys.argv[1] == "feet":
        feet(sys.argv[2])
