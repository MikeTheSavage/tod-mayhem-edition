"""intro.py - the Suave activation intro: suave_intro_<pairing> (Ram) + suave_grabbed_<pairing> (partner).

  python intro.py PARTNER [--fps 25]      -> ../work/intro_<partner>_<fps>.npz + .json

6 beats of Suavemente (2.899 s), clip time 0 = the music's first downbeat; the last frame IS the dance's frame 0 of the
same pairing (lead / follow), so the slot switch at beat 6 needs no blend. Same shared frame as the dance (both nodes at
Ram's node; Bip carries the offset).

Beats (design doc, "a longer, clearer activation"):
  Ram     0-2  raises both hands, palms out ("easy now, here we go"), hips start to move on the beat
          2-4  steps in (right, left), the left hand reaches for the partner's right hand, takes it on beat 4
          4-6  pulls the partner in, right hand onto the partner's back -> the dance's open-hold frame 0
  partner 0-2  turns toward Ram in surprise (two pivot steps), shoulders up, hands up by the chest
          2-4  frozen in disbelief, the right hand drifts up and is taken on beat 4
          4-6  pulled in: two stumbling steps forward, leaning in -> the dance's frame 0
Built procedurally on the game's own standing pose (ram_idle_breathe_aware frame 0), with 2-bone IK for hands and feet
(foot schedule: planted feet do not move, swing feet arc 7 cm), then the last 0.36 s blends in local space into the
dance's own source lead-in (pair npz inG/inB) so the motion enters the dance with the dance's velocity. Arms in the
game convention (retarget.canonical_arms).
"""
import argparse, json, os, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot

sys.dont_write_bytecode = True
import retarget as rt
import pair as pr
import armtwist as at
import todclips

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "work")
BEATS = 6
B = pr.BEAT


def ss(x, a, b):
    """smoothstep of x from a to b (0..1)"""
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def Ry(a):
    return pr.Ry(a)


def keyed(b, keys):
    """piecewise smoothstep through keys [(beat, value)...] (values: floats or arrays)"""
    if b <= keys[0][0]:
        return np.array(keys[0][1], float)
    for (b0, v0), (b1, v1) in zip(keys[:-1], keys[1:]):
        if b <= b1:
            s = ss(b, b0, b1)
            return (1 - s) * np.array(v0, float) + s * np.array(v1, float)
    return np.array(keys[-1][1], float)


def slerp1(R0, R1, a):
    return pr.slerp_rows(R0[None], R1[None], np.array([a]))[0]


def hand_frame(sk, S, finger_dir_w, palm_w):
    """hand world rotation with the fingers along finger_dir_w and the palm facing palm_w"""
    h = sk.idx[S + " Hand"]
    fd = sk.Gb[h].T @ (0.5 * (sk.Pb[sk.idx[S + " Finger1"]] + sk.Pb[sk.idx[S + " Finger2"]]) - sk.Pb[h])
    fd /= np.linalg.norm(fd)
    f11, f1 = sk.idx[S + " Finger11"], sk.idx[S + " Finger1"]
    tip = sk.Pb[f11] + sk.Gb[f11] @ np.array([-0.05, 0, 0])
    straight = sk.Pb[f11] + (sk.Pb[f11] - sk.Pb[f1]) / np.linalg.norm(sk.Pb[f11] - sk.Pb[f1]) * 0.05
    pn = sk.Gb[h].T @ (tip - straight)
    pn -= fd * np.dot(pn, fd); pn /= np.linalg.norm(pn)
    fw = finger_dir_w / np.linalg.norm(finger_dir_w)
    pw = palm_w - fw * np.dot(palm_w, fw); pw /= np.linalg.norm(pw)
    return rt._frame_from(fd, pn, fw, pw)


class Actor:
    """one character's procedural pose builder (TOD pair frame)"""

    def __init__(self, ident, idle_clip, bip_y):
        self.sk = sk = rt.TodSkel(ident)
        G, P = at.clip_world(sk, idle_clip)
        self.L0 = sk.local(G[:1])[0]
        self.Gbip0 = G[0, sk.idx["Bip"]]
        self.bip_y = bip_y
        # idle feet relative to the root (yaw 0), foot / toe world rotations at yaw 0
        bip = P[0, sk.idx["Bip"]]
        self.toe_rel = {S: P[0, sk.idx[S + " Toe0"]] - bip * [1, 0, 1] for S in ("L", "R")}
        self.Gfoot0 = {S: G[0, sk.idx[S + " Foot"]] for S in ("L", "R")}
        self.Ltoe0 = {S: self.L0[sk.idx[S + " Toe0"]] for S in ("L", "R")}
        self.idle_wrist_rel = {S: P[0, sk.idx[S + " Hand"]] - bip for S in ("L", "R")}
        self.Ghand0 = {S: G[0, sk.idx[S + " Hand"]] for S in ("L", "R")}

    def body(self, xz, yaw, dy, lean=0.0, head=0.0, shrug=0.0, sway=0.0):
        """world G, P of the idle pose at root (xz, yaw), Bip y = idle + dy, with spine lean (+ = forward),
        head pitch (+ = down), shoulder shrug (+ = up) and side sway of the upper body (+ = to the right)"""
        sk = self.sk
        L = self.L0.copy()
        L[sk.idx["Bip"]] = Ry(yaw) @ self.Gbip0
        pos = sk.pos.copy()
        pos[sk.idx["Bip"]] = [xz[0], self.bip_y + dy, xz[1]]
        G, _ = sk.fk_local(L[None], pos[None])
        G = G[0]
        R = Ry(yaw)
        right, fwd = R @ np.array([1.0, 0, 0]), R @ np.array([0, 0, 1.0])
        bip = pos[sk.idx["Bip"]]

        def rot_sub(root_name, R_):
            for b in pr.subtree(sk, sk.idx[root_name]):
                G[b] = R_ @ G[b]
        if lean or sway:
            rot_sub("Spine1", Rot.from_rotvec(np.radians(lean) * right - np.radians(sway) * fwd).as_matrix())
        if head:
            rot_sub("Neck", Rot.from_rotvec(np.radians(head) * right).as_matrix())
        if shrug:
            for S in ("L", "R"):
                P = sk.positions(G[None], bip[None])[0]
                ua = P[sk.idx[S + " UpperArm"]].copy()
                Rs = Rot.from_rotvec(np.radians(shrug) * fwd).as_matrix()
                cl = sk.idx[S + " Clavicle"]
                piv = P[cl]
                if (Rs @ (ua - piv))[1] < (ua - piv)[1]:
                    Rs = Rs.T
                rot_sub(S + " Clavicle", Rs)
        return G, bip

    def place_feet(self, G, bip, toes, foot_rots):
        sk = self.sk
        for S in ("L", "R"):
            fo, toe = sk.idx[S + " Foot"], sk.idx[S + " Toe0"]
            G[fo] = foot_rots[S]
            G[toe] = foot_rots[S] @ self.Ltoe0[S]
        P = sk.positions(G[None], bip[None])
        # hips down where a leg cannot reach
        drop = 0.0
        for S in ("L", "R"):
            fo, toe, th = sk.idx[S + " Foot"], sk.idx[S + " Toe0"], sk.idx[S + " Thigh"]
            goal = toes[S] - (P[0, toe] - P[0, fo])
            Lleg = 0.995 * (sk.chain_len(S + " Thigh", S + " Calf") + sk.chain_len(S + " Calf", S + " Foot"))
            dv = P[0, th] - goal
            ymax = np.sqrt(max(Lleg ** 2 - dv[0] ** 2 - dv[2] ** 2, 0))
            drop = max(drop, dv[1] - ymax)
        if drop > 0:
            bip = bip - [0, drop, 0]
            P = sk.positions(G[None], bip[None])
        for S in ("L", "R"):
            fo, toe = sk.idx[S + " Foot"], sk.idx[S + " Toe0"]
            goal = toes[S] - (P[0, toe] - P[0, fo])
            P = pr.apply_leg_ik(sk, G[None], P, S, goal[None], np.ones(1), bip[None])
        return G, bip, drop

    def place_hand(self, G, bip, S, hp_goal, Rhand, pole_dir, w=1.0):
        """hand world rotation Rhand (blended by w from the current) and hand point to hp_goal (2-bone IK)"""
        sk = self.sk
        ua, fa, ha = sk.idx[S + " UpperArm"], sk.idx[S + " Forearm"], sk.idx[S + " Hand"]
        Rn = slerp1(G[ha], Rhand, w)
        corr = Rn @ G[ha].T
        for b in pr.subtree(sk, ha):
            G[b] = corr @ G[b]
        P = sk.positions(G[None], bip[None])[0]
        hp = pr.hand_point_tod(sk, P[None], S)[0]
        goal = hp + w * (hp_goal - hp)
        wrist_goal = goal - (hp - P[ha])
        Ra, Rb, _ = rt.two_bone_ik(P[ua], P[fa], P[ha], wrist_goal, P[ua] + pole_dir)
        Gh = G[ha].copy()
        for b in pr.subtree(sk, ua):
            G[b] = Ra @ G[b]
        for b in pr.subtree(sk, fa):
            G[b] = Rb @ G[b]
        corr = Gh @ G[ha].T
        for b in pr.subtree(sk, ha):
            G[b] = corr @ G[b]
        return G


def foot_track(b, plan, start):
    """plan: list of (b0, b1, target_pos, target_rot) for one foot; returns pos, rot at beat b (planted between)"""
    pos, rot = start
    for b0, b1, tp, tr in plan:
        if b <= b0:
            break
        s = ss(b, b0, b1)
        if b < b1:
            p = (1 - s) * pos + s * tp
            p = p + np.array([0, 0.07 * np.sin(np.pi * s), 0])
            return p, slerp1(rot, tr, s)
        pos, rot = tp, tr
    return pos, rot


def yaw_of_rot(Gbip, sk):
    fwd = (Gbip @ sk.Gb[sk.idx["Bip"]].T) @ np.array([0, 0, 1.0])
    return float(np.arctan2(fwd[0], fwd[2]))


def build(partner, fps=25, verbose=True):
    d = np.load(os.path.join(WORK, "pair_%s_%d.npz" % (partner, fps)))
    prep = json.load(open(os.path.join(WORK, "pair_%s_%d.json" % (partner, fps))))
    dt_pair = prep["dt"]
    n = int(round(BEATS * B * fps)) + 1
    dt = BEATS * B / (n - 1)
    beats = np.arange(n) * dt / B
    idle = todclips.get_clip("ram_idle_breathe_aware")
    ids = {"lead": "ram", "follow": partner}
    act = {}
    for r in ("lead", "follow"):
        sk = rt.TodSkel(ids[r])
        bip_stand = pr.GROUND - (pr.SOLE[ids[r]] - sk.Pb[sk.idx["Bip"]][1])
        bip_stand_ram = pr.GROUND - (pr.SOLE["ram"] - rt.TodSkel("ram").Pb[rt.TodSkel("ram").idx["Bip"]][1])
        act[r] = Actor(ids[r], idle, bip_stand + (-0.016 - bip_stand_ram))
    # ---- the lead-in (the dance's source frames up to its frame 0)
    inG = {r: d[r + "_inG"] for r in act}; inB = {r: d[r + "_inB"] for r in act}
    T_in = (len(inG["lead"]) - 1) * dt_pair
    b_in = BEATS - T_in / B                                   # beat where the blend into the lead-in starts
    inP = {r: act[r].sk.positions(inG[r], inB[r]) for r in act}
    sk_l, sk_f = act["lead"].sk, act["follow"].sk
    # targets at the lead-in start
    R_in = {r: inB[r][0] for r in act}
    yaw_in = {r: yaw_of_rot(inG[r][0, act[r].sk.idx["Bip"]], act[r].sk) for r in act}
    toes_in = {r: {S: inP[r][0, act[r].sk.idx[S + " Toe0"]] for S in ("L", "R")} for r in act}
    feet_in = {r: {S: inG[r][0, act[r].sk.idx[S + " Foot"]] for S in ("L", "R")} for r in act}
    hp_in = {r: {S: pr.hand_point_tod(act[r].sk, inP[r][:1], S)[0] for S in ("L", "R")} for r in act}
    hand_in = {r: {S: inG[r][0, act[r].sk.idx[S + " Hand"]] for S in ("L", "R")} for r in act}
    grip_off = hp_in["lead"]["L"] - hp_in["follow"]["R"]          # Ram's left hand holds the partner's right hand
    grip_mid_in = 0.5 * (hp_in["lead"]["L"] + hp_in["follow"]["R"])
    sp2 = sk_f.idx["Spine2"]
    back_rel = inG["follow"][0, sp2].T @ (hp_in["lead"]["R"] - inP["follow"][0, sp2])   # Ram's right hand on the back
    # ---- Ram's path: starts 0.42 m back (his own facing = +z of the pair frame), steps in on beats 2-3.6
    yaw_R0 = 0.0
    startR = np.array([R_in["lead"][0], R_in["lead"][2] - 0.42])
    endR = np.array([R_in["lead"][0], R_in["lead"][2]])
    # ---- partner: starts 0.30 m further out, turned 110 degrees away; turns to Ram on beats 0.4-1.6
    away = (R_in["follow"] - R_in["lead"])[[0, 2]]; away /= np.linalg.norm(away)
    endF = R_in["follow"][[0, 2]]
    startF = endF + 0.30 * away
    face_ram = float(np.arctan2(*(endR - startF)))           # yaw looking at Ram (atan2(x, z))
    yaw_F0 = face_ram + np.radians(110)
    stance = lambda a, xz, yaw: {S: np.array([xz[0], 0, xz[1]]) + Ry(yaw) @ (a.toe_rel[S] * [1, 0, 1]) + [0, pr.GROUND, 0] for S in ("L", "R")}
    def flat(a, yaw):
        return {S: Ry(yaw) @ a.Gfoot0[S] for S in ("L", "R")}
    for a in act.values():
        th = {S: a.toe_rel[S][1] for S in ("L", "R")}
    toeh = {r: {S: act[r].toe_rel[S][1] - pr.GROUND for S in ("L", "R")} for r in act}   # toe pivot above ground (idle)
    def lift(a_r, st):
        return {S: st[S] + [0, toeh[a_r][S], 0] for S in ("L", "R")}
    stR0 = lift("lead", stance(act["lead"], startR, yaw_R0))
    stF0 = lift("follow", stance(act["follow"], startF, yaw_F0))
    stF1 = lift("follow", stance(act["follow"], startF, face_ram))
    planR = {"R": [(2.0, 2.8, toes_in["lead"]["R"], feet_in["lead"]["R"])],
             "L": [(2.8, 3.6, toes_in["lead"]["L"], feet_in["lead"]["L"])]}
    planF = {"L": [(0.4, 1.0, stF1["L"], flat(act["follow"], face_ram)["L"]),
                   (4.7, 5.3, toes_in["follow"]["L"], feet_in["follow"]["L"])],
             "R": [(1.0, 1.6, stF1["R"], flat(act["follow"], face_ram)["R"]),
                   (4.1, 4.7, toes_in["follow"]["R"], feet_in["follow"]["R"])]}
    startfeet = {"lead": (stR0, flat(act["lead"], yaw_R0)), "follow": (stF0, flat(act["follow"], yaw_F0))}
    Gout = {r: np.zeros((n, len(act[r].sk.names), 3, 3)) for r in act}
    Bout = {r: np.zeros((n, 3)) for r in act}
    drops = {r: 0.0 for r in act}
    for k, b in enumerate(beats):
        # ================= Ram
        a = act["lead"]; sk = a.sk
        xz = (1 - ss(b, 2.0, 3.6)) * startR + ss(b, 2.0, 3.6) * endR
        bounce = -0.02 * (0.5 - 0.5 * np.cos(2 * np.pi * b)) * (1 - ss(b, 4.5, b_in))
        sway = 4.0 * np.sin(np.pi * b) * (1 - ss(b, 2.0, 2.6))                  # hips/upper body to a side on each beat
        lean = keyed(b, [(0.3, 0), (1.2, -6), (2.2, -6), (3.3, 9), (4.0, 9), (4.7, -5), (b_in, 0)])
        G, bip = a.body(xz, yaw_R0, bounce, lean=float(lean), sway=float(sway))
        bip = bip + Ry(yaw_R0) @ np.array([0.03 * np.sin(np.pi * b) * (1 - ss(b, 2.0, 2.6)), 0, 0])
        toes, rots = {}, {}
        for S in ("L", "R"):
            toes[S], rots[S] = foot_track(b, planR[S], (startfeet["lead"][0][S], startfeet["lead"][1][S]))
        G, bip, dr = a.place_feet(G, bip, toes, rots); drops["lead"] = max(drops["lead"], dr)
        P = sk.positions(G[None], bip[None])[0]
        R = Ry(yaw_R0)
        fwd, right, up = R @ [0, 0, 1.0], R @ [1.0, 0, 0], np.array([0, 1.0, 0])
        # flourish: hands up by the head, palms out to the partner, a small shake on each beat
        w_fl = ss(b, 0.15, 1.1) * (1 - ss(b, 2.0, 3.0))
        shake = 0.03 * np.sin(np.pi * b)
        # partner's root (for the reach / meeting point) at beat 4
        mF = startF
        M = 0.5 * (np.array([endR[0], 0, endR[1]]) + np.array([mF[0], 0, mF[1]])) + [0, bip[1] + 0.22, 0]
        for S, sgn in (("L", -1), ("R", 1)):
            sh = P[sk.idx[S + " UpperArm"]]
            fl_goal = sh + sgn * 0.20 * right + 0.22 * up + 0.24 * fwd + shake * right
            fl_rot = hand_frame(sk, S, up + 0.15 * sgn * right, fwd + 0.25 * sgn * right)
            G = a.place_hand(G, bip, S, fl_goal, fl_rot, sgn * 0.5 * right - 0.5 * up - 0.15 * fwd, w_fl)
        P = sk.positions(G[None], bip[None])[0]
        # left: reach for the partner's right hand (beats 2.2-4), then the grip moves to the lead-in's grip
        w_reach = ss(b, 2.2, 3.6)
        if w_reach > 0:
            g = ss(b, 4.0, b_in)
            grip_mid = (1 - g) * M + g * grip_mid_in
            tgt = grip_mid + 0.5 * grip_off
            rot = hand_in["lead"]["L"]
            G = a.place_hand(G, bip, "L", tgt, rot, -0.6 * right - 0.6 * up, w_reach)
        # right: from the flourish down to the side (2-3), then onto the partner's back (4-5.4)
        P = sk.positions(G[None], bip[None])[0]
        w_back = ss(b, 3.9, b_in - 0.15)
        if b >= 2.0:
            side = P[sk.idx["R UpperArm"]] + 0.12 * right - 0.50 * up + 0.10 * fwd
            Gb_ = Gout["follow"][k - 1] if k > 0 else None
            if w_back > 0 and Gb_ is not None:
                Pf = act["follow"].sk.positions(Gb_[None], Bout["follow"][k - 1][None])[0]
                back = Pf[sp2] + Gb_[sp2] @ back_rel
                tgt = (1 - w_back) * side + w_back * back
            else:
                tgt = side
            rot = slerp1(a.Ghand0["R"], hand_in["lead"]["R"], w_back)
            G = a.place_hand(G, bip, "R", tgt, rot, 0.6 * right - 0.6 * up, ss(b, 2.0, 2.8))
        Gout["lead"][k] = G; Bout["lead"][k] = bip
        # ================= partner
        a = act["follow"]; sk = a.sk
        yaw = yaw_F0 + (face_ram - yaw_F0) * ss(b, 0.4, 1.6)
        yaw = yaw + (yaw_in["follow"] - face_ram + np.pi) % (2 * np.pi) - np.pi if False else yaw
        pull = ss(b, 4.1, 5.3)
        dyaw = ((yaw_in["follow"] - face_ram + np.pi) % (2 * np.pi)) - np.pi
        yaw = yaw + pull * dyaw
        xz = (1 - pull) * startF + pull * endF
        lean = keyed(b, [(0.3, 0), (1.0, -8), (2.5, -5), (3.3, -4), (3.9, 8), (4.5, 14), (5.2, 6), (b_in, 0)])
        head = keyed(b, [(0.3, 0), (1.0, -10), (2.5, -6), (4.0, -4), (4.6, 6), (b_in, 0)])
        shrug = keyed(b, [(0.3, 0), (1.0, 9), (3.5, 6), (4.4, 0)])
        dy = -0.03 * ss(b, 0.5, 1.2) * (1 - ss(b, 3.5, 4.5)) - 0.04 * np.sin(np.pi * ss(b, 4.1, 5.3))
        G, bip = a.body(xz, yaw, dy, lean=float(lean), head=float(head), shrug=float(shrug))
        toes, rots = {}, {}
        for S in ("L", "R"):
            toes[S], rots[S] = foot_track(b, planF[S], (startfeet["follow"][0][S], startfeet["follow"][1][S]))
        G, bip, dr = a.place_feet(G, bip, toes, rots); drops["follow"] = max(drops["follow"], dr)
        P = sk.positions(G[None], bip[None])[0]
        R = Ry(yaw)
        fwd, right, up = R @ [0, 0, 1.0], R @ [1.0, 0, 0], np.array([0, 1.0, 0])
        w_sur = ss(b, 0.5, 1.3)
        for S, sgn in (("L", -1), ("R", 1)):
            sh = P[sk.idx[S + " UpperArm"]]
            sur_goal = sh + sgn * 0.02 * right - 0.16 * up + 0.30 * fwd
            sur_rot = hand_frame(sk, S, up + 0.3 * fwd - 0.2 * sgn * right, fwd)
            wS = w_sur * (1 - ss(b, 3.0, 4.0)) if S == "R" else w_sur * (1 - ss(b, 4.3, b_in))
            G = a.place_hand(G, bip, S, sur_goal, sur_rot, sgn * 0.4 * right - 0.7 * up, wS)
        # right hand: offered / taken on beat 4, then held in Ram's grip
        P = sk.positions(G[None], bip[None])[0]
        w_take = ss(b, 3.0, 4.0)
        if w_take > 0:
            g = ss(b, 4.0, b_in)
            tgt = (1 - g) * M + g * grip_mid_in - 0.5 * grip_off
            if b > 4.0:      # follow Ram's actual hand (the grip)
                PR = act["lead"].sk.positions(Gout["lead"][k][None], Bout["lead"][k][None])
                tgt = pr.hand_point_tod(act["lead"].sk, PR, "L")[0] - grip_off
            G = a.place_hand(G, bip, "R", tgt, hand_in["follow"]["R"], 0.4 * right - 0.7 * up, w_take)
        Gout["follow"][k] = G; Bout["follow"][k] = bip
    # ---- last T_in: local-space blend into the lead-in (the dance's own approach to its frame 0)
    for r in act:
        sk = act[r].sk
        Lin = sk.local(inG[r])
        La = sk.local(Gout[r])
        for k, b in enumerate(beats):
            if b < b_in:
                continue
            u = (b - b_in) * B / dt_pair
            i = min(int(np.floor(u)), len(Lin) - 2); fr = u - i
            Li = pr.slerp_rows(Lin[i], Lin[i + 1], np.full(len(sk.names), fr))
            Bi = (1 - fr) * inB[r][i] + fr * inB[r][i + 1]
            w = ss(b, b_in, BEATS)
            Lm = pr.slerp_rows(La[k], Li, np.full(len(sk.names), w))
            pos = sk.pos.copy(); pos[sk.idx["Bip"]] = (1 - w) * Bout[r][k] + w * Bi
            Gout[r][k] = sk.fk_local(Lm[None], pos[None])[0][0]
            Bout[r][k] = pos[sk.idx["Bip"]]
        # the last frame IS the dance's frame 0
        end_err = float(np.abs(Gout[r][-1] - d[r + "_G"][0]).max())
        Gout[r][-1] = d[r + "_G"][0]; Bout[r][-1] = d[r + "_BIP"][0]
    # ---- contact pass: Ram's left hand holds the partner's right hand from beat 4 (both arms meet halfway, two
    # passes), Ram's right hand settles on the partner's back; released into the lead-in's own contacts at the end
    wg = np.array([ss(b, 3.6, 4.0) * (1 - ss(b, 5.6, 6.0)) for b in beats])
    wb = np.array([ss(b, 4.2, 5.0) * (1 - ss(b, 5.6, 6.0)) for b in beats])
    for _ in range(3):
        PL = sk_l.positions(Gout["lead"], Bout["lead"]); PF = sk_f.positions(Gout["follow"], Bout["follow"])
        mid = 0.5 * (pr.hand_point_tod(sk_l, PL, "L") + pr.hand_point_tod(sk_f, PF, "R"))
        pr.apply_arm_ik(sk_f, Gout["follow"], PF, "R", mid - 0.5 * grip_off, wg, Bout["follow"])
        PF = sk_f.positions(Gout["follow"], Bout["follow"])
        PL = pr.apply_arm_ik(sk_l, Gout["lead"], PL, "L", pr.hand_point_tod(sk_f, PF, "R") + grip_off, wg, Bout["lead"])
        back = PF[:, sp2] + np.einsum("fab,b->fa", Gout["follow"][:, sp2], back_rel)
        pr.apply_arm_ik(sk_l, Gout["lead"], PL, "R", back, wb, Bout["lead"])
    for r in act:
        last = Gout[r][-1].copy()
        Gout[r] = rt.canonical_arms(act[r].sk, Gout[r], Bout[r])
        Gout[r][-1] = last
    out, rep = {}, {"partner": partner, "fps": fps, "frames": n, "dt": dt, "beats": BEATS,
                    "blend_into_leadin_from_beat": round(b_in, 3), "hips_dropped_for_reach_cm": {r: round(100 * v, 1) for r, v in drops.items()}}
    for r in act:
        sk = act[r].sk
        P = sk.positions(Gout[r], Bout[r])
        out[r] = {"G": Gout[r], "BIP": Bout[r], "P": P}
        # checks: feet (slide while planted), arms, continuity into the dance
        feet = {}
        for S in ("L", "R"):
            t = P[:, sk.idx[S + " Toe0"]]
            sp = np.linalg.norm(np.diff(t, axis=0), axis=1)
            low = t[:-1, 1] < pr.GROUND + 0.03
            feet[S] = {"lowest_cm": round(100 * float(t[:, 1].min() - pr.GROUND), 1),
                       "planted_slide_mm_per_frame_max": round(1000 * float(sp[low & (sp < 0.01)].max() if (low & (sp < 0.01)).any() else 0), 2)}
        Pd = sk.positions(d[r + "_G"][:3], d[r + "_BIP"][:3])
        v_intro = np.linalg.norm(P[-1] - P[-2], axis=1).max() / dt
        v_dance = np.linalg.norm(Pd[1] - Pd[0], axis=1).max() / dt_pair
        rep[r] = {"feet": feet, "arms": at.clip_report(sk, ids[r], Gout[r], Bout[r], dt),
                  "joint_speed_into_dance_m_s": [round(float(v_intro), 2), round(float(v_dance), 2)]}
    PL = act["lead"].sk.positions(Gout["lead"], Bout["lead"]); PF = act["follow"].sk.positions(Gout["follow"], Bout["follow"])
    grip = np.linalg.norm(pr.hand_point_tod(sk_l, PL, "L") - pr.hand_point_tod(sk_f, PF, "R"), axis=1)
    g0 = float(np.linalg.norm(grip_off))
    k4 = int(np.searchsorted(beats, 4.0))
    rep["grip_cm_from_beat4"] = {"target": round(100 * g0, 1), "min": round(100 * float(grip[k4:].min()), 1),
                                 "max": round(100 * float(grip[k4:].max()), 1)}
    depth, _ = pr.penetration(sk_l, PL, sk_f, PF)
    rep["penetration_cm_max"] = round(100 * float(depth.max()), 1)
    rep["pelvis_distance_m_start_end"] = [round(float(np.linalg.norm((PL[0, 1] - PF[0, 1])[[0, 2]])), 2),
                                          round(float(np.linalg.norm((PL[-1, 1] - PF[-1, 1])[[0, 2]])), 2)]
    if verbose:
        print(json.dumps({k: v for k, v in rep.items()}, indent=1, default=float))
    return out, rep


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("partner")
    ap.add_argument("--fps", type=int, default=25)
    a = ap.parse_args()
    out, rep = build(a.partner, a.fps)
    path = os.path.join(WORK, "intro_%s_%d.npz" % (a.partner, a.fps))
    np.savez_compressed(path, **{"%s_%s" % (r, k): v for r in out for k, v in out[r].items()})
    json.dump(rep, open(path.replace(".npz", ".json"), "w"), indent=1, default=float)
    print(path)
