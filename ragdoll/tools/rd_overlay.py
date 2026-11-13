"""rd_overlay.py - the [ragdoll] debug draw: projects the capsules the DLL logged ([ragdoll] debug_draw = 1, lines
"ragdoll: draw cam ..." / "ragdoll: draw cap ..." / "ragdoll: draw end") onto a screenshot of the same moment.

  python -I rd_overlay.py RUN_DIR SHOT_NAME [OUT.png] [--block N]

The camera line holds the rendered camera matrix (0x87BC60: rows = right / up / forward axes, then the position) and
the camera's fov (+B4h, the 4:3 horizontal fov the projection 0x87DE40 uses: vfov = 2 atan(0.75 tan(fov / 2)), the
width follows the screen: Hor+). The draw block nearest in time before the shot is used (log line order; the harness
records each shot's time in result.json, the log lines carry no time, so --block picks one by index when needed).
Each capsule is drawn as its two end circles and the side lines, coloured by mode (active green, limp orange,
frozen blue)."""
import json, math, os, sys
from PIL import Image, ImageDraw

COL = {"active": (60, 255, 90), "limp": (255, 150, 30), "frozen": (80, 160, 255)}


def blocks(log):
    out, cur = [], None
    for line in open(log, encoding="utf-8", errors="replace"):
        if "ragdoll: draw cam " in line:
            ts = line[:12]
            t = line.split("ragdoll: draw cam ", 1)[1].split()
            v = [float(x) for x in t[:12]]
            fov = float(t[t.index("fov") + 1])
            cur = {"ts": ts, "X": v[0:3], "Y": v[3:6], "Z": v[6:9], "P": v[9:12], "fov": fov, "caps": []}
        elif "ragdoll: draw cap " in line and cur is not None:
            t = line.split("ragdoll: draw cap ", 1)[1].split()
            cur["caps"].append((t[0], int(t[1]), [float(x) for x in t[2:5]], [float(x) for x in t[5:8]], float(t[8])))
        elif "ragdoll: draw end" in line and cur is not None:
            out.append(cur); cur = None
    return out


def project(b, p, W, H):
    d = [p[i] - b["P"][i] for i in range(3)]
    x = sum(d[i] * b["X"][i] for i in range(3)); y = sum(d[i] * b["Y"][i] for i in range(3)); z = sum(d[i] * b["Z"][i] for i in range(3))
    if z < 0.05:
        return None
    vf = 2.0 * math.atan(0.75 * math.tan(math.radians(b["fov"]) / 2.0))
    f = (H / 2.0) / math.tan(vf / 2.0)
    return (W / 2.0 + x / z * f, H / 2.0 - y / z * f, f / z)


def draw(img, b):
    W, H = img.size
    d = ImageDraw.Draw(img)
    n = 0
    for mode, body, a, c, r in b["caps"]:
        pa, pc = project(b, a, W, H), project(b, c, W, H)
        if not pa or not pc:
            continue
        col = COL.get(mode, (255, 255, 255))
        ra, rc = r * pa[2], r * pc[2]
        d.ellipse([pa[0] - ra, pa[1] - ra, pa[0] + ra, pa[1] + ra], outline=col, width=2)
        d.ellipse([pc[0] - rc, pc[1] - rc, pc[0] + rc, pc[1] + rc], outline=col, width=2)
        dx, dy = pc[0] - pa[0], pc[1] - pa[1]; l = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / l, dx / l
        d.line([pa[0] + nx * ra, pa[1] + ny * ra, pc[0] + nx * rc, pc[1] + ny * rc], fill=col, width=2)
        d.line([pa[0] - nx * ra, pa[1] - ny * ra, pc[0] - nx * rc, pc[1] - ny * rc], fill=col, width=2)
        d.line([pa[0], pa[1], pc[0], pc[1]], fill=col, width=1)
        n += 1
    return n


def main():
    run, shot = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else os.path.join(run, shot + "_debugdraw.png")
    bl = blocks(os.path.join(run, "TODGameplay.log"))
    if not bl:
        sys.exit("no draw blocks in the log")
    idx = len(bl) - 1
    mark_ts = None   # the block logged right after the shot's mark line (the harness grabs within ~50 ms of it)
    for line in open(os.path.join(run, "TODGameplay.log"), encoding="utf-8", errors="replace"):
        if "debug keys: mark shot %s " % shot in line or line.rstrip().endswith("mark shot %s" % shot):
            mark_ts = line[:12]
    if mark_ts:
        after = [i for i, b in enumerate(bl) if b["ts"] >= mark_ts]
        if after: idx = after[0]
    # a burst frame NAME_NN: the burst's mark time + (this frame's time - frame 00's time) from result.json
    if not mark_ts and "_" in shot and shot.rsplit("_", 1)[1].isdigit():
        base = shot.rsplit("_", 1)[0]
        res = json.load(open(os.path.join(run, "result.json")))["shots"]
        for line in open(os.path.join(run, "TODGameplay.log"), encoding="utf-8", errors="replace"):
            if "debug keys: mark burst %s " % base in line: mark_ts = line[:12]
        if mark_ts and shot in res and base + "_00" in res:
            def sec(ts): h, m, x = ts.split(":"); return int(h) * 3600 + int(m) * 60 + float(x)
            want = sec(mark_ts) + res[shot]["t"] - res[base + "_00"]["t"]
            idx = min(range(len(bl)), key=lambda i: abs(sec(bl[i]["ts"]) - want))
    if "--block" in sys.argv:
        idx = int(sys.argv[sys.argv.index("--block") + 1])
    img = Image.open(os.path.join(run, "shots", shot + ".png")).convert("RGB")
    n = draw(img, bl[idx])
    img.save(out)
    print("block %d of %d (%s, shot mark %s): %d capsules drawn -> %s" % (idx, len(bl), bl[idx]["ts"], mark_ts, n, out))


if __name__ == "__main__":
    main()
