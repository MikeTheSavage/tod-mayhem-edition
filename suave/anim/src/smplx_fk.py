"""smplx_fk.py - pure-numpy SMPL-X forward kinematics for CoMPAS3D sequences.

Rest joints = J_regressor x (v_template + shapedirs[:, :, :n] . betas[:n]); global rotations through the kintree;
root = rest pelvis + trans. Pose blend shapes do not move joints, so they are not needed.
Returns joint world positions (F, 55, 3) and world rotation matrices (F, 55, 3, 3) in the dataset's frame.
"""
import os
import numpy as np

MODELS = os.environ.get("SMPLX_MODELS", "smplx_models")  # SMPL-X model files (download separately)
DATA = os.environ.get("COMPAS3D_DATA", "compas3d")  # CoMPAS3D dataset (download separately)

NAMES = ["pelvis", "left_hip", "right_hip", "spine1", "left_knee", "right_knee", "spine2", "left_ankle",
         "right_ankle", "spine3", "left_foot", "right_foot", "neck", "left_collar", "right_collar", "head",
         "left_shoulder", "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist", "jaw",
         "left_eye", "right_eye"]
_F = ["index", "middle", "pinky", "ring", "thumb"]
NAMES += ["left_%s%d" % (f, i) for f in _F for i in (1, 2, 3)]
NAMES += ["right_%s%d" % (f, i) for f in _F for i in (1, 2, 3)]
IDX = {n: i for i, n in enumerate(NAMES)}

_model_cache = {}


def load_model(gender):
    g = {"male": "MALE", "female": "FEMALE"}.get(str(gender).lower(), "NEUTRAL")
    if g not in _model_cache:
        z = np.load(os.path.join(MODELS, "SMPLX_%s.npz" % g), allow_pickle=True)
        _model_cache[g] = {"v": z["v_template"].astype(np.float64), "sd": z["shapedirs"].astype(np.float64),
                           "jr": z["J_regressor"].astype(np.float64), "par": z["kintree_table"][0].astype(np.int64)}
        _model_cache[g]["par"][0] = -1
    return _model_cache[g]


def load_seq(path):
    """Load a CoMPAS3D npz without pickle (the object-array keys are skipped)."""
    z = np.load(path, allow_pickle=False)
    out = {}
    for k in ("gender", "surface_model_type", "mocap_frame_rate", "betas", "poses", "trans"):
        out[k] = z[k]
    out["gender"] = str(out["gender"])
    out["fps"] = float(out["mocap_frame_rate"])
    return out


def rodrigues(aa):
    """(..., 3) axis-angle -> (..., 3, 3)"""
    th = np.linalg.norm(aa, axis=-1, keepdims=True)
    k = aa / np.maximum(th, 1e-12)
    K = np.zeros(aa.shape[:-1] + (3, 3))
    K[..., 0, 1], K[..., 0, 2] = -k[..., 2], k[..., 1]
    K[..., 1, 0], K[..., 1, 2] = k[..., 2], -k[..., 0]
    K[..., 2, 0], K[..., 2, 1] = -k[..., 1], k[..., 0]
    s, c = np.sin(th)[..., None], np.cos(th)[..., None]
    I = np.eye(3)
    return I + s * K + (1 - c) * (K @ K)


def rest_joints(seq):
    m = load_model(seq["gender"])
    b = seq["betas"]
    n = min(len(b), 300, m["sd"].shape[2])
    v = m["v"] + np.einsum("vck,k->vc", m["sd"][:, :, :n], b[:n])
    return m["jr"] @ v, m["par"]


def fk(seq, frames=None):
    J, par = rest_joints(seq)
    poses = seq["poses"] if frames is None else seq["poses"][frames]
    trans = seq["trans"] if frames is None else seq["trans"][frames]
    F = len(poses)
    R_loc = rodrigues(poses.reshape(F, 55, 3))
    Rw = np.zeros((F, 55, 3, 3))
    P = np.zeros((F, 55, 3))
    for j in range(55):
        p = par[j]
        if p < 0:
            Rw[:, j] = R_loc[:, j]
            P[:, j] = J[j] + trans
        else:
            Rw[:, j] = Rw[:, p] @ R_loc[:, j]
            P[:, j] = P[:, p] + np.einsum("fab,b->fa", Rw[:, p], J[j] - J[p])
    return {"P": P, "R": Rw, "J": J, "par": par}


def seq_paths(pair, song, take):
    d = os.path.join(DATA, "Pair%d" % pair, "Pair%d_song%d_take%d" % (pair, song, take))
    b = os.path.join(d, "Pair%d_song%d_take%d" % (pair, song, take))
    return b + "_leader.npz", b + "_follower.npz", b + ".txt"


def annotations(txt):
    rows = []
    if not os.path.exists(txt):
        return rows
    for line in open(txt, encoding="utf-8", errors="replace"):
        c = line.rstrip("\n").split("\t")
        if len(c) >= 9:
            rows.append({"kind": c[0], "who": c[1], "start": float(c[3]), "end": float(c[5]), "move": c[8].strip()})
    return rows
