"""make_car_clip.py - MP4 clip (and a contact sheet) of one test-harness run's screenshot bursts
(the harness is not included), for [car_physics] clips.

  python -I make_car_clip.py RUN OUT.mp4 --prefix c3_drive [--prefix c3_kick ...] [--title TEXT] [--sheet OUT.jpg]

Frames: the run's shots whose names start with a prefix, in capture order; each is shown for the time until the next
capture (clamped 0.04 .. 1.0 s); 1280x720 with a title bar; H.264 yuv420p through the DAZ ffmpeg. Read only on the run.
"""
import argparse, json, os, shutil, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run"); ap.add_argument("out")
    ap.add_argument("--prefix", action="append", required=True)
    ap.add_argument("--title", default="TOD Gameplay+ [car_physics]")
    ap.add_argument("--sheet", default=None)
    ap.add_argument("--sheet-every", type=int, default=6)
    ap.add_argument("--crop", default=None, help="x0,y0,x1,y1 of the 1920x1080 shots (a close-up)")
    ap.add_argument("--first", type=int, default=0); ap.add_argument("--last", type=int, default=100000)
    x = ap.parse_args()
    r = json.load(open(os.path.join(x.run, "result.json"), encoding="utf-8"))
    seq = sorted([(v["t"], k, v["file"]) for k, v in r["shots"].items() if any(k.startswith(p) for p in x.prefix)])
    seq = [s for s in seq if os.path.exists(s[2])][x.first:x.last]
    if not seq:
        sys.exit("no shots")
    fr = os.path.splitext(x.out)[0] + "_frames"
    if os.path.exists(fr):
        shutil.rmtree(fr)
    os.makedirs(fr)
    try:
        f = ImageFont.truetype("arialbd.ttf", 26); g = ImageFont.truetype("arial.ttf", 18)
    except OSError:
        f = g = ImageFont.load_default()
    lines, ims = [], []
    t0 = seq[0][0]
    for i, (t, k, fn) in enumerate(seq):
        im = Image.open(fn).convert("RGB")
        if x.crop:
            im = im.crop(tuple(int(v) for v in x.crop.split(",")))
        im = im.resize((1280, 720), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, 1280, 38], fill=(0, 0, 0))
        d.text((12, 5), x.title, fill=(255, 220, 60), font=f)
        sub = "%s  +%.1f s" % (k.rsplit("_", 1)[0], t - t0)
        d.text((1268 - d.textlength(sub, font=g), 10), sub, fill=(220, 220, 220), font=g)
        out = os.path.abspath(os.path.join(fr, "%04d.png" % i)); im.save(out)
        if i % x.sheet_every == 0:
            ims.append(im)
        dur = (seq[i + 1][0] - t) if i + 1 < len(seq) else 1.0
        lines.append("file '%s'\nduration %.3f\n" % (out.replace("\\", "/"), min(max(dur, 0.04), 1.0)))
    lines.append("file '%s'\n" % os.path.abspath(os.path.join(fr, "%04d.png" % (len(seq) - 1))).replace("\\", "/"))
    lst = os.path.join(fr, "list.txt")
    open(lst, "w", encoding="utf-8").write("".join(lines))
    subprocess.run([FFMPEG, "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst, "-vf", "fps=25,format=yuv420p",
                    "-c:v", "libx264", "-crf", "22", "-preset", "medium", x.out], check=True)
    if x.sheet:
        cols = 4; rows = (len(ims) + cols - 1) // cols
        S = Image.new("RGB", (480 * cols, 270 * rows))
        for i, im in enumerate(ims):
            S.paste(im.resize((480, 270), Image.LANCZOS), ((i % cols) * 480, (i // cols) * 270))
        S.save(x.sheet, quality=88)
    shutil.rmtree(fr)
    print("%s: %d frames, %.1f s, %.1f MB" % (x.out, len(seq), seq[-1][0] - t0, os.path.getsize(x.out) / 1e6))


if __name__ == "__main__":
    main()
