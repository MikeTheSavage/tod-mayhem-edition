"""make_clip.py - a proof clip from a burst of PrintWindow shots of a test-copy run (ffmpeg of DAZ Studio 6, libx264).
  python -I make_clip.py RUN_DIR PREFIX OUT.mp4 [--fps 20] [--title TEXT] [--crop x0,y0,x1,y1] [--scale W]
The frames are taken in name order; the title is burnt into the top left (PIL), the clip is H.264 yuv420p."""
import argparse, glob, os, shutil, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFont

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run"); ap.add_argument("prefix"); ap.add_argument("out")
    ap.add_argument("--fps", type=float, default=20); ap.add_argument("--title", default="")
    ap.add_argument("--crop", default=None); ap.add_argument("--scale", type=int, default=1280)
    ap.add_argument("--hold", type=int, default=0, help="repeat the last frame N times")
    a = ap.parse_args()
    fs = sorted(glob.glob(os.path.join(a.run, "shots", a.prefix + "*.png")))
    if not fs:
        raise SystemExit("no frames")
    tmp = tempfile.mkdtemp(prefix="rdclip_")
    try:
        font = ImageFont.truetype("arial.ttf", 28)
    except OSError:
        font = ImageFont.load_default()
    frames = fs + [fs[-1]] * a.hold
    for i, f in enumerate(frames):
        im = Image.open(f).convert("RGB")
        if a.crop:
            im = im.crop([int(v) for v in a.crop.split(",")])
        h = int(im.size[1] * a.scale / im.size[0]) // 2 * 2
        im = im.resize((a.scale, h), Image.LANCZOS)
        if a.title:
            d = ImageDraw.Draw(im)
            d.rectangle([0, 0, im.size[0], 44], fill=(0, 0, 0))
            d.text((12, 8), a.title, fill=(255, 210, 60), font=font)
        im.save(os.path.join(tmp, "f%04d.png" % i))
    subprocess.run([FFMPEG, "-y", "-loglevel", "error", "-framerate", str(a.fps), "-i", os.path.join(tmp, "f%04d.png"),
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", a.out], check=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print("%s: %d frames at %g fps" % (a.out, len(frames), a.fps))


if __name__ == "__main__":
    main()
