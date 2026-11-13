"""todsticks.py - stick-figure sheet of a built pair (TOD skeletons) for a quick look.

  python todsticks.py ../work/pair_soldier_25.npz PARTNER OUT.png [step_s] [part=dance|fo]
View: front three-quarter of the pair frame, mirrored to true handedness (TOD data is left-handed).
"""
import sys
import numpy as np
from PIL import Image, ImageDraw
import retarget as rt


def main():
    npz, partner, out = sys.argv[1], sys.argv[2], sys.argv[3]
    step = float(sys.argv[4]) if len(sys.argv) > 4 else 0.4
    part = sys.argv[5] if len(sys.argv) > 5 else "dance"
    d = np.load(npz)
    sks = {"lead": rt.TodSkel("ram"), "follow": rt.TodSkel(partner)}
    key = "P" if part == "dance" else "foP"
    PL, PF = d["lead_" + key], d["follow_" + key]
    dt = 1 / 25.0 if "_25" in npz else 1 / 30.0
    idxs = np.arange(0, len(PL), max(1, int(round(step / dt))))
    cols, W = 8, 220
    rows = (len(idxs) + cols - 1) // cols
    img = Image.new("RGB", (cols * W, rows * W), (245, 245, 240))
    dr = ImageDraw.Draw(img)
    yaw = np.radians(-35)
    for k, f in enumerate(idxs):
        ox, oy = (k % cols) * W, (k // cols) * W
        cen = 0.5 * (PL[f, 1] + PF[f, 1])
        for P, sk, col, lcol in ((PL[f], sks["lead"], (30, 80, 200), (120, 170, 255)), (PF[f], sks["follow"], (200, 40, 40), (255, 140, 140))):
            Q = P - cen
            Q = Q * np.array([-1, 1, 1])          # mirror to true handedness
            c, s = np.cos(yaw), np.sin(yaw)
            x = c * Q[:, 0] + s * Q[:, 2]
            z = -s * Q[:, 0] + c * Q[:, 2]
            y = Q[:, 1] * np.cos(0.3) + z * np.sin(0.3)
            sc = W / 2.8
            pts = [(ox + W / 2 + x[i] * sc, oy + W * 0.45 - y[i] * sc) for i in range(len(P))]
            for i, p in enumerate(sk.parent):
                if p < 0 or sk.names[i] in ("Bip", "GamePivot") or sk.names[p] in ("",):
                    continue
                dr.line([pts[p], pts[i]], fill=lcol if sk.names[i].startswith("L ") else col, width=3)
            hx, hy = pts[sk.idx["Head"]]
            dr.ellipse([hx - 6, hy - 6, hx + 6, hy + 6], outline=col, width=2)
        gy = oy + W * 0.45 - ((-1.09 - cen[1]) * np.cos(0.3)) * (W / 2.8)
        dr.line([(ox + 10, gy), (ox + W - 10, gy)], fill=(160, 160, 160))
        dr.text((ox + 4, oy + 4), "%.2f" % (f * dt), fill=(0, 0, 0))
    img.save(out)
    print(out)


if __name__ == "__main__":
    main()
