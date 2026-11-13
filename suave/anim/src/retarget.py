"""retarget.py - SMPL-X joint motion -> TOD 50-pivot Biped, in the engine's own convention.

TOD model space (docs: animation replacement section 5): +y up, +z forward, -x = the character's LEFT, i.e. a
left-handed frame. FK: world_b = world_parent * T(p_b) * R(conj(q_b)), q_b the engine's raw quaternion.
SMPL-X rest space: +y up, +z forward, +x = the body's LEFT (right-handed). So SMPL-X rest coordinates map to TOD rest
coordinates by the mirror M = diag(-1, 1, 1) and every SMPL "left_*" joint drives the TOD "L *" pivot (verified
numerically by check_sides(): TOD L pivots sit at -x in the bind pose; the retargeted L Hand follows the source
left wrist on the same side of the pelvis in every frame).

Dataset world (CoMPAS3D) is z-up and right-handed. A = M_world maps it to TOD axes: (x, y, z) -> (-x, z, -y)
... composed as A = M @ Rx(-90): (x, y, z) -> (x, z, -y) then x -> -x.
A source joint's world rotation G (dataset) becomes G_tod = A @ G @ M (proper rotation: det A = det M = -1).

Per mapped bone b <- joint j (child joint c for its direction):
  G_rest'_b = rotmin(d_bind_b, M d_rest_jc) @ G_bind_b     (target posed into the source's T-pose)
  G_b(t)    = G_tod_j(t) @ G_rest'_b
so each target bone points exactly where its source bone points, with the source's twist. Unmapped bones (face,
GamePivot) keep their bind local rotation; twist bones follow their forearm.
"""
import json, os
import numpy as np
from scipy.spatial.transform import Rotation as Rot

import smplx_fk as sx

HERE = os.path.dirname(os.path.abspath(__file__))
CLIPS = os.path.join(os.path.dirname(HERE), "clips")

M3 = np.diag([-1.0, 1.0, 1.0])
A3 = M3 @ np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], dtype=float)    # dataset z-up -> TOD

# TOD pivot <- (SMPL joint, SMPL child joint for the direction or None)
MAP = {
    "Bip": ("pelvis", None), "Pelvis": ("pelvis", None),
    "Spine": ("spine1", None), "Spine1": ("spine2", None), "Spine2": ("spine3", None), "Spine3": ("spine3", None),
    "Neck": ("neck", None), "Head": ("head", None),
}
for side, S in (("left", "L"), ("right", "R")):
    MAP.update({
        S + " Clavicle": (side + "_collar", side + "_shoulder"),
        S + " UpperArm": (side + "_shoulder", side + "_elbow"),
        S + " Forearm": (side + "_elbow", side + "_wrist"),
        S + " Hand": (side + "_wrist", side + "_middle1"),
        S + " Finger0": (side + "_thumb1", side + "_thumb2"),
        S + " Finger01": (side + "_thumb2", side + "_thumb3"),
        S + " Finger1": (side + "_index1", side + "_index2"),
        S + " Finger11": (side + "_index2", side + "_index3"),
        S + " Finger2": (side + "_ring1", side + "_ring2"),
        S + " Finger21": (side + "_ring2", side + "_ring3"),
        S + " Thigh": (side + "_hip", side + "_knee"),
        S + " Calf": (side + "_knee", side + "_ankle"),
        S + " Foot": (side + "_ankle", side + "_foot"),
        S + " Toe0": (side + "_foot", None),
    })
FOLLOW = {"L ForeTwist": "L Forearm", "L ForeTwist1": "L Forearm", "R ForeTwist": "R Forearm",
          "R ForeTwist1": "R Forearm"}
DELTA_FROM = {"L Toe0": "L Foot", "R Toe0": "R Foot"}


def qmat_engine(q_wxyz):
    """engine raw quaternion (w,x,y,z) -> local rotation matrix R(conj(q))"""
    w, x, y, z = q_wxyz
    return Rot.from_quat([-x, -y, -z, w]).as_matrix()


def engine_quat(Rm):
    """local rotation matrix -> engine raw quaternion (w,x,y,z) = conj(quat(R))"""
    x, y, z, w = Rot.from_matrix(Rm).as_quat()
    return np.array([w, -x, -y, -z])


def rotmin(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        if c > 0:
            return np.eye(3)
        p = np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0])
        return Rot.from_rotvec(np.pi * p / np.linalg.norm(p)).as_matrix()
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1 / (1 + c))


class TodSkel:
    def __init__(self, ident):
        d = json.load(open(os.path.join(CLIPS, "skel_%s.json" % ident)))
        self.ident = ident
        self.model = d["model"]
        pv = d["pivots"]
        self.names = [p["name"] for p in pv]
        self.idx = {n: i for i, n in enumerate(self.names)}
        self.parent = [p["parent"] if p["parent"] != p["i"] else -1 for p in pv]
        self.pos = np.array([p["pos"] for p in pv], float)
        qx = [p["quat_xyzw"] for p in pv]
        self.qbind = np.array([[q[3], q[0], q[1], q[2]] for q in qx])           # engine raw (w,x,y,z)
        self.Lbind = np.array([qmat_engine(q) for q in self.qbind])
        self.order = []
        done = set()
        while len(self.order) < len(pv):
            for i in range(len(pv)):
                if i not in done and (self.parent[i] < 0 or self.parent[i] in done):
                    self.order.append(i); done.add(i)
        self.Gb, self.Pb = self.fk_local(self.Lbind[None], self.pos[None])
        self.Gb, self.Pb = self.Gb[0], self.Pb[0]

    def fk_local(self, L, rootpos):
        """L (F,n,3,3) local rotations; rootpos (F,n,3) local positions (bind or tracks) -> G, P world"""
        F = L.shape[0]
        n = len(self.names)
        G = np.zeros((F, n, 3, 3)); P = np.zeros((F, n, 3))
        for i in self.order:
            p = self.parent[i]
            if p < 0:
                G[:, i] = L[:, i]; P[:, i] = rootpos[:, i]
            else:
                G[:, i] = G[:, p] @ L[:, i]
                P[:, i] = P[:, p] + np.einsum("fab,fb->fa", G[:, p], rootpos[:, i])
        return G, P

    def positions(self, G, bip_pos):
        """world positions from world rotations G (F,n,3,3) and the Bip position (F,3); bone offsets = bind"""
        F = G.shape[0]
        P = np.zeros((F, len(self.names), 3))
        for i in self.order:
            p = self.parent[i]
            if p < 0:
                P[:, i] = bip_pos if self.names[i] == "Bip" else self.pos[i]
            else:
                P[:, i] = P[:, p] + np.einsum("fab,b->fa", G[:, p], self.pos[i])
        return P

    def local(self, G):
        L = np.zeros_like(G)
        for i in range(len(self.names)):
            p = self.parent[i]
            L[:, i] = G[:, i] if p < 0 else np.einsum("fba,fbc->fac", G[:, p], G[:, i])
        return L

    def chain_len(self, a, b):
        """bind distance between two pivots"""
        return float(np.linalg.norm(self.Pb[self.idx[a]] - self.Pb[self.idx[b]]))

    def leg_len(self):
        return 0.5 * (self.chain_len("L Thigh", "L Calf") + self.chain_len("L Calf", "L Foot") +
                      self.chain_len("R Thigh", "R Calf") + self.chain_len("R Calf", "R Foot"))

    def arm_len(self):
        return 0.5 * (self.chain_len("L UpperArm", "L Forearm") + self.chain_len("L Forearm", "L Hand") +
                      self.chain_len("R UpperArm", "R Forearm") + self.chain_len("R Forearm", "R Hand"))


class Source:
    """one SMPL-X dancer: rest joints + FK over a frame range, with helpers in TOD axes"""

    def __init__(self, path, frames=None):
        self.seq = sx.load_seq(path)
        self.fps = self.seq["fps"]
        r = sx.fk(self.seq, frames)
        self.P, self.R, self.J, self.par = r["P"], r["R"], r["J"], r["par"]
        m = sx.load_model(self.seq["gender"])
        b = self.seq["betas"]; n = min(len(b), 300)
        v = m["v"] + np.einsum("vck,k->vc", m["sd"][:, :, :n], b[:n])
        self.sole_y = float(v[:, 1].min())                          # rest-template floor (y up)

    def leg_len(self):
        J = self.J
        L = lambda a, b: np.linalg.norm(J[sx.IDX[a]] - J[sx.IDX[b]])
        return 0.5 * (L("left_hip", "left_knee") + L("left_knee", "left_ankle") + L("right_hip", "right_knee") +
                      L("right_knee", "right_ankle"))

    def arm_len(self):
        J = self.J
        L = lambda a, b: np.linalg.norm(J[sx.IDX[a]] - J[sx.IDX[b]])
        return 0.5 * (L("left_shoulder", "left_elbow") + L("left_elbow", "left_wrist") +
                      L("right_shoulder", "right_elbow") + L("right_elbow", "right_wrist"))

    def hip_rest_height(self):
        J = self.J
        return float(0.5 * (J[1, 1] + J[2, 1]) - self.sole_y)

    def height(self):
        return float(self.J[sx.IDX["head"], 1] + 0.12 - self.sole_y)


def rest_alignment(skel, J=None, jidx=None, bmap=None, J_is_tod=False):
    """per mapped bone: G_rest' = Delta @ G_bind (target in the source T-pose).
    J = the source's rest joints (SMPL rest coords, or TOD rest coords when J_is_tod), jidx = joint name -> row,
    bmap = TOD pivot -> (joint, child joint)."""
    jidx = jidx or sx.IDX
    bmap = bmap or MAP
    if J is None:
        m = sx.load_model("neutral")
        J = m["jr"] @ m["v"]
    Jt = J if J_is_tod else (M3 @ np.asarray(J).T).T
    G = {}
    for name, (j, c) in bmap.items():
        if name not in skel.idx:
            continue
        b = skel.idx[name]
        if c is None:
            G[name] = skel.Gb[b]
            continue
        kids = [k for k in range(len(skel.names)) if skel.parent[k] == b]
        # bind direction: to the child pivot along the chain (first child with a nonzero offset)
        if name.endswith("Hand"):
            tgt = [skel.idx[name.replace("Hand", "Finger1")], skel.idx[name.replace("Hand", "Finger2")]]
            d_bind = skel.Pb[tgt].mean(0) - skel.Pb[b]
        elif name.endswith("Foot"):
            d_bind = skel.Pb[skel.idx[name.replace("Foot", "Toe0")]] - skel.Pb[b]
        else:
            nxt = {"Clavicle": "UpperArm", "UpperArm": "Forearm", "Forearm": "Hand", "Thigh": "Calf", "Calf": "Foot",
                   "Finger0": "Finger01", "Finger1": "Finger11", "Finger2": "Finger21"}
            key = name.split(" ")[1]
            if key in nxt:
                d_bind = skel.Pb[skel.idx[name.split(" ")[0] + " " + nxt[key]]] - skel.Pb[b]
            else:   # last finger segment: continue the parent's direction
                d_bind = skel.Pb[b] - skel.Pb[skel.parent[b]]
        d_src = Jt[jidx[c]] - Jt[jidx[j]]
        G[name] = rotmin(d_bind, d_src) @ skel.Gb[b]
    for name, frm in DELTA_FROM.items():
        b = skel.idx[name]; f = skel.idx[frm]
        G[name] = (G[frm] @ skel.Gb[f].T) @ skel.Gb[b]
    return G


def source_world_tod(src):
    """(F,55,3,3) source joint world rotations in TOD axes, (F,55,3) positions in TOD axes"""
    G = np.einsum("ab,fjbc,cd->fjad", A3, src.R, M3)
    P = np.einsum("ab,fjb->fja", A3, src.P)
    return G, P


def retarget_rotations(skel, Gsrc_tod, align, jidx=None, bmap=None):
    """world rotations of every TOD pivot (F,n,3,3) from the source's TOD-axis world rotations"""
    jidx = jidx or sx.IDX
    bmap = bmap or MAP
    F = Gsrc_tod.shape[0]
    n = len(skel.names)
    G = np.zeros((F, n, 3, 3))
    # start from the bind pose propagated through the hierarchy (unmapped bones keep bind locals)
    done = {}
    for i in skel.order:
        name = skel.names[i]
        p = skel.parent[i]
        if name in bmap and name in align:
            j = jidx[bmap[name][0]]
            G[:, i] = Gsrc_tod[:, j] @ align[name]
        elif name in FOLLOW:
            f = skel.idx[FOLLOW[name]]
            G[:, i] = G[:, f] @ (skel.Gb[f].T @ skel.Gb[i])
        elif p < 0:
            G[:, i] = skel.Lbind[i]
        else:
            G[:, i] = G[:, p] @ skel.Lbind[i]
    # forearm twist bones: ForeTwist1 takes half the hand's twist about the forearm axis
    for S in ("L", "R"):
        fa, ha, t1 = skel.idx[S + " Forearm"], skel.idx[S + " Hand"], skel.idx[S + " ForeTwist1"]
        rel = np.einsum("fba,fbc->fac", G[:, fa], G[:, ha]) @ (skel.Gb[fa].T @ skel.Gb[ha]).T   # hand vs forearm, bind-relative
        axis_local = skel.Gb[fa].T @ (skel.Pb[ha] - skel.Pb[fa]); axis_local /= np.linalg.norm(axis_local)
        rv = Rot.from_matrix(rel).as_rotvec()
        tw = np.einsum("fa,a->f", rv, axis_local)
        half = Rot.from_rotvec(np.outer(0.5 * tw, axis_local)).as_matrix()
        G[:, t1] = G[:, fa] @ half @ (skel.Gb[fa].T @ skel.Gb[t1])
    return G


HINGE_BETA = 12.0          # elbow bend (deg) above which the bend plane alone defines the elbow hinge axis
TWIST_SHARE = {"ForeTwist": 1.0 / 3.0, "ForeTwist1": 2.0 / 3.0}   # share of the hand's roll (vanilla, armtwist.py)


def _twist(D, axis):
    q = Rot.from_matrix(D).as_quat()
    return 2.0 * np.arctan2(q[:, :3] @ axis, q[:, 3])


def canonical_arms(skel, G, bip, sides=("L", "R"), report=None, cyclic=False):
    """Re-express the arms in the game's own convention (3ds Max Biped, measured on 390 vanilla Ram clips by
    armtwist.py survey):
      * Forearm = a pure hinge about the UpperArm's local +y (elbow twist 0, bend axis never flips);
      * the forearm roll lives in the Hand (relative to the Forearm) and is spread over the twist links:
        ForeTwist (at the elbow, child of UpperArm) 1/3, ForeTwist1 (mid-forearm) 2/3;
      * the UpperArm's roll is whatever puts the hinge axis on the bend-plane normal.
    Joint positions (shoulder, elbow, wrist) and the hand's (and fingers') world rotation are kept exactly, so
    contacts do not move. Where the arm is almost straight (bend < HINGE_BETA) the bend plane is undefined: the hinge
    axis is interpolated in time between the neighbouring bent frames (in the incoming upper arm's frame), so the
    elbow never flips. G (F,n,3,3) world rotations; bip (F,3). Returns a new G."""
    G = G.copy()
    P = skel.positions(G, bip)
    F = len(G)
    for S in sides:
        ua, fa, ha = skel.idx[S + " UpperArm"], skel.idx[S + " Forearm"], skel.idx[S + " Hand"]
        u = P[:, fa] - P[:, ua]; v = P[:, ha] - P[:, fa]
        uh = u / np.linalg.norm(u, axis=1, keepdims=True); vh = v / np.linalg.norm(v, axis=1, keepdims=True)
        n = np.cross(uh, vh); sn = np.linalg.norm(n, axis=1)
        beta = np.degrees(np.arctan2(sn, np.sum(uh * vh, 1)))
        Gu0 = G[:, ua].copy()
        # hinge axis in the incoming upper arm's frame: the bend normal where bent, else interpolated in time
        hl = np.einsum("fba,fb->fa", Gu0, n / np.maximum(sn, 1e-12)[:, None])
        anchor = beta >= HINGE_BETA
        if anchor.any():
            ia = np.nonzero(anchor)[0]
            idx, val = ia, hl[ia].copy()
            if cyclic:                                   # frame F-1 == frame 0: anchors repeat with period F-1
                idx = np.concatenate([ia - (F - 1), ia, ia + (F - 1)])
                val = np.concatenate([val, val, val])
            for f in np.nonzero(~anchor)[0]:
                k = np.searchsorted(idx, f)
                if k == 0:
                    hl[f] = val[0]
                elif k == len(idx):
                    hl[f] = val[-1]
                else:
                    t = (f - idx[k - 1]) / (idx[k] - idx[k - 1])
                    x = (1 - t) * val[k - 1] + t * val[k]
                    if np.linalg.norm(x) < 1e-6:          # opposite anchors: keep the earlier one
                        x = val[k - 1]
                    hl[f] = x / np.linalg.norm(x)
        else:
            hl[:] = [0, 1, 0]
        h = np.einsum("fab,fb->fa", Gu0, hl)
        h = h - uh * np.sum(h * uh, 1, keepdims=True)
        h /= np.linalg.norm(h, axis=1, keepdims=True)
        # UpperArm: bone direction (local -x, the Forearm pivot sits at -x) on u, local +y on the hinge axis
        bdir = skel.pos[fa] / np.linalg.norm(skel.pos[fa])                  # (-1,0,0) for the Biped
        Gu = np.zeros((F, 3, 3))
        for f in range(F):
            Gu[f] = _frame_from(bdir, np.array([0, 1.0, 0]), uh[f], h[f])
        # Forearm: hinge about local y to the projection of v, then the (small) remainder as a swing
        vl = np.einsum("fba,fb->fa", Gu, vh)                              # v in the new upper-arm frame
        dfa = skel.pos[ha] / np.linalg.norm(skel.pos[ha])                  # Forearm bone direction (local)
        Gf = np.zeros((F, 3, 3))
        a0 = np.arctan2(dfa[0], dfa[2])
        off = np.zeros(F)
        for f in range(F):
            # Ry(phi) (x,0,z) = (r sin(alpha+phi), 0, r cos(alpha+phi)) with alpha = atan2(x, z)
            phi = np.arctan2(vl[f, 0], vl[f, 2]) - a0
            c, s = np.cos(phi), np.sin(phi)
            Ry = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
            sw = rotmin(Ry @ dfa, vl[f])
            off[f] = np.degrees(np.arccos(np.clip(np.dot(Ry @ dfa, vl[f]), -1, 1)))
            Gf[f] = Gu[f] @ sw @ Ry
        G[:, ua] = Gu
        G[:, fa] = Gf
        # roll: the hand's twist about the forearm axis (continuous in time), shared by the twist links
        ax = skel.Gb[fa].T @ (skel.Pb[ha] - skel.Pb[fa]); ax /= np.linalg.norm(ax)
        rel = np.einsum("fba,fbc->fac", Gf, G[:, ha]) @ (skel.Gb[fa].T @ skel.Gb[ha]).T
        tw = _twist(rel, ax)
        tw = (tw + np.pi) % (2 * np.pi) - np.pi
        tau = np.unwrap(tw)
        if np.abs(tau - tw).max() > 1e-9 and abs(tau[-1] - tau[0] - (tw[-1] - tw[0])) > 1e-6:
            # the roll crosses +-180 degrees: keep the unwrapped branch (no jump); loop ends may then differ
            pass
        for nm, k in TWIST_SHARE.items():
            b = skel.idx[S + " " + nm]
            Rt = Rot.from_rotvec(np.outer(k * tau, ax)).as_matrix()
            G[:, b] = Gf @ Rt @ (skel.Gb[fa].T @ skel.Gb[b])
        if report is not None:
            report[S] = {"hand_roll_deg_max": round(float(np.degrees(np.abs(tau)).max()), 1),
                         "straight_frames": int((~anchor).sum()),
                         "roll_crosses_180": bool(np.abs(tau - tw).max() > 1e-9),
                         "forearm_off_hinge_deg_max": round(float(off.max()), 2)}
    return G


def _frame_from(a_loc, b_loc, a_w, b_w):
    """rotation R with R a_loc = a_w and R b_loc = b_w (a ⟂ b in both frames, unit vectors)"""
    A = np.stack([a_loc, b_loc, np.cross(a_loc, b_loc)], 1)
    B = np.stack([a_w, b_w, np.cross(a_w, b_w)], 1)
    return B @ A.T


def hinge_pole(skel, Gf, S, Pa, target):
    """IK pole point for the elbow (one frame): the side the elbow bends to under the UpperArm's hinge axis (local
    +y), so a straight arm keeps its elbow direction instead of picking a random side. Gf = (n,3,3) world rotations."""
    h = Gf[skel.idx[S + " UpperArm"]] @ np.array([0, 1.0, 0])
    dn = target - Pa
    dn = dn / max(np.linalg.norm(dn), 1e-9)
    return Pa + np.cross(dn, h)


def two_bone_ik(Pa, Pb, Pc, target, pole_hint):
    """positions of a 2-bone chain a-b-c; returns rotations (3x3) to apply to the a-bone and the b-bone (world, pre-
    multiplied) so that c reaches target (clamped), keeping the knee/elbow on the side of pole_hint."""
    l1 = np.linalg.norm(Pb - Pa); l2 = np.linalg.norm(Pc - Pb)
    d = target - Pa
    dist = np.clip(np.linalg.norm(d), abs(l1 - l2) + 1e-4, l1 + l2 - 1e-4)
    dn = d / max(np.linalg.norm(d), 1e-9)
    # elbow position in the plane of (dn, pole)
    pole = pole_hint - Pa
    pole = pole - dn * np.dot(pole, dn)
    if np.linalg.norm(pole) < 1e-6:
        pole = np.cross(dn, [0, 1, 0])
    pole /= np.linalg.norm(pole)
    cosA = (l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist)
    sinA = np.sqrt(max(0.0, 1 - cosA * cosA))
    Pb_new = Pa + l1 * (cosA * dn + sinA * pole)
    Pc_new = Pa + dist * dn
    Ra = rotmin(Pb - Pa, Pb_new - Pa)
    # after Ra, the b->c segment direction is Ra (Pc - Pb); rotate it onto the new one
    Rb = rotmin(Ra @ (Pc - Pb), Pc_new - Pb_new)
    return Ra, Rb, Pc_new


# ------------------------------------------------------------------------------------------------ Mixamo sources
MIXAMO_MAP = {
    "Bip": ("Hips", None), "Pelvis": ("Hips", None), "Spine": ("Spine", None), "Spine1": ("Spine1", None),
    "Spine2": ("Spine2", None), "Spine3": ("Spine2", None), "Neck": ("Neck", None), "Head": ("Head", None),
}
for _side, _S in (("Left", "L"), ("Right", "R")):
    MIXAMO_MAP.update({
        _S + " Clavicle": (_side + "Shoulder", _side + "Arm"),
        _S + " UpperArm": (_side + "Arm", _side + "ForeArm"),
        _S + " Forearm": (_side + "ForeArm", _side + "Hand"),
        _S + " Hand": (_side + "Hand", _side + "HandMiddle1"),
        _S + " Finger0": (_side + "HandThumb1", _side + "HandThumb2"),
        _S + " Finger01": (_side + "HandThumb2", _side + "HandThumb3"),
        _S + " Finger1": (_side + "HandIndex1", _side + "HandIndex2"),
        _S + " Finger11": (_side + "HandIndex2", _side + "HandIndex3"),
        _S + " Finger2": (_side + "HandRing1", _side + "HandRing2"),
        _S + " Finger21": (_side + "HandRing2", _side + "HandRing3"),
        _S + " Thigh": (_side + "UpLeg", _side + "Leg"),
        _S + " Calf": (_side + "Leg", _side + "Foot"),
        _S + " Foot": (_side + "Foot", _side + "ToeBase"),
        _S + " Toe0": (_side + "ToeBase", None),
    })


class MixamoSource:
    """a Blender-dumped Mixamo animation (bl_fbxdump.py): world matrices per frame, z-up.
    G (F,n,3,3) = world rotation deltas from the rest pose in TOD axes; P (F,n,3) positions in TOD axes;
    Jt (n,3) rest positions in TOD axes; idx: short bone name -> column. The rest frame (0 in the Rokoko takes) is a
    T-pose; its own left / up / forward define the axes, so the mirror into TOD axes keeps Left* -> L *."""

    def __init__(self, npz, rest_frame=0):
        d = np.load(npz)
        self.names = [str(n).split(":")[-1] for n in d["names"]]
        self.idx = {n: i for i, n in enumerate(self.names)}
        self.fps = float(d["fps"])
        W = d["world"]
        rest = W[rest_frame]
        Jb = rest[:, :3, 3]
        left = Jb[self.idx["LeftArm"]] - Jb[self.idx["RightArm"]]; left /= np.linalg.norm(left)
        up = Jb[self.idx["Head"]] - Jb[self.idx["Hips"]]; up -= left * np.dot(up, left); up /= np.linalg.norm(up)
        fwd = np.cross(left, up)
        C = np.stack([left, up, fwd])                      # Blender world -> right-handed rest coords (left, up, fwd)
        self.A = M3 @ C                                    # -> TOD axes (left = -x)
        Rrest = rest[:, :3, :3]
        D = np.einsum("fjab,jcb->fjac", W[:, :, :3, :3], Rrest)    # world delta from the rest pose
        self.G = np.einsum("ab,fjbc,dc->fjad", self.A, D, self.A)
        self.P = np.einsum("ab,fjb->fja", self.A, W[:, :, :3, 3])
        self.Jt = (self.A @ Jb.T).T
