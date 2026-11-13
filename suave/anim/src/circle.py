"""circle.py - the ring stand-in: Rokoko GentleSwayingDancing (Mixamo rig) -> TOD Biped (soldier.model), in-place loop.

  python circle.py [--fps 25]   -> ../work/circle_<fps>.npz + .json

ONE SWAY PER BEAT (a review note on the first previews): the body reaches one side on every beat of Suavemente
(124.17 BPM), so one left-right cycle = 2 beats = 0.966 s; the clip is CIRCLE_BEATS = 24 beats (12 cycles) and loops.
Source: the single sway cycle of the arms-up part of the take (frames 757-1189 at 24 fps, a crowd cheering with raised
hands) whose end pose matches its start best, played about 3.3x faster. The cycle's last CYCLE_X of phase cross-fades
(local rotations + hips) into the frames just before its start, so every cycle joins the next seamlessly and the
pose has no drift (no yaw / translation correction needed). Root: hips sway kept, centred on the slot, facing +z
(the ring centre). Feet locked on the ground (2-bone IK, planted the whole time). Arms in the game's convention
(retarget.canonical_arms).
"""
import argparse, json, os, sys
import numpy as np

sys.dont_write_bytecode = True
import retarget as rt
import pair as pr

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "work")
SRC = os.path.join(WORK, "rokoko", "GentleSwayingDancing_mixamo.npz")
RANGE = (757, 1189)
CIRCLE_BEATS = 24
BEATS_PER_CYCLE = 2          # one sway (one side) per beat
CYCLE_X = 0.25               # share of the cycle cross-faded into its own lead-in
SKEL = "soldier"


def choose_cycle(src):
    """start frame / length (source frames) of the best-closing single sway cycle inside RANGE"""
    P = src.P
    hips = src.idx["Hips"]
    rel = P - P[:, hips:hips + 1]
    vel = np.gradient(P[:, hips], axis=0)
    best = None
    for L in range(int(2.8 * src.fps), int(3.5 * src.fps)):
        for s0 in range(RANGE[0] + 10, RANGE[1] - L - 10):
            e = np.linalg.norm(rel[s0] - rel[s0 + L], axis=1).mean() + 2.0 * np.linalg.norm(vel[s0] - vel[s0 + L]) * src.fps * 0.1
            if best is None or e < best[0]:
                best = (e, s0, L)
    return best


def build(fps=25, verbose=True):
    src = rt.MixamoSource(SRC)
    err, s0, L = choose_cycle(src)
    T = CIRCLE_BEATS * pr.BEAT
    Tc = BEATS_PER_CYCLE * pr.BEAT
    n = int(round(T * fps)) + 1
    dt = T / (n - 1)
    K = (L / src.fps) / Tc                             # source seconds per clip second (~3.3)
    taus = np.arange(n) * dt
    c = np.mod(taus, Tc)
    c[-1] = c[0]                                       # the last frame IS the first (cyclic)
    shim = type("S", (), {})()
    shim.P, shim.R, shim.fps = src.P, src.G, src.fps
    PA, GA = pr.resample(shim, s0 / src.fps + K * c, 0)
    PB, GB = pr.resample(shim, s0 / src.fps + K * (c - Tc), 0)      # the lead-in (just before the cycle start)
    ph = c / Tc
    a = np.clip((ph - (1 - CYCLE_X)) / CYCLE_X, 0, 1)
    a = 0.5 - 0.5 * np.cos(np.pi * a)
    sk = rt.TodSkel(SKEL)
    al = rt.rest_alignment(sk, src.Jt, src.idx, rt.MIXAMO_MAP, J_is_tod=True)
    GtA = rt.retarget_rotations(sk, GA, al, src.idx, rt.MIXAMO_MAP)
    GtB = rt.retarget_rotations(sk, GB, al, src.idx, rt.MIXAMO_MAP)
    LA, LB = sk.local(GtA), sk.local(GtB)
    Gt = np.zeros_like(GtA)
    for f in range(n):
        Lm = pr.slerp_rows(LA[f], LB[f], np.full(LA.shape[1], a[f]))
        Gt[f] = sk.fk_local(Lm[None], sk.pos[None])[0][0]
    P = PA * (1 - a)[:, None, None] + PB * a[:, None, None]
    # scales and root
    leg_s = np.mean([np.linalg.norm(src.Jt[src.idx[x]] - src.Jt[src.idx[y]]) for x, y in
                     (("LeftUpLeg", "LeftLeg"), ("LeftLeg", "LeftFoot"), ("RightUpLeg", "RightLeg"), ("RightLeg", "RightFoot"))]) * 2
    s = sk.leg_len() / leg_s
    floor = float(np.percentile(np.minimum(P[:, src.idx["LeftToeBase"], 1], P[:, src.idx["RightToeBase"], 1]), 5))
    hip_rest = float(0.5 * (src.Jt[src.idx["LeftUpLeg"], 1] + src.Jt[src.idx["RightUpLeg"], 1]) - src.Jt[src.idx["LeftToeBase"], 1])
    hipc = 0.5 * (P[:, src.idx["LeftUpLeg"]] + P[:, src.idx["RightUpLeg"]])
    # face +z (the ring centre): the mean facing over one cycle
    fwd = np.einsum("fab,b->fa", Gt[:, sk.idx["Bip"]] @ sk.Gb[sk.idx["Bip"]].T, [0, 0, 1.0])
    yaw = pr.yaw_of(fwd)
    face = float(np.arctan2(np.sin(yaw).mean(), np.cos(yaw).mean()))
    Rc = pr.Ry(-face)
    Gt = np.einsum("ab,fjbc->fjac", Rc, Gt)
    xz = np.einsum("ab,fb->fa", Rc, hipc * [1, 0, 1])
    xz = xz - xz.mean(0) * [1, 0, 1]                  # centre the sway on the slot
    bip_stand = pr.GROUND - (pr.SOLE[SKEL] - sk.Pb[sk.idx["Bip"]][1])
    y = bip_stand + s * (hipc[:, 1] - floor - hip_rest)
    BIP = np.stack([xz[:, 0] * s, y, xz[:, 2] * s], 1)
    # planted feet keep one world rotation (flat on the ground, the clip's mean), so the toes do not skate
    from scipy.spatial.transform import Rotation as _R
    for S in ("L", "R"):
        for nm in ("Foot", "Toe0"):
            b = sk.idx[S + " " + nm]
            Gt[:, b] = _R.from_matrix(Gt[:, b]).mean().as_matrix()
    Pt = sk.positions(Gt, BIP)
    # the faster sway lifts the hips past the legs' reach at the top of the bob: lower the Bip there (smooth) so the
    # planted toes can stay put
    locks = {}
    drop = np.zeros(n)
    for S in ("L", "R"):
        toe = sk.idx[S + " Toe0"]; ank = sk.idx[S + " Foot"]; th = sk.idx[S + " Thigh"]
        toe_h = (sk.Pb[toe][1] - sk.Pb[sk.idx["Bip"]][1]) - (pr.SOLE[SKEL] - sk.Pb[sk.idx["Bip"]][1])
        lock = Pt[:, toe].mean(0).copy(); lock[1] = pr.GROUND + max(toe_h, 0.0)
        locks[S] = lock
        goal = Pt[:, ank] + (lock - Pt[:, toe])
        Lleg = 0.995 * (sk.chain_len(S + " Thigh", S + " Calf") + sk.chain_len(S + " Calf", S + " Foot"))
        dv = Pt[:, th] - goal
        hz = np.linalg.norm(dv[:, [0, 2]], axis=1)
        ymax = np.sqrt(np.maximum(Lleg ** 2 - hz ** 2, 0))
        drop = np.maximum(drop, dv[:, 1] - ymax)
    drop = np.maximum(drop, 0)
    mx = np.array([drop[np.r_[k - 3:k + 4] % (n - 1)].max() for k in range(n)])
    ker = np.hanning(9); ker /= ker.sum()
    drop = np.array([np.dot(ker, mx[np.r_[k - 4:k + 5] % (n - 1)]) for k in range(n)])
    BIP[:, 1] -= drop
    Pt = sk.positions(Gt, BIP)
    # feet: planted the whole time (the source dancer never lifts them in this part); one lock per foot = its mean
    feet = {}
    for side, S in (("Left", "L"), ("Right", "R")):
        toe = sk.idx[S + " Toe0"]; ank = sk.idx[S + " Foot"]
        toe_h = (sk.Pb[toe][1] - sk.Pb[sk.idx["Bip"]][1]) - (pr.SOLE[SKEL] - sk.Pb[sk.idx["Bip"]][1])
        gy = pr.GROUND + max(toe_h, 0.0)
        lock = np.tile(locks[S], (n, 1)); lock[:, 1] = gy
        goal = Pt[:, ank] + (lock - Pt[:, toe])
        Pt = pr.apply_leg_ik(sk, Gt, Pt, S, goal, np.ones(n), BIP)
        feet[S] = {"locked_frames": n}
    Gt = rt.canonical_arms(sk, Gt, BIP, cyclic=True)
    Gt[-1] = Gt[0]; BIP[-1] = BIP[0]
    Pd = sk.positions(Gt, BIP)
    toes = {S: Pd[:, sk.idx[S + " Toe0"]] for S in ("L", "R")}
    hx = BIP[:, 0]
    # sway count check: hips side extremes per beat
    ext = int(np.sum(np.diff(np.sign(np.diff(hx))) != 0))
    rep = {"source": os.path.basename(SRC), "source_cycle_frames": [int(s0), int(s0 + L)], "source_fps": src.fps,
           "cycle_match_error": float(err), "beats": CIRCLE_BEATS, "beats_per_cycle": BEATS_PER_CYCLE,
           "sway_per_beat": True, "T_s": T, "fps": fps, "frames": n,
           "time_scale_src_per_clip": K, "leg_scale": s, "feet": feet,
           "toe_slide_mm_max": round(1000 * float(max(np.linalg.norm(np.diff(t, axis=0), axis=1).max() for t in toes.values())), 2),
           "lowest_toe_above_ground_cm": round(100 * float(min(t[:, 1].min() for t in toes.values()) - pr.GROUND), 1),
           "bip_sway_m": [round(float(np.ptp(BIP[:, 0])), 3), round(float(np.ptp(BIP[:, 2])), 3)],
           "hips_side_extremes": ext, "bip_lowered_for_reach_cm_max": round(100 * float(drop.max()), 1), "loop_closure_bip_m": float(np.linalg.norm(BIP[-1] - BIP[0]))}
    if verbose:
        print(json.dumps(rep, indent=1))
    return {"G": Gt, "BIP": BIP, "P": Pd}, rep


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=25)
    a = ap.parse_args()
    out, rep = build(a.fps)
    path = os.path.join(WORK, "circle_%d.npz" % a.fps)
    np.savez_compressed(path, **out)
    json.dump(rep, open(path.replace(".npz", ".json"), "w"), indent=1)
    print(path)
