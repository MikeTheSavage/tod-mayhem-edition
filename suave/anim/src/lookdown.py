"""lookdown.py - suave_lookdown_<pairing> (partner): after the push the partner stops, looks down at the
dynamite in the front of his belt and freezes in terror.

  python lookdown.py PARTNER [--fps 25]     -> ../work/lookdown_<partner>_<fps>.npz + .json

1.5 s, oneshot, starts on the stumble's last frame (same shared frame: Bip carries the offset; the dynamite sits on the
Pelvis). 0-0.35 s he catches himself (feet planted where the stumble left them, knees give 4 cm); 0.15-0.6 s the head
drops to the belt (neck 16 + head 32 degrees, upper spine curls 12), shoulders hunch up 12 degrees, both hands fly up
by the chin, palms out, elbows tucked; 0.6-1.5 s frozen, trembling (hands +-0.8 cm, spine +-0.6 degrees at 7 Hz).
Arms in the game convention (retarget.canonical_arms).
"""
import argparse, json, os, sys
import numpy as np

sys.dont_write_bytecode = True
import retarget as rt
import pair as pr
import armtwist as at
import intro as it
import todclips

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "work")
T = 1.5


def build(partner, fps=25, verbose=True):
    d = np.load(os.path.join(WORK, "pair_%s_%d.npz" % (partner, fps)))
    G0 = d["follow_foG"][-1]; B0 = d["follow_foB"][-1]
    a = it.Actor(partner, todclips.get_clip("ram_idle_breathe_aware"), 0.0)
    sk = a.sk
    L0 = sk.local(G0[None])[0]
    a.Ltoe0 = {S: L0[sk.idx[S + " Toe0"]] for S in ("L", "R")}
    P0 = sk.positions(G0[None], B0[None])[0]
    toes = {S: P0[sk.idx[S + " Toe0"]] for S in ("L", "R")}
    frots = {S: G0[sk.idx[S + " Foot"]] for S in ("L", "R")}
    yaw = it.yaw_of_rot(G0[sk.idx["Bip"]], sk)
    R = it.Ry(yaw)
    fwd, right, up = R @ [0, 0, 1.0], R @ [1.0, 0, 0], np.array([0, 1.0, 0])
    n = int(round(T * fps)) + 1
    dt = T / (n - 1)
    Gout = np.zeros((n,) + G0.shape); Bout = np.zeros((n, 3))
    from scipy.spatial.transform import Rotation as Rot
    for k in range(n):
        t = k * dt
        w = it.ss(t, 0.15, 0.6)
        trem = it.ss(t, 0.55, 0.8) * np.sin(2 * np.pi * 7.0 * t)
        G = G0.copy()
        bip = B0 - [0, 0.04 * it.ss(t, 0.0, 0.35), 0]

        def rot_sub(name, axis, deg):
            Rm = Rot.from_rotvec(np.radians(deg) * axis).as_matrix()
            for b in pr.subtree(sk, sk.idx[name]):
                G[b] = Rm @ G[b]
        # upright first (undo most of the stumble's lean), then the curl over the belt
        P = sk.positions(G[None], bip[None])[0]
        sp = P[sk.idx["Neck"]] - P[sk.idx["Spine1"]]
        lean_now = np.degrees(np.arctan2(np.dot(sp, fwd), np.dot(sp, up)))
        rot_sub("Spine1", right, -0.7 * lean_now * it.ss(t, 0.0, 0.35) + 12 * w + 0.6 * trem)
        rot_sub("Neck", right, 16 * w)
        rot_sub("Head", right, 32 * w)
        for S, sgn in (("L", -1), ("R", 1)):
            P = sk.positions(G[None], bip[None])[0]
            cl = sk.idx[S + " Clavicle"]; ua = sk.idx[S + " UpperArm"]
            Rs = Rot.from_rotvec(np.radians(12 * w) * fwd).as_matrix()
            if (Rs @ (P[ua] - P[cl]))[1] < (P[ua] - P[cl])[1]:
                Rs = Rs.T
            for b in pr.subtree(sk, cl):
                G[b] = Rs @ G[b]
        G, bip, _ = a.place_feet(G, bip, toes, frots)
        P = sk.positions(G[None], bip[None])[0]
        head = P[sk.idx["Head"]]
        chin = head + 0.10 * up + 0.06 * fwd
        wa = it.ss(t, 0.12, 0.45)
        for S, sgn in (("L", -1), ("R", 1)):
            goal = chin - 0.24 * up + 0.12 * fwd + sgn * 0.10 * right + np.array([0.008 * trem, 0.006 * trem, 0])
            rot = it.hand_frame(sk, S, up + 0.2 * fwd + 0.25 * sgn * right, fwd - 0.3 * sgn * right)
            G = a.place_hand(G, bip, S, goal, rot, sgn * 0.3 * right - 0.8 * up, wa)
        Gout[k] = G; Bout[k] = bip
    Gout = rt.canonical_arms(sk, Gout, Bout)
    Gout[0] = G0; Bout[0] = B0                         # starts exactly on the stumble's last frame
    P = sk.positions(Gout, Bout)
    # head pitch toward the pelvis: angle between the head's down-forward look and the direction to the belt
    hd = P[:, sk.idx["Head"]] - P[:, sk.idx["Neck"]]
    belt = P[:, sk.idx["Pelvis"]] + 0.12 * fwd - P[:, sk.idx["Head"]]
    face_dir = np.array([Gout[k, sk.idx["Head"]] @ (sk.Gb[sk.idx["Head"]].T @ np.array([0, 0, 1.0])) for k in range(n)])
    look_err = np.degrees(np.arccos(np.clip(np.sum(face_dir * belt, 1) / np.linalg.norm(belt, axis=1), -1, 1)))
    toe_mv = max(float(np.linalg.norm(P[:, sk.idx[S + " Toe0"]] - toes[S], axis=1).max()) for S in ("L", "R"))
    hp = {S: pr.hand_point_tod(sk, P, S) for S in ("L", "R")}
    chin_d = [float(np.linalg.norm(hp[S][-1] - P[-1, sk.idx["Head"]])) for S in ("L", "R")]
    rep = {"partner": partner, "fps": fps, "frames": n, "dt": dt, "T_s": T,
           "start_vs_stumble_last_mm": 0.0, "toe_moved_mm_max": round(1000 * toe_mv, 2),
           "face_dir_to_belt_deg_start_end": [round(float(look_err[0]), 1), round(float(look_err[-1]), 1)],
           "hands_to_head_cm_end": [round(100 * c, 1) for c in chin_d],
           "arms": at.clip_report(sk, partner, Gout, Bout, dt)}
    if verbose:
        print(json.dumps({k: v for k, v in rep.items() if k != "arms"}, indent=1))
    return {"follow_G": Gout, "follow_BIP": Bout, "follow_P": P}, rep


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("partner")
    ap.add_argument("--fps", type=int, default=25)
    a = ap.parse_args()
    out, rep = build(a.partner, a.fps)
    path = os.path.join(WORK, "lookdown_%s_%d.npz" % (a.partner, a.fps))
    np.savez_compressed(path, **out)
    json.dump(rep, open(path.replace(".npz", ".json"), "w"), indent=1, default=float)
    print(path)
