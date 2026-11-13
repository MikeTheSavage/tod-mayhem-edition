"""sticksheet.py - stick-figure contact sheet of a CoMPAS3D pair window (PIL only), for choosing the section.

  python sticksheet.py PAIR SONG TAKE T0 T1 [STEP] OUT.png
Leader blue, follower red (left limbs lighter). Each cell is a 3/4 view from above-front of the pair's mean heading.
"""
import sys
import numpy as np
from PIL import Image, ImageDraw
import smplx_fk as s

BONES = [(0, 3), (3, 6), (6, 9), (9, 12), (12, 15), (9, 13), (13, 16), (16, 18), (18, 20), (9, 14), (14, 17), (17, 19),
         (19, 21), (0, 1), (1, 4), (4, 7), (7, 10), (0, 2), (2, 5), (5, 8), (8, 11)]
LEFT = {1, 4, 7, 10, 13, 16, 18, 20}


def proj(P, yaw, elev=np.radians(20)):
    c, s_ = np.cos(yaw), np.sin(yaw)
    x = c * P[..., 0] + s_ * P[..., 1]
    y = -s_ * P[..., 0] + c * P[..., 1]
    z = P[..., 2]
    return np.stack([x, z * np.cos(elev) - y * np.sin(elev)], -1)


def main():
    pair, song, take = map(int, sys.argv[1:4])
    t0, t1 = float(sys.argv[4]), float(sys.argv[5])
    step = float(sys.argv[6]) if len(sys.argv) > 7 else 0.4
    out = sys.argv[-1]
    L, F, T = s.seq_paths(pair, song, take)
    ql, qf = s.load_seq(L), s.load_seq(F)
    fps = ql["fps"]
    i0, i1 = int(t0 * fps), int(t1 * fps)
    a, b = s.fk(ql, slice(i0, i1)), s.fk(qf, slice(i0, i1))
    times = np.arange(t0, t1, step)
    cols = 8
    rows = (len(times) + cols - 1) // cols
    W = 220
    img = Image.new("RGB", (cols * W, rows * W), (245, 245, 240))
    dr = ImageDraw.Draw(img)
    # view: look along the perpendicular of the leader->follower line so both are visible side by side, from 35 deg
    for k, t in enumerate(times):
        i = int((t - t0) * fps)
        PL, PF = a["P"][i, :22], b["P"][i, :22]
        v = PF[0, :2] - PL[0, :2]
        yaw = np.arctan2(v[1], v[0]) + np.radians(35)
        cen = 0.5 * (PL[0] + PF[0])
        ox, oy = (k % cols) * W, (k // cols) * W
        for P, col, lcol in ((PL, (30, 80, 200), (120, 170, 255)), (PF, (200, 40, 40), (255, 140, 140))):
            q = proj(P - cen + np.array([0, 0, 0.0]), yaw)
            sc = W / 2.6
            pts = [(ox + W / 2 + x * sc, oy + W * 0.55 - y * sc) for x, y in q]
            for (u, w) in BONES:
                dr.line([pts[u], pts[w]], fill=lcol if w in LEFT else col, width=3)
            hx, hy = pts[15]
            dr.ellipse([hx - 6, hy - 6, hx + 6, hy + 6], outline=col, width=2)
        dr.text((ox + 4, oy + 4), "%.1f" % t, fill=(0, 0, 0))
    img.save(out)
    print(out, img.size)


if __name__ == "__main__":
    main()
