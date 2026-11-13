"""make_clip.py - [mayhem_heat] clips from a test-harness burst (frames shots/clip_N.png; the
harness is not included).

Writes into OUT (default <repo>/work/clips, git-ignored):
  NAME_full.mp4    the whole frame at the capture rate (real time), with a caption strip: run time, heat level and heat
                   from TODGameplay.log (the 'heat: state' lines are every 2 s with log_verbose)
  NAME_radar.mp4   the radar corner enlarged 2x (tint, rim pulse, coins), same captions
  NAME_sheet.jpg   a contact sheet of the radar corner every few seconds
usage: python -I make_clip.py RUN_DIR NAME [--out DIR] [--prefix clip]
"""
import json, os, re, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def log_states(run):
    """(time of day seconds, heat, level, waves, reinforcements arrived so far) from the DLL log"""
    out, arrived = [], 0
    for line in open(os.path.join(run, "TODGameplay.log"), encoding="utf-8", errors="replace"):
        m = re.match(r"(\d+):(\d+):(\d+)\.(\d+) (.*)", line)
        if not m:
            continue
        t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000.0
        txt = m[5]
        if "reinforcement arrived" in txt:
            arrived += 1
        s = re.match(r"heat: state heat ([\d.]+) level (\d) .*waves (\d+)", txt)
        if s:
            out.append((t, float(s[1]), int(s[2]), int(s[3]), arrived))
    return out


def main():
    run, name = sys.argv[1], sys.argv[2]
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(os.path.dirname(__file__), "..", "..", "work", "clips")
    prefix = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "clip"
    os.makedirs(out, exist_ok=True)
    res = json.load(open(os.path.join(run, "result.json")))
    shots = sorted([(v["t"], k, v["file"]) for k, v in res["shots"].items() if k.startswith(prefix + "_")])
    if len(shots) < 2:
        sys.exit("no burst frames")
    t0 = shots[0][0]
    dur = shots[-1][0] - t0
    fps = max(1.0, (len(shots) - 1) / dur) if dur > 0 else 10.0
    # log time of the first frame: the run start (result 'start') + t; the log uses local time of day
    st = res["start"].split()[1].split(":")
    start_tod = int(st[0]) * 3600 + int(st[1]) * 60 + int(st[2])
    states = log_states(run)
    quad = (36.0, 772.0, 324.0, 1060.0)   # the radar quad at 1920x1080 (the stage logs it: "radar stage: radar quad")
    for line in open(os.path.join(run, "TODGameplay.log"), encoding="utf-8", errors="replace"):
        if "radar stage: radar quad" in line:
            pts = [(float(a), float(b)) for a, b in re.findall(r"\(([\d.]+),([\d.]+)\)", line.split(" uv")[0])]
            quad = (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
            break
    tmp = os.path.join(out, "_frames_" + name)
    os.makedirs(tmp, exist_ok=True)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 26)
    except OSError:
        font = ImageFont.load_default()
    sheet = []
    for i, (t, k, f) in enumerate(shots):
        im = Image.open(f).convert("RGB")
        W, H = im.size
        tod = start_tod + t
        st_ = [s for s in states if s[0] <= tod]
        cap = "t %5.1f s" % (t - t0)
        if st_:
            s = st_[-1]
            cap += "   heat %5.1f   level %d   waves %d   reinforcements arrived %d" % (s[1], s[2], s[3], s[4])
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, W, 40], fill=(0, 0, 0))
        d.text((12, 6), cap, fill=(255, 220, 160), font=font)
        im.save(os.path.join(tmp, "f_%05d.png" % i))
        # radar corner (bottom left, the radar sits at about x 0.13..0.33 W, y 0.68..0.97 H at 1920x1080)
        box = (max(0, int(quad[0] - 20)), max(0, int(quad[1] - 130)), min(W, int(quad[2] + 170)), H)
        rc = im.crop(box)
        rc = rc.resize((rc.width * 2, rc.height * 2), Image.LANCZOS)
        cap_im = Image.new("RGB", (rc.width, rc.height + 44), (0, 0, 0))
        cap_im.paste(rc, (0, 44)); ImageDraw.Draw(cap_im).text((10, 8), cap, fill=(255, 220, 160), font=font)
        cap_im.save(os.path.join(tmp, "r_%05d.png" % i))
        if i % max(1, int(fps * 6)) == 0:
            sheet.append(cap_im.resize((cap_im.width // 3, cap_im.height // 3), Image.LANCZOS))
    for kind in ("f", "r"):
        dst = os.path.join(out, "%s_%s.mp4" % (name, "full" if kind == "f" else "radar"))
        subprocess.run([FFMPEG, "-y", "-loglevel", "error", "-framerate", "%.3f" % fps, "-i", os.path.join(tmp, kind + "_%05d.png"),
                        "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", dst], check=True)
        print("wrote", dst)
    cols = 6
    rows = (len(sheet) + cols - 1) // cols
    sw, sh = sheet[0].size
    S = Image.new("RGB", (cols * sw, rows * sh), (0, 0, 0))
    for i, im in enumerate(sheet):
        S.paste(im, ((i % cols) * sw, (i // cols) * sh))
    S.save(os.path.join(out, name + "_sheet.jpg"), quality=88)
    print("wrote sheet, %d frames at %.2f fps (%.1f s)" % (len(shots), fps, dur))
    for f in os.listdir(tmp):
        os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)


if __name__ == "__main__":
    main()
