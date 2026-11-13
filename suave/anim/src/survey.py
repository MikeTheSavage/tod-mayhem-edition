"""survey.py - per-second timeline of a CoMPAS3D pair: distance, turns, hand contacts, step beat estimate.

  python survey.py PAIR SONG TAKE [t0 t1]
Columns: t, pelvis distance, leader / follower yaw change over the second (deg, + = left turn seen from above),
contacts (leader hand -> follower part within 10 cm), annotation.
"""
import sys
import numpy as np
import smplx_fk as s

PARTS = ["left_wrist", "right_wrist", "spine3", "spine2", "left_shoulder", "right_shoulder", "head", "pelvis"]


def yaw(R):
    # forward = body +z axis in world (z-up dataset): heading angle in the x-y plane
    f = R[:, :, 2]
    return np.unwrap(np.arctan2(f[:, 1], f[:, 0]))


def handpt(P, side):
    w = s.IDX[side + "_wrist"]; m = s.IDX[side + "_middle1"]
    return 0.5 * (P[:, w] + P[:, m])


def main():
    pair, song, take = map(int, sys.argv[1:4])
    L, F, T = s.seq_paths(pair, song, take)
    ql, qf = s.load_seq(L), s.load_seq(F)
    n = min(len(ql["poses"]), len(qf["poses"]))
    a, b = s.fk(ql, slice(0, n)), s.fk(qf, slice(0, n))
    fps = ql["fps"]
    PL, PF = a["P"], b["P"]
    yl, yf = yaw(a["R"][:, 0]), yaw(b["R"][:, 0])
    d = np.linalg.norm(PL[:, 0, :2] - PF[:, 0, :2], axis=1)
    ann = s.annotations(T)
    t0 = float(sys.argv[4]) if len(sys.argv) > 4 else 0
    t1 = float(sys.argv[5]) if len(sys.argv) > 5 else n / fps
    for sec in np.arange(t0, t1, 0.5):
        i = int(sec * fps); j = min(n - 1, int((sec + 0.5) * fps))
        con = []
        for side in ("left", "right"):
            h = handpt(PL, side)[i:j]
            for p in PARTS:
                q = PF[i:j, s.IDX[p]] if "wrist" not in p else handpt(PF, p.split("_")[0])[i:j]
                dd = np.linalg.norm(h - q, axis=1).min()
                if dd < 0.10:
                    con.append("L%s-F%s" % (side[0].upper(), p.replace("left_wrist", "Lh").replace("right_wrist", "Rh")))
        an = [x["move"][:60] for x in ann if x["kind"] == "Together" and x["start"] <= sec < x["end"]]
        print("%6.1f d=%.2f yl=%+5.0f yf=%+5.0f %-40s %s" % (sec, d[i], np.degrees(yl[j] - yl[i]),
              np.degrees(yf[j] - yf[i]), " ".join(con), an[0] if an else ""))


if __name__ == "__main__":
    main()
