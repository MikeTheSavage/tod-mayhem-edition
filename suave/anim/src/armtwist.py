"""armtwist.py - forearm twist / elbow measurements.

  python armtwist.py survey            vanilla game clips: how the roll is split Forearm -> ForeTwist -> ForeTwist1 -> Hand
  python armtwist.py weights           skin weights of the forearm region per model (stage glbs, read only)
  python armtwist.py npz FILE SKEL...  metrics of a built pair / circle npz (keys <role>_G or G)

Definitions (all per arm S = L / R, angles in degrees, bind-relative):
  elbow_twist  = twist of the Forearm's local rotation (vs its bind local) about the forearm axis: the roll the elbow
                 itself carries; a hinge has none.
  hand_twist   = twist of the Hand relative to the Forearm (bind-relative) about the forearm axis = the forearm roll
                 (pronation / supination) the twist bones must spread.
  ft_twist / ft1_twist = twist of ForeTwist / ForeTwist1 relative to the Forearm, same axis.
  bend         = elbow flexion angle; bend_axis = the flexion axis in the UpperArm frame (sign flips = elbow flips).
  candy        = the largest twist difference between two skinned neighbours along the forearm (elbow: UpperArm vs
                 the forearm segment weights, mid: ForeTwist vs ForeTwist1, wrist: ForeTwist1 vs Hand) = what the
                 skin has to absorb (candy-wrapping shows above ~90 degrees).
"""
import json, os, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "lib"))
import retarget as rt


def twist_angle(D, axis):
    """twist (rad) of rotation matrices D (F,3,3) about the unit axis (3,) (swing-twist decomposition)"""
    q = Rot.from_matrix(D).as_quat()                       # x y z w
    return 2.0 * np.arctan2(q[:, :3] @ axis, q[:, 3])


def wrap(a):
    return (a + np.pi) % (2 * np.pi) - np.pi


def clip_world(sk, clip):
    """clipjson dict -> world rotations G (F,n,3,3), positions P (F,n,3) on skeleton sk"""
    F = clip["frames"]
    by = {c["name"]: c for c in clip["channels"]}
    L = np.zeros((F, len(sk.names), 3, 3)); pos = np.tile(sk.pos[None], (F, 1, 1))
    for i, nm in enumerate(sk.names):
        c = by.get(nm)
        if c and c.get("ori"):
            o = c["ori"]
            qs = [o["static"]] * F if "static" in o else o["anim"]
            L[:, i] = np.array([rt.qmat_engine(q) for q in qs])
        else:
            L[:, i] = sk.Lbind[i]
        if c and c.get("pos"):
            p = c["pos"]
            pos[:, i] = np.array([p["static"]] * F if "static" in p else p["anim"])
    return sk.fk_local(L, pos)


def fa_axis(sk, S):
    fa, ha = sk.idx[S + " Forearm"], sk.idx[S + " Hand"]
    a = sk.Pb[ha] - sk.Pb[fa]
    a = sk.Gb[fa].T @ a
    return a / np.linalg.norm(a)


def rel_delta(sk, G, a, b):
    """rotation (forearm-local) of bone b relative to bone a, vs the bind relation: (Ga^T Gb)(Gba^T Gbb)^T"""
    ia, ib = sk.idx[a], sk.idx[b]
    R = np.einsum("fba,fbc->fac", G[:, ia], G[:, ib])
    return R @ (sk.Gb[ia].T @ sk.Gb[ib]).T


def arm_metrics(sk, G, P, S, dt=None):
    ax = fa_axis(sk, S)
    ua, fa = sk.idx[S + " UpperArm"], sk.idx[S + " Forearm"]
    # elbow: forearm local vs bind local (in the forearm's bind frame)
    Lfa = np.einsum("fba,fbc->fac", G[:, ua], G[:, fa])
    Dfa = np.einsum("ba,fbc->fac", sk.Lbind[fa], Lfa)            # Lbind^T L: forearm-frame delta
    elbow = twist_angle(Dfa, ax)
    hand = twist_angle(rel_delta(sk, G, S + " Forearm", S + " Hand"), ax)
    ft = twist_angle(rel_delta(sk, G, S + " Forearm", S + " ForeTwist"), ax)
    ft1 = twist_angle(rel_delta(sk, G, S + " Forearm", S + " ForeTwist1"), ax)
    # elbow bend and its axis in the upper-arm frame
    u = P[:, fa] - P[:, ua]; v = P[:, sk.idx[S + " Hand"]] - P[:, fa]
    un = u / np.linalg.norm(u, axis=1, keepdims=True); vn = v / np.linalg.norm(v, axis=1, keepdims=True)
    bend = np.degrees(np.arccos(np.clip(np.sum(un * vn, 1), -1, 1)))
    ax_w = np.cross(un, vn)
    ax_l = np.einsum("fba,fb->fa", G[:, ua], ax_w)
    nrm = np.linalg.norm(ax_l, axis=1)
    ax_l = ax_l / np.maximum(nrm, 1e-9)[:, None]
    elbow, hand, ft, ft1 = wrap(elbow), wrap(hand), wrap(ft), wrap(ft1)
    return {"elbow": elbow, "hand": hand, "ft": ft, "ft1": ft1, "bend": bend, "bend_axis_ua": ax_l, "bend_sin": nrm}


def summarize(m, dt):
    d = lambda a: np.degrees(a)
    def jump(a):
        return float(np.abs(np.degrees(wrap(np.diff(a)))).max()) if len(a) > 1 else 0.0
    # elbow flip: the flexion axis (UA frame) reversing between frames while the arm is clearly bent (> 20 deg)
    ok = m["bend"] > 20
    axl = m["bend_axis_ua"]
    ref = np.median(axl[ok], 0) if ok.any() else np.array([0, 0, 1.0])
    ref = ref / max(np.linalg.norm(ref), 1e-9)
    dotref = axl @ ref
    flips = int(np.sum(ok & (dotref < 0)))
    # wrist / mid / elbow skin twist the mesh must absorb
    wrist = d(np.abs(wrap(m["hand"] - m["ft1"])))
    mid = d(np.abs(wrap(m["ft1"] - m["ft"])))
    elb = d(np.abs(wrap(m["ft"] + m["elbow"])))        # ForeTwist (child of UpperArm) vs the upper arm
    return {"hand_twist_max": round(float(np.abs(d(m["hand"])).max()), 1),
            "hand_twist_p95": round(float(np.percentile(np.abs(d(m["hand"])), 95)), 1),
            "elbow_twist_max": round(float(np.abs(d(m["elbow"])).max()), 1),
            "elbow_twist_p95": round(float(np.percentile(np.abs(d(m["elbow"])), 95)), 1),
            "ft_twist_max": round(float(np.abs(d(m["ft"])).max()), 1),
            "ft1_twist_max": round(float(np.abs(d(m["ft1"])).max()), 1),
            "skin_twist_wrist_max": round(float(wrist.max()), 1),
            "skin_twist_mid_max": round(float(mid.max()), 1),
            "skin_twist_elbow_max": round(float(elb.max()), 1),
            "twist_jump_per_frame_max": round(max(jump(m["hand"]), jump(m["elbow"]), jump(m["ft1"])), 1),
            "twist_jump_deg_per_s_max": round(max(jump(m["hand"]), jump(m["elbow"]), jump(m["ft1"])) / dt, 1),
            "bend_max": round(float(m["bend"].max()), 1),
            "elbow_axis_flip_frames": flips,
            "elbow_axis_min_dot": round(float(dotref[ok].min()) if ok.any() else 1.0, 3)}


def hinge_continuity(sk, G, P, S):
    """frame-to-frame turn of the elbow hinge axis (world) and of the elbow's pole direction (bent frames)"""
    ua, fa, ha = sk.idx[S + " UpperArm"], sk.idx[S + " Forearm"], sk.idx[S + " Hand"]
    h = np.einsum("fab,b->fa", G[:, ua], [0, 1.0, 0])
    dh = np.degrees(np.arccos(np.clip(np.sum(h[1:] * h[:-1], 1), -1, 1)))
    a, b, c = P[:, ua], P[:, fa], P[:, ha]
    d = c - a; d /= np.linalg.norm(d, axis=1, keepdims=True)
    e = (b - a) - d * np.sum((b - a) * d, 1, keepdims=True)
    en = np.linalg.norm(e, axis=1)
    e = e / np.maximum(en, 1e-9)[:, None]
    bent = (en[1:] > 0.04) & (en[:-1] > 0.04)              # elbow >= 4 cm off the shoulder-wrist line
    de = np.degrees(np.arccos(np.clip(np.sum(e[1:] * e[:-1], 1), -1, 1)))
    return float(dh.max()), float(de[bent].max()) if bent.any() else 0.0


_skin_cache = {}


def skin_data(ident):
    if ident in _skin_cache:
        return _skin_cache[ident]
    import glbio
    import pair as pr
    sk = rt.TodSkel(ident)
    gl, b = glbio.read_glb(pr.GLB[ident])
    skin = gl["skins"][0]
    jn = [gl["nodes"][j].get("name", "") for j in skin["joints"]]
    order = [sk.idx["" if n == "bone0" else n] for n in jn]
    ibm = glbio.accessor(gl, b, skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
    Js, Ws = [], []
    for mesh in gl["meshes"]:
        for p in mesh["primitives"]:
            Js.append(glbio.accessor(gl, b, p["attributes"]["JOINTS_0"]).astype(int))
            W = glbio.accessor(gl, b, p["attributes"]["WEIGHTS_0"]).astype(float)
            Ws.append(W / np.maximum(W.sum(1, keepdims=True), 1e-9))
    J = np.concatenate(Js); W = np.concatenate(Ws)
    out = {}
    for S in ("L", "R"):
        fore = [k for k, n in enumerate(jn) if n.startswith(S + " ") and any(t in n for t in ("Forearm", "ForeTwist", "Hand"))]
        finger = [k for k, n in enumerate(jn) if n.startswith(S + " Finger")]
        live = W > 0.05
        # forearm-region vertices (an influence on Forearm / twist links / Hand, none on a finger) blended from two or
        # more bones: the ones that candy-wrap when neighbouring influences roll differently
        sel = (np.isin(J, fore) & live).any(1) & ~(np.isin(J, finger) & live).any(1) & (live.sum(1) >= 2)
        out[S] = (J[sel], np.where(live[sel], W[sel], 0.0))
    _skin_cache[ident] = (order, ibm, out)
    return _skin_cache[ident]


def skin_candy(ident, G, P):
    """per arm, on the real mesh weights: for every forearm-region vertex blended from several bones, the largest roll
    difference between two of its influences about the forearm's own axis (elbow -> wrist, world). Bending the elbow
    (a hinge) adds no roll; a roll difference is what linear-blend skinning collapses into a candy wrapper.
    'candy_deg' = that roll x the smaller of the two weights x 2 (an equal 50/50 blend counts the full roll)."""
    sk = rt.TodSkel(ident)
    order, ibm, verts = skin_data(ident)
    Rj = np.einsum("fjab,jbc->fjac", G[:, order], ibm[:, :3, :3])      # skin rotation per glb joint
    Pp = sk.positions(G, np.zeros((len(G), 3))) if P is None else P
    res = {}
    for S, (J, W) in verts.items():
        ax = Pp[:, sk.idx[S + " Hand"]] - Pp[:, sk.idx[S + " Forearm"]]
        ax /= np.linalg.norm(ax, axis=1, keepdims=True)
        worst = np.zeros(len(G)); raw = np.zeros(len(G))
        for f in range(len(G)):
            R = Rj[f][J]                                              # (v,4,3,3)
            best = np.zeros(len(J)); best_raw = np.zeros(len(J))
            for i in range(4):
                for j in range(i + 1, 4):
                    m = (W[:, i] > 0) & (W[:, j] > 0)
                    if not m.any():
                        continue
                    Dr = np.einsum("vab,vcb->vac", R[m, j], R[m, i])
                    q = Rot.from_matrix(Dr).as_quat()
                    tw = np.abs(np.degrees(wrap(2 * np.arctan2(q[:, :3] @ ax[f], q[:, 3]))))
                    eff = tw * 2 * np.minimum(W[m, i], W[m, j])
                    best[m] = np.maximum(best[m], eff); best_raw[m] = np.maximum(best_raw[m], tw)
            worst[f] = best.max(); raw[f] = best_raw.max()
        res[S] = {"candy_deg_max": round(float(worst.max()), 1), "candy_deg_p95": round(float(np.percentile(worst, 95)), 1),
                  "neighbour_roll_deg_max": round(float(raw.max()), 1),
                  "frames_candy_over_60deg": int((worst > 60).sum()), "frame_of_max": int(np.argmax(worst))}
    return res


def survey():
    import todclips
    recs = todclips.anim_records()
    sk = rt.TodSkel("ram")
    rows = []
    for n, (R, h) in sorted(recs.items()):
        b = os.path.basename(n)
        if not b.startswith("ram_") or "facial" in b:
            continue
        clip = todclips.cj.record_to_clip(h)
        names = {c["name"] for c in clip["channels"] if c.get("ori")}
        if not {"L ForeTwist", "L ForeTwist1", "L Hand", "L Forearm"} <= names:
            continue
        G, P = clip_world(sk, clip)
        for S in ("L", "R"):
            m = arm_metrics(sk, G, P, S)
            rows.append((b, S, m))
    # pooled ratios where the hand twist is large
    H = np.concatenate([r[2]["hand"] for r in rows]); FT = np.concatenate([r[2]["ft"] for r in rows])
    FT1 = np.concatenate([r[2]["ft1"] for r in rows]); EL = np.concatenate([r[2]["elbow"] for r in rows])
    big = np.abs(H) > np.radians(20)
    k_ft = float(np.sum(FT[big] * H[big]) / np.sum(H[big] ** 2))
    k_ft1 = float(np.sum(FT1[big] * H[big]) / np.sum(H[big] ** 2))
    res_ft1 = float(np.degrees(np.abs(FT1[big] - k_ft1 * H[big])).max())
    out = {"clips": len(rows) // 2, "frames": int(len(H)), "frames_hand_twist_over_20deg": int(big.sum()),
           "ForeTwist_over_Hand_ratio": round(k_ft, 4), "ForeTwist1_over_Hand_ratio": round(k_ft1, 4),
           "ForeTwist1_fit_residual_max_deg": round(res_ft1, 2),
           "hand_twist_abs_p50_p95_max": [round(float(np.degrees(np.percentile(np.abs(H), q))), 1) for q in (50, 95, 100)],
           "elbow_twist_abs_p50_p95_max": [round(float(np.degrees(np.percentile(np.abs(EL), q))), 1) for q in (50, 95, 100)],
           "ForeTwist_abs_max": round(float(np.degrees(np.abs(FT)).max()), 2)}
    # per-clip extremes
    worst = sorted(rows, key=lambda r: -np.abs(r[2]["hand"]).max())[:8]
    out["largest_hand_twist_clips"] = [(r[0], r[1], round(float(np.degrees(np.abs(r[2]["hand"]).max())), 1),
                                        round(float(np.degrees(np.abs(r[2]["ft1"]).max())), 1),
                                        round(float(np.degrees(np.abs(r[2]["elbow"]).max())), 1)) for r in worst]
    worst_e = sorted(rows, key=lambda r: -np.abs(r[2]["elbow"]).max())[:8]
    out["largest_elbow_twist_clips"] = [(r[0], r[1], round(float(np.degrees(np.abs(r[2]["elbow"]).max())), 1)) for r in worst_e]
    return out


def weights(ident):
    import glbio
    import pair as pr
    gl, b = glbio.read_glb(pr.GLB[ident])
    skin = gl["skins"][0]
    jn = [gl["nodes"][j].get("name", "") for j in skin["joints"]]
    tot = {}
    for mesh in gl["meshes"]:
        for p in mesh["primitives"]:
            J = glbio.accessor(gl, b, p["attributes"]["JOINTS_0"]).astype(int)
            W = glbio.accessor(gl, b, p["attributes"]["WEIGHTS_0"]).astype(float)
            for k in range(4):
                for j, w in zip(J[:, k], W[:, k]):
                    if w > 0:
                        tot[jn[j]] = tot.get(jn[j], 0.0) + w
    keys = [k for k in tot if any(s in k for s in ("UpperArm", "Forearm", "ForeTwist", "Hand"))]
    return {k: round(tot[k], 1) for k in sorted(keys)}


def clip_report(sk, ident, G, bip, dt, candy=True):
    P = sk.positions(G, bip)
    out = {}
    cd = skin_candy(ident, G, P) if candy else {}
    for S in ("L", "R"):
        r = summarize(arm_metrics(sk, G, P, S), dt)
        hj, ej = hinge_continuity(sk, G, P, S)
        r["hinge_axis_turn_deg_per_frame_max"] = round(hj, 1)
        r["elbow_pole_turn_deg_per_frame_max"] = round(ej, 1)
        if candy:
            r.update(cd[S])
        out[S] = r
    return out


def npz_metrics(path, roles, keys=("G", "BIP")):
    d = np.load(path)
    dt = 1 / 25.0 if "_25" in path else 1 / 30.0
    out = {}
    for role, ident in roles:
        sk = rt.TodSkel(ident)
        G = d[(role + "_" + keys[0]) if role else keys[0]]
        bip = d[(role + "_" + keys[1]) if role else keys[1]]
        for S, r in clip_report(sk, ident, G, bip, dt).items():
            out["%s %s" % (role or ident, S)] = r
    return out


def vanilla_candy(names=("ram_vehicle_truck_exit", "ram_climb_obstacle_up", "ram_aim_lo_melee", "ram_idle_breathe_aware",
                         "ram_run", "ram_shootdodge_front_midair", "ram_reload_dual", "ram_cc_defend_front")):
    """the same metrics on game clips (Ram): what the game itself shows"""
    import todclips
    recs = todclips.anim_records()
    sk = rt.TodSkel("ram")
    out = {}
    for n in names:
        clip = todclips.get_clip(n, recs)
        G, P = clip_world(sk, clip)
        bip = P[:, sk.idx["Bip"]]
        out[n] = clip_report(sk, "ram", G, bip, 1.0 / max(clip["fps"], 1))
    return out


def _emit(obj):
    s = json.dumps(obj, indent=1)
    out = os.environ.get("ARMTWIST_OUT")
    if out:
        open(out, "w").write(s)
    print(s)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "survey":
        print(json.dumps(survey(), indent=1))
    elif cmd == "weights":
        for ident in ("ram", "soldier", "civil", "dea_taskforce"):
            print(ident, weights(ident))
    elif cmd == "vanilla":
        _emit(vanilla_candy())
    elif cmd == "npz":
        path = sys.argv[2]
        idents = sys.argv[3:]
        roles = list(zip(["lead", "follow"], idents)) if len(idents) == 2 else [("", idents[0])]
        _emit(npz_metrics(path, roles))
