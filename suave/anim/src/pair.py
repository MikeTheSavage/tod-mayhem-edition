"""pair.py - build one matched Suave pairing: Ram (lead) + a partner model (follow), from the chosen CoMPAS3D window.

  python pair.py PARTNER [--fps 25] [--out ../work/pair_<partner>_<fps>.npz]     PARTNER = soldier | civil | dea_taskforce

Pipeline (all in the TOD pair frame = Ram's model space at the dance start; +z = Ram's facing, -x = Ram's left):
 1 time warp: clip time tau -> source time t0 + K*tau, K fitted so the six annotated 8-count boundaries land on every
   4th beat of Suavemente (one salsa count = half a merengue beat); 24 beats of dance + the release follow-on;
 2 source joints (SMPL-X FK) resampled at the output rate, rotated into the pair frame;
 3 rotations retargeted per bone (retarget.py); Bip = hip centre, heights scaled by leg length, partner placed relative
   to Ram with the arm-span ratio, both standing on one ground (GROUND, model-space y of the soles);
 4 torso interpenetration removed (partner root pushed out, smoothed);
 5 foot contacts from the source (low ball speed + low height): balls locked in place on the ground, 2-bone leg IK;
 6 hand contacts from the source (hand-hand grips, hand on the partner's back / shoulder / waist / head): 2-bone arm IK
   to the source contact offset, scaled; ramps of 3 frames;
 7 loop: the pair's drift over the 24 beats (rigid yaw + translation) is spread linearly over the clip and the last
   LOOPX seconds cross-fade into the first frames, so frame N-1 == frame 0;
 8 metrics (hands, feet, penetration, pose fidelity) -> the npz + a json report.
"""
import argparse, json, os, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot, Slerp

sys.dont_write_bytecode = True
import retarget as rt
import smplx_fk as sx

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "work")

# ---- the chosen section (see README: Pair5 song2 take1, 13.92-28.36 s, six 8-counts) ----
PAIR, SONG, TAKE = 5, 2, 1
BOUNDS = [13.92, 16.2, 18.64, 21.08, 23.52, 25.92, 28.36]     # annotated 8-count starts (source seconds)
BPM = 124.17
BEAT = 60.0 / BPM
DANCE_BEATS = 24
RELEASE_BEAT = 19.0            # plant frame: Ram's right hand reaches the partner's waist (the plant), no hand-hand grip
FOLLOW_ON = 4.5 * BEAT         # push / stumble stand-ins: waist hold (plant) then the open break (partner sent away)
LOOPX = 0.30                   # loop cross-fade length (s)
GROUND = -1.09                 # model-space y of the soles (vanilla idle: Bip y -0.016, Ram's soles 1.08 below)
SOLE = {"ram": -1.0747, "soldier": -1.029088, "civil": -1.028065, "dea_taskforce": -1.080762}   # glb mesh min y
STAGE = os.environ.get("TOD_STAGE_DIR", "stage")  # folder with the exported character .glb files
GLB = {"ram": STAGE + "/c8b4da5f7a4.glb", "soldier": STAGE + "/c57f9245ca9.glb",
       "civil": STAGE + "/cfd2f5f2554.glb", "dea_taskforce": STAGE + "/c2cdf871494.glb"}

ANCHORS = {   # SMPL joint -> (TOD pivot, surface radius m)
    "spine3": ("Spine3", 0.12), "spine2": ("Spine2", 0.12), "spine1": ("Spine1", 0.13), "pelvis": ("Bip", 0.13),
    "neck": ("Neck", 0.06), "head": ("Head", 0.10),
    "left_shoulder": ("L UpperArm", 0.06), "right_shoulder": ("R UpperArm", 0.06),
    "left_collar": ("L Clavicle", 0.07), "right_collar": ("R Clavicle", 0.07),
    "left_elbow": ("L Forearm", 0.05), "right_elbow": ("R Forearm", 0.05),
    "left_hip": ("L Thigh", 0.09), "right_hip": ("R Thigh", 0.09),
}
HAND_R = 0.045
HH_CONTACT = 0.20              # hand-hand contact: palm centres closer than this
BODY_GAP = 0.05                # hand-body contact: palm centre within radius + HAND_R + BODY_GAP of the joint


def time_map():
    b = np.arange(len(BOUNDS)) * 4.0 * BEAT
    K, t0 = np.polyfit(b, np.array(BOUNDS), 1)
    return float(t0), float(K)


# ------------------------------------------------------------------------------------------------ helpers
def slerp_rows(R0, R1, a):
    """per-row slerp of rotation matrices (n,3,3) by weights a (n,)"""
    q0 = Rot.from_matrix(R0).as_quat(); q1 = Rot.from_matrix(R1).as_quat()
    d = np.sum(q0 * q1, -1); q1[d < 0] *= -1; d = np.abs(d)
    th = np.arccos(np.clip(d, -1, 1)); s = np.sin(th)
    w0 = np.where(s > 1e-6, np.sin((1 - a) * th) / np.maximum(s, 1e-9), 1 - a)
    w1 = np.where(s > 1e-6, np.sin(a * th) / np.maximum(s, 1e-9), a)
    q = w0[:, None] * q0 + w1[:, None] * q1
    return Rot.from_quat(q / np.linalg.norm(q, axis=1, keepdims=True)).as_matrix()


def resample(src, times, f0):
    """source world rotations / positions at source times (s); src frames start at sequence frame f0"""
    x = times * src.fps - f0
    i = np.clip(np.floor(x).astype(int), 0, len(src.P) - 2)
    a = np.clip(x - i, 0, 1)
    P = src.P[i] * (1 - a)[:, None, None] + src.P[i + 1] * a[:, None, None]
    F, J = len(times), src.R.shape[1]
    R = np.zeros((F, J, 3, 3))
    for j in range(J):
        R[:, j] = slerp_rows(src.R[i, j], src.R[i + 1, j], a)
    return P, R


def yaw_of(fwd):
    return np.arctan2(fwd[..., 0], fwd[..., 2])


def Ry(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def segments(flag, fill=2, minlen=4):
    """boolean array -> list of (start, end_exclusive) after closing gaps <= fill and dropping short runs"""
    f = flag.copy()
    n = len(f)
    i = 0
    while i < n:            # close small gaps
        if not f[i]:
            j = i
            while j < n and not f[j]:
                j += 1
            if 0 < i and j < n and j - i <= fill:
                f[i:j] = True
            i = j
        else:
            i += 1
    segs, i = [], 0
    while i < n:
        if f[i]:
            j = i
            while j < n and f[j]:
                j += 1
            if j - i >= minlen:
                segs.append((i, j))
            i = j
        else:
            i += 1
    return segs


def ramp_weights(n, segs, ramp):
    w = np.zeros(n)
    for a, b in segs:
        for k in range(a - ramp, b + ramp):
            if 0 <= k < n:
                if k < a:
                    v = (k - (a - ramp) + 1) / (ramp + 1)
                elif k >= b:
                    v = ((b + ramp) - k) / (ramp + 1)
                else:
                    v = 1.0
                w[k] = max(w[k], v)
    return w


def hand_point_tod(sk, P, S):
    f = 0.5 * (P[:, sk.idx[S + " Finger1"]] + P[:, sk.idx[S + " Finger2"]])
    return 0.5 * (P[:, sk.idx[S + " Hand"]] + f)


def hand_point_src(P, side):
    return 0.5 * (P[:, sx.IDX[side + "_wrist"]] + P[:, sx.IDX[side + "_middle1"]])


def apply_arm_ik(sk, G, P, S, target_hp, w, bip):
    """move hand point toward target_hp (F,3) with weight w (F,), keeping the hand's world rotation"""
    ua, fa, ha = sk.idx[S + " UpperArm"], sk.idx[S + " Forearm"], sk.idx[S + " Hand"]
    hp = hand_point_tod(sk, P, S)
    for f in np.nonzero(w > 1e-4)[0]:
        goal_hp = hp[f] + w[f] * (target_hp[f] - hp[f])
        wrist_goal = goal_hp - (hp[f] - P[f, ha])
        # pole from the upper arm's hinge axis: a straight arm keeps its elbow side (no flips)
        Ra, Rb, _ = rt.two_bone_ik(P[f, ua], P[f, fa], P[f, ha], wrist_goal, rt.hinge_pole(sk, G[f], S, P[f, ua], wrist_goal))
        Gh = G[f, ha].copy()
        # rotate the upper arm subtree, then the forearm subtree; keep the hand (and fingers) world rotation
        sub_u = subtree(sk, ua); sub_f = subtree(sk, fa)
        for b in sub_u:
            G[f, b] = Ra @ G[f, b]
        for b in sub_f:
            G[f, b] = Rb @ G[f, b]
        hand_sub = subtree(sk, ha)
        corr = Gh @ G[f, ha].T
        for b in hand_sub:
            G[f, b] = corr @ G[f, b]
    return sk.positions(G, bip)


def apply_leg_ik(sk, G, P, S, ankle_goal, w, bip):
    th, ca, fo = sk.idx[S + " Thigh"], sk.idx[S + " Calf"], sk.idx[S + " Foot"]
    for f in np.nonzero(w > 1e-4)[0]:
        goal = P[f, fo] + w[f] * (ankle_goal[f] - P[f, fo])
        knee_dir = P[f, ca] - 0.5 * (P[f, th] + P[f, fo])
        Ra, Rb, _ = rt.two_bone_ik(P[f, th], P[f, ca], P[f, fo], goal, P[f, ca] + knee_dir)
        Gfoot = G[f, fo].copy()
        for b in subtree(sk, th):
            G[f, b] = Ra @ G[f, b]
        for b in subtree(sk, ca):
            G[f, b] = Rb @ G[f, b]
        corr = Gfoot @ G[f, fo].T
        for b in subtree(sk, fo):
            G[f, b] = corr @ G[f, b]
    return sk.positions(G, bip)


_sub_cache = {}


def subtree(sk, b):
    key = (sk.ident, b)
    if key not in _sub_cache:
        out = [b]
        k = 0
        while k < len(out):
            out += [i for i in range(len(sk.names)) if sk.parent[i] == out[k]]
            k += 1
        _sub_cache[key] = out
    return _sub_cache[key]


def capsules(sk, P):
    """torso / head / thigh capsules (a, b, radius) used for the interpenetration metric"""
    ix = sk.idx
    head_top = P[:, ix["Head"]] + (P[:, ix["Head"]] - P[:, ix["Neck"]]) * 1.2
    return [(P[:, ix["Bip"]], P[:, ix["Spine2"]], 0.14), (P[:, ix["Spine2"]], P[:, ix["Neck"]], 0.15),
            (P[:, ix["Head"]], head_top, 0.10),
            (P[:, ix["L Thigh"]], P[:, ix["L Calf"]], 0.08), (P[:, ix["R Thigh"]], P[:, ix["R Calf"]], 0.08)]


def seg_dist(a0, a1, b0, b1):
    """closest distance between segments (vectorised over frames), plus the direction from b to a"""
    u, v, w0 = a1 - a0, b1 - b0, a0 - b0
    a = np.sum(u * u, -1); b = np.sum(u * v, -1); c = np.sum(v * v, -1); d = np.sum(u * w0, -1); e = np.sum(v * w0, -1)
    den = np.maximum(a * c - b * b, 1e-9)
    s = np.clip((b * e - c * d) / den, 0, 1)
    t = np.clip((a * s + e) / np.maximum(c, 1e-9), 0, 1)
    s = np.clip((b * t - d) / np.maximum(a, 1e-9), 0, 1)
    pa = a0 + u * s[:, None]; pb = b0 + v * t[:, None]
    dv = pa - pb
    return np.linalg.norm(dv, axis=-1), dv


def penetration(skA, PA, skB, PB):
    """per frame the worst capsule overlap depth (m, >0 = interpenetration) and its push direction (B away from A)"""
    worst = np.full(len(PA), -9.0); dirv = np.zeros((len(PA), 3))
    for a0, a1, ra in capsules(skA, PA):
        for b0, b1, rb in capsules(skB, PB):
            d, dv = seg_dist(a0, a1, b0, b1)
            depth = ra + rb - d
            m = depth > worst
            worst[m] = depth[m]
            dirv[m] = -dv[m]
    return worst, dirv


# ------------------------------------------------------------------------------------------------ main build
def build(partner, fps=25, verbose=True):
    t0, K = time_map()
    T_dance = DANCE_BEATS * BEAT
    tau_rel = RELEASE_BEAT * BEAT
    n_dance = int(round(T_dance * fps)) + 1                      # frame N-1 == frame 0 (cyclic)
    dt = T_dance / (n_dance - 1)
    tau_end = max(T_dance, tau_rel + FOLLOW_ON)
    pre = int(np.ceil(LOOPX / dt)) + 1
    taus = np.arange(-pre, int(np.ceil(tau_end / dt)) + 2) * dt  # includes a lead-in for the loop cross-fade
    ts = t0 + K * taus
    fsrc0 = int(np.floor(ts.min() * 30)) - 2
    fsrc1 = int(np.ceil(ts.max() * 30)) + 3
    Lp, Fp, Tp = sx.seq_paths(PAIR, SONG, TAKE)
    srcs = {"lead": rt.Source(Lp, slice(fsrc0, fsrc1)), "follow": rt.Source(Fp, slice(fsrc0, fsrc1))}
    skels = {"lead": rt.TodSkel("ram"), "follow": rt.TodSkel(partner)}
    ids = {"lead": "ram", "follow": partner}
    res = {}
    # ---- source in TOD axes, resampled
    SG, SP = {}, {}
    for r in ("lead", "follow"):
        P, R = resample(srcs[r], ts, fsrc0)
        src = srcs[r]
        src_PR = type("X", (), {})()
        src_PR.P, src_PR.R = P, R
        G, Pt = rt.source_world_tod(src_PR)
        SG[r], SP[r] = G, Pt
    # floor of each source dancer (dataset z-up -> TOD y): low percentile of the foot joints minus their rest height
    floor = {}
    for r in ("lead", "follow"):
        J = srcs[r].J
        ball_h = 0.5 * (J[sx.IDX["left_foot"], 1] + J[sx.IDX["right_foot"], 1]) - srcs[r].sole_y
        low = np.minimum(SP[r][:, sx.IDX["left_foot"], 1], SP[r][:, sx.IDX["right_foot"], 1])
        floor[r] = float(np.percentile(low, 5) - ball_h)
    # ---- pair frame: Ram's hip centre (xz) and heading at tau = 0
    i0 = pre
    hipL = 0.5 * (SP["lead"][:, 1] + SP["lead"][:, 2])
    fwd0 = SG["lead"][i0, 0] @ np.array([0, 0, 1.0])
    R_pair = Ry(-yaw_of(fwd0))
    org = np.array([hipL[i0, 0], 0, hipL[i0, 2]])
    for r in ("lead", "follow"):
        SP[r] = np.einsum("ab,fjb->fja", R_pair, SP[r] - org)
        SG[r] = np.einsum("ab,fjbc->fjac", R_pair, SG[r])
        SP[r][..., 1] -= floor[r]                               # source heights above its own floor
    # ---- scales
    sL = skels["lead"].leg_len() / srcs["lead"].leg_len()
    sF = skels["follow"].leg_len() / srcs["follow"].leg_len()
    span_t = skels["lead"].arm_len() + skels["follow"].arm_len()
    span_s = srcs["lead"].arm_len() + srcs["follow"].arm_len()
    s_rel = 0.5 * (span_t / span_s + 0.5 * (sL + sF))
    scale = {"lead": sL, "follow": sF}
    hip = {r: 0.5 * (SP[r][:, 1] + SP[r][:, 2]) for r in ("lead", "follow")}
    xz = {"lead": hip["lead"][:, [0, 2]] * sL}
    xz["follow"] = xz["lead"] + s_rel * (hip["follow"][:, [0, 2]] - hip["lead"][:, [0, 2]])
    # ---- rotations + root
    G, P, BIP = {}, {}, {}
    for r in ("lead", "follow"):
        sk = skels[r]
        J = srcs[r].J
        al = rt.rest_alignment(sk, J)
        G[r] = rt.retarget_rotations(sk, SG[r], al)
        G[r] = rt.canonical_arms(sk, G[r], np.zeros((len(G[r]), 3)))     # game arm convention: hinge elbow
        hip_rest = 0.5 * (J[1, 1] + J[2, 1]) - srcs[r].sole_y
        bip_stand = GROUND - (SOLE[ids[r]] - sk.Pb[sk.idx["Bip"]][1])
        y = bip_stand + scale[r] * (hip[r][:, 1] - hip_rest)
        BIP[r] = np.stack([xz[r][:, 0], y, xz[r][:, 1]], 1)
        P[r] = sk.positions(G[r], BIP[r])
    # ---- loop drift: the pair's wrapped yaw + translation over the 24 beats is spread linearly over the clip BEFORE
    # any contact fixing, so the feet are locked (and the hands placed) in the corrected frame and nothing slides
    n_tot = len(taus)
    iE = i0 + n_dance - 1
    fwd = np.einsum("fab,b->fa", G["lead"][:, skels["lead"].idx["Bip"]] @ skels["lead"].Gb[skels["lead"].idx["Bip"]].T, [0, 0, 1.0])
    yaw = np.unwrap(yaw_of(fwd))
    dyaw = (yaw[iE] - yaw[i0] + np.pi) % (2 * np.pi) - np.pi      # whole spins belong to the dance; only the rest drifts
    dpos = BIP["lead"][iE] - BIP["lead"][i0]; dpos[1] = 0
    O = BIP["lead"][i0] * np.array([1, 0, 1])
    e = Ry(-dyaw) @ (BIP["lead"][iE] - O) + O - BIP["lead"][i0]; e[1] = 0
    alpha = np.clip((np.arange(n_tot) - i0) / (n_dance - 1), 0, 1)
    Rc = np.stack([Ry(-ai * dyaw) for ai in alpha])
    for r in ("lead", "follow"):
        G[r] = np.einsum("fab,fjbc->fjac", Rc, G[r])
        BIP[r] = np.einsum("fab,fb->fa", Rc, BIP[r] - O) + O - alpha[:, None] * e
        P[r] = skels[r].positions(G[r], BIP[r])
        SP[r] = np.einsum("fab,fjb->fja", Rc, SP[r])          # source offsets follow the same rotation
    rel_i = i0 + int(round(tau_rel / dt))
    # ---- interpenetration: push the partner out along the contact normal (horizontal), smoothed
    depth, dirv = penetration(skels["lead"], P["lead"], skels["follow"], P["follow"])
    margin = 0.01
    push = np.zeros((len(taus), 3))
    m = depth > -margin
    dh = dirv.copy(); dh[:, 1] = 0
    dh /= np.maximum(np.linalg.norm(dh, axis=1, keepdims=True), 1e-9)
    push[m] = dh[m] * (depth[m] + margin)[:, None]
    ker = np.hanning(9); ker /= ker.sum()
    for c in (0, 2):
        # max-filter then smooth so the correction never undershoots a peak
        mx = np.array([push[max(0, k - 3):k + 4, c][np.argmax(np.abs(push[max(0, k - 3):k + 4, c]))] for k in range(len(push))])
        push[:, c] = np.convolve(mx, ker, mode="same")
    BIP["follow"] = BIP["follow"] + push
    P["follow"] = skels["follow"].positions(G["follow"], BIP["follow"])
    res["push_max_m"] = float(np.abs(push).max())
    # ---- feet: contacts from the source, balls locked on the ground
    feet_report, foot_segs = {}, {}
    for r in ("lead", "follow"):
        sk = skels[r]
        for side, S in (("left", "L"), ("right", "R")):
            ball = SP[r][:, sx.IDX[side + "_foot"]]
            sp = np.linalg.norm(np.gradient(ball, dt * K, axis=0), axis=1)        # source speed (m/s, source time)
            J = srcs[r].J
            ball_rest = J[sx.IDX[side + "_foot"], 1] - srcs[r].sole_y
            contact = (sp < 0.30) & (ball[:, 1] < ball_rest + 0.035)
            segs = segments(contact, fill=2, minlen=3)
            toe = sk.idx[S + " Toe0"]; ank = sk.idx[S + " Foot"]
            toe_h = (sk.Pb[toe][1] - sk.Pb[sk.idx["Bip"]][1]) - (SOLE[ids[r]] - sk.Pb[sk.idx["Bip"]][1])  # toe pivot above the sole
            lock = P[r][:, toe].copy()
            for a, b in segs:
                mid = (a + b) // 2
                lock[a:b] = P[r][mid, toe]
                lock[a:b, 1] = GROUND + max(toe_h, 0.0)
            w = ramp_weights(len(taus), segs, 2)
            # outside contacts: never below the ground
            below = np.maximum(0, (GROUND + max(toe_h, 0.0)) - P[r][:, toe, 1])
            goal = P[r][:, ank] + (lock - P[r][:, toe]) * (w > 0)[:, None]
            goal[:, 1] += below * (w == 0)
            wt = np.maximum(w, (below > 0).astype(float))
            P[r] = apply_leg_ik(sk, G[r], P[r], S, goal, wt, BIP[r])
            feet_report["%s_%s" % (r, S)] = {"segments": len(segs), "frames": int(sum(b - a for a, b in segs))}
            foot_segs[(r, S)] = segs
    # ---- hands: contacts from the source
    contacts = []
    hp_src = {(r, side): hand_point_src(SP[r], side) for r in ("lead", "follow") for side in ("left", "right")}
    for r, o in (("lead", "follow"), ("follow", "lead")):
        for side in ("left", "right"):
            h = hp_src[(r, side)]
            best = np.full(len(taus), 9.0); which = np.array([""] * len(taus), dtype=object)
            for oside in ("left", "right"):          # gripping palms: centres 12-20 cm apart in the source
                d = np.linalg.norm(h - hp_src[(o, oside)], axis=1) - HH_CONTACT
                m = d < best; best[m] = d[m]; which[m] = "hand:" + oside
            for jn, (piv, rad) in ANCHORS.items():
                d = np.linalg.norm(h - SP[o][:, sx.IDX[jn]], axis=1) - rad - HAND_R - BODY_GAP
                m = d < best; best[m] = d[m]; which[m] = "body:" + jn
            flag = best < 0
            # one segment per continuous contact; it keeps the anchor the hand is nearest to most of the time
            # (a hand on the back flickers between spine3 / spine2 / pelvis frame by frame)
            for a, b in segments(flag, fill=3, minlen=4):
                kinds, counts = np.unique(which[a:b][flag[a:b]], return_counts=True)
                kind = kinds[int(np.argmax(counts))]
                contacts.append({"who": r, "side": side, "on": kind, "segs": [(a, b)]})
    hh_lead = [c for c in contacts if c["on"].startswith("hand:") and c["who"] == "lead"]
    body = [c for c in contacts if c["on"].startswith("body:")]
    targets = {(r, S): (np.zeros((len(taus), 3)), np.zeros(len(taus))) for r in ("lead", "follow") for S in ("L", "R")}
    SIDE = {"left": "L", "right": "R"}
    # body contacts: the source offset from the touched joint, scaled to the touched body
    for c in body:
        r, o = c["who"], ("follow" if c["who"] == "lead" else "lead")
        S = SIDE[c["side"]]
        jn = c["on"].split(":")[1]
        piv = skels[o].idx[ANCHORS[jn][0]]
        off = (hp_src[(r, c["side"])] - SP[o][:, sx.IDX[jn]]) * scale[o]
        tgt = P[o][:, piv] + off
        w = ramp_weights(len(taus), c["segs"], 3)
        T, W = targets[(r, S)]
        m = w > W
        T[m] = tgt[m]; W[m] = w[m]
    # hand-hand: meet in the middle with the source's grip offset (scaled)
    for c in hh_lead:
        oside = c["on"].split(":")[1]
        a = hand_point_tod(skels["lead"], P["lead"], SIDE[c["side"]])
        b = hand_point_tod(skels["follow"], P["follow"], SIDE[oside])
        off = (hp_src[("lead", c["side"])] - hp_src[("follow", oside)]) * s_rel
        mid = 0.5 * (a + b)
        w = ramp_weights(len(taus), c["segs"], 3)
        for r, S, sgn in (("lead", SIDE[c["side"]], 0.5), ("follow", SIDE[oside], -0.5)):
            T, W = targets[(r, S)]
            m = w > W
            T[m] = (mid + sgn * off)[m]; W[m] = w[m]
    for (r, S), (T, W) in targets.items():
        P[r] = apply_arm_ik(skels[r], G[r], P[r], S, T, W, BIP[r])
    # grips: where one arm ran out of reach, the other hand comes the rest of the way (two alternating passes)
    for _ in range(2):
        for c in hh_lead:
            oside = c["on"].split(":")[1]
            Sl, Sf = SIDE[c["side"]], SIDE[oside]
            off = (hp_src[("lead", c["side"])] - hp_src[("follow", oside)]) * s_rel
            w = ramp_weights(len(taus), c["segs"], 3)
            a = hand_point_tod(skels["lead"], P["lead"], Sl)
            P["follow"] = apply_arm_ik(skels["follow"], G["follow"], P["follow"], Sf, a - off, w, BIP["follow"])
            b = hand_point_tod(skels["follow"], P["follow"], Sf)
            P["lead"] = apply_arm_ik(skels["lead"], G["lead"], P["lead"], Sl, b + off, w, BIP["lead"])
    # body contacts again: a touched elbow / shoulder may have moved with its own arm's IK above; hands that
    # are gripping a hand at that moment are left alone
    hh_w = {(r, S): np.zeros(len(taus)) for r in ("lead", "follow") for S in ("L", "R")}
    for c in hh_lead:
        w = ramp_weights(len(taus), c["segs"], 3)
        hh_w[("lead", SIDE[c["side"]])] = np.maximum(hh_w[("lead", SIDE[c["side"]])], w)
        hh_w[("follow", SIDE[c["on"].split(":")[1]])] = np.maximum(hh_w[("follow", SIDE[c["on"].split(":")[1]])], w)
    for c in body:
        r, o = c["who"], ("follow" if c["who"] == "lead" else "lead")
        S = SIDE[c["side"]]
        jn = c["on"].split(":")[1]
        piv = skels[o].idx[ANCHORS[jn][0]]
        off = (hp_src[(r, c["side"])] - SP[o][:, sx.IDX[jn]]) * scale[o]
        w = ramp_weights(len(taus), c["segs"], 3) * (1 - hh_w[(r, S)])
        P[r] = apply_arm_ik(skels[r], G[r], P[r], S, P[o][:, piv] + off, w, BIP[r])
    # ---- cross-fade the last LOOPX of the dance into the lead-in (tau in [-LOOPX, 0]) so frame N-1 == frame 0;
    # the follow-on (push / stumble) branches off at the release from the uncrossfaded timeline
    nx = int(round(LOOPX / dt))
    n_fo = int(round(FOLLOW_ON / dt)) + 1
    out = {}
    arm_rep = {}
    for r in ("lead", "follow"):
        # the IK above moved elbows and wrists with minimal rotations: back to the game's arm convention,
        # joint positions and hand rotations unchanged
        arm_rep[r] = {}
        G[r] = rt.canonical_arms(skels[r], G[r], BIP[r], report=arm_rep[r])
        Gd = G[r][i0:iE + 1].copy(); Bd = BIP[r][i0:iE + 1].copy()
        sk = skels[r]
        Lall = sk.local(G[r])
        for k in range(nx + 1):
            f = (n_dance - 1) - nx + k              # frame in the dance
            src_f = i0 - nx + k                     # lead-in frame (tau - T_dance)
            a = 0.5 - 0.5 * np.cos(np.pi * k / nx)
            # blend LOCAL rotations: world-space slerp of each bone on its own swung the arms through
            # unrelated poses (elbow flips in the loop seam); local blending keeps the hinge a hinge
            Lm = slerp_rows(Lall[i0 + f], Lall[src_f], np.full(Gd.shape[1], a))
            Gd[f] = sk.fk_local(Lm[None], sk.pos[None])[0][0]
            Bd[f] = (1 - a) * Bd[f] + a * BIP[r][src_f]
        Gd[-1] = G[r][i0]; Bd[-1] = BIP[r][i0]
        Gd = rt.canonical_arms(skels[r], Gd, Bd, cyclic=True)   # the cross-fade slerp leaves a tiny elbow twist
        fo = slice(rel_i, rel_i + n_fo)
        out[r] = {"G": Gd, "BIP": Bd, "P": skels[r].positions(Gd, Bd),
                  "foG": G[r][fo], "foB": BIP[r][fo], "foP": skels[r].positions(G[r][fo], BIP[r][fo]),
                  "inG": G[r][:i0 + 1], "inB": BIP[r][:i0 + 1]}       # source lead-in up to dance frame 0 (intro clips)
    res["foot_segs"] = foot_segs
    # ---- metrics
    rep = {"partner": partner, "fps": fps, "frames_dance": n_dance, "frames_follow_on": n_fo, "dt": dt,
           "t0_src": t0, "K_src_per_clip_s": K, "T_dance_s": T_dance, "release_frame": int(round(tau_rel / dt)),
           "release_s": tau_rel, "scales": {"lead": sL, "follow": sF, "relative": s_rel},
           "loop_drift": {"yaw_deg": float(np.degrees(dyaw)), "xz_m": [float(dpos[0]), float(dpos[2])]},
           "feet": feet_report, "push_max_m": res["push_max_m"], "arms_canonical": arm_rep}
    rep.update(metrics(skels, out, contacts, hp_src, SP, scale, s_rel, i0, iE, n_dance, i0, taus, dt))
    rep["contacts"] = [{"who": c["who"], "side": c["side"], "on": c["on"],
                        "segments_s": [[round((a - i0) * dt, 3), round((b - i0) * dt, 3)] for a, b in c["segs"]]}
                       for c in contacts]
    if verbose:
        print(json.dumps({k: v for k, v in rep.items() if k != "contacts"}, indent=1))
    return out, rep, skels


def metrics(skels, out, contacts, hp_src, SP, scale, s_rel, i0, iE, n_dance, _, taus, dt):
    """hand gaps while holding, feet slide / height while planted, interpenetration, fidelity to the source"""
    SIDE = {"left": "L", "right": "R"}
    Pl, Pf = out["lead"]["P"], out["follow"]["P"]
    rng = slice(i0, iE + 1)
    hand = []
    for c in contacts:
        if not c["on"].startswith("hand:") or c["who"] != "lead":
            continue
        a = hand_point_tod(skels["lead"], Pl, SIDE[c["side"]])
        b = hand_point_tod(skels["follow"], Pf, SIDE[c["on"].split(":")[1]])
        d = np.linalg.norm(a - b, axis=1)
        ds = np.linalg.norm(hp_src[("lead", c["side"])] - hp_src[("follow", c["on"].split(":")[1])], axis=1)[rng] * s_rel
        for s0, s1 in c["segs"]:
            k0, k1 = max(s0 - i0 + 3, 0), min(s1 - i0 - 3, n_dance - 1)
            if k1 > k0:
                hand.append({"hands": "lead %s - follow %s" % (c["side"], c["on"].split(":")[1]),
                             "t": [round(k0 * dt, 2), round(k1 * dt, 2)], "dist_cm_max": round(100 * float(d[k0:k1].max()), 1),
                             "dist_cm_mean": round(100 * float(d[k0:k1].mean()), 1),
                             "err_vs_source_grip_cm_max": round(100 * float(np.abs(d[k0:k1] - ds[k0:k1]).max()), 1)})
    body = []
    for c in contacts:
        if not c["on"].startswith("body:"):
            continue
        r = c["who"]; o = "follow" if r == "lead" else "lead"
        sk, so = skels[r], skels[o]
        jn = c["on"].split(":")[1]
        piv = so.idx[ANCHORS[jn][0]]
        Pr, Po = out[r]["P"], out[o]["P"]
        hpt = hand_point_tod(sk, Pr, SIDE[c["side"]])
        d = np.linalg.norm(hpt - Po[:, piv], axis=1)
        ds = np.linalg.norm(hp_src[(r, c["side"])] - SP[o][:, sx.IDX[jn]], axis=1)[rng] * scale[o]
        for s0, s1 in c["segs"]:
            k0, k1 = max(s0 - i0 + 3, 0), min(s1 - i0 - 3, n_dance - 1)
            if k1 > k0:
                body.append({"hand": "%s %s on %s %s" % (r, c["side"], o, jn), "t": [round(k0 * dt, 2), round(k1 * dt, 2)],
                             "err_cm_max": round(100 * float(np.abs(d[k0:k1] - ds[k0:k1]).max()), 1)})
    feet = {}
    for r in ("lead", "follow"):
        sk = skels[r]
        for S in ("L", "R"):
            toe = out[r]["P"][:, sk.idx[S + " Toe0"]]
            sp = np.linalg.norm(np.diff(toe, axis=0), axis=1) / dt
            low = toe[:-1, 1] < GROUND + 0.02
            planted = low & (sp < 0.2)
            feet["%s_%s" % (r, S)] = {"lowest_above_ground_cm": round(100 * float(toe[:, 1].min() - GROUND), 1),
                                      "planted_frames": int(planted.sum()),
                                      "planted_slide_mm_per_frame_mean": round(1000 * float((sp[planted] * dt).mean()) if planted.any() else 0, 1)}
    depth, _ = penetration(skels["lead"], Pl, skels["follow"], Pf)
    return {"hand_hand": hand, "hand_body": body, "feet_after": feet,
            "penetration_cm_max": round(100 * float(depth.max()), 1),
            "penetration_frames_over_2cm": int((depth > 0.02).sum()),
            "pelvis_distance_m": [round(float(np.linalg.norm((Pl[:, 1] - Pf[:, 1])[:, [0, 2]], axis=1).min()), 2),
                                  round(float(np.linalg.norm((Pl[:, 1] - Pf[:, 1])[:, [0, 2]], axis=1).max()), 2)]}


def save(out, rep, partner, fps, path=None):
    path = path or os.path.join(WORK, "pair_%s_%d.npz" % (partner, fps))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    np.savez_compressed(path, **{"%s_%s" % (r, k): v for r in out for k, v in out[r].items()})
    json.dump(rep, open(path.replace(".npz", ".json"), "w"), indent=1)
    return path


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("partner")
    ap.add_argument("--fps", type=int, default=25)
    a = ap.parse_args()
    o, r, _ = build(a.partner, a.fps)
    print(save(o, r, a.partner, a.fps))
