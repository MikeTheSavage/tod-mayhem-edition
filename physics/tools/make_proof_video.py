"""make_proof_video.py - side-by-side MP4 of two test-harness runs (harness not
included) that played the same input script (vanilla vs physics).

  python -I make_proof_video.py RUN_A RUN_B OUT.mp4 [--label-a TEXT] [--label-b TEXT] [--prefix ph_]

Frames are the shots both runs share (same mark names), in capture order of run B; each frame is shown for the time
until the next capture of run B (clamped 0.04 .. 1.5 s). Scaled to 960x540 each, labelled, stacked left / right, encoded
with ffmpeg (H.264, yuv420p). Read only on the runs; writes OUT.mp4 and a frames folder next to it.
"""
import argparse, json, os, shutil, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def shots(run, prefix):
    r = json.load(open(os.path.join(run, "result.json"), encoding="utf-8"))
    out = [(k, v["file"], v["t"]) for k, v in r["shots"].items() if k.startswith(prefix)]
    return sorted(out, key=lambda x: (x[2], x[0]))


def label(im, text, sub):
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype("arialbd.ttf", 30); g = ImageFont.truetype("arial.ttf", 18)
    except OSError:
        f = g = ImageFont.load_default()
    d.rectangle([0, 0, im.width, 44], fill=(0, 0, 0))
    d.text((12, 6), text, fill=(255, 220, 60), font=f)
    d.text((im.width - 12 - d.textlength(sub, font=g), 14), sub, fill=(220, 220, 220), font=g)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b"); ap.add_argument("out")
    ap.add_argument("--label-a", default="vanilla"); ap.add_argument("--label-b", default="Box3D physics")
    ap.add_argument("--prefix", default="ph_")
    x = ap.parse_args()
    A = {k: f for k, f, t in shots(x.a, x.prefix)}
    B = shots(x.b, x.prefix)
    seq = [(k, A[k], fb, t) for k, fb, t in B if k in A]
    if not seq:
        sys.exit("no shared shots")
    fr = os.path.splitext(x.out)[0] + "_frames"
    if os.path.exists(fr):
        shutil.rmtree(fr)
    os.makedirs(fr)
    lines = []
    for i, (k, fa, fb, t) in enumerate(seq):
        ia = Image.open(fa).convert("RGB").resize((960, 540), Image.LANCZOS)
        ib = Image.open(fb).convert("RGB").resize((960, 540), Image.LANCZOS)
        step = k.split("_")[1] if "_" in k else k
        label(ia, x.label_a, step); label(ib, x.label_b, step)
        im = Image.new("RGB", (1920, 540)); im.paste(ia, (0, 0)); im.paste(ib, (960, 0))
        fn = os.path.abspath(os.path.join(fr, "%04d.png" % i)); im.save(fn)
        dur = (seq[i + 1][3] - t) if i + 1 < len(seq) else 1.5
        dur = min(max(dur, 0.04), 1.5)
        lines.append("file '%s'\nduration %.3f\n" % (fn.replace("\\", "/"), dur))
    lines.append("file '%s'\n" % os.path.abspath(os.path.join(fr, "%04d.png" % (len(seq) - 1))).replace("\\", "/"))
    lst = os.path.join(fr, "list.txt")
    open(lst, "w", encoding="utf-8").write("".join(lines))
    cmd = [FFMPEG, "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst, "-vf", "fps=25,format=yuv420p",
           "-c:v", "libx264", "-crf", "23", "-preset", "medium", x.out]
    subprocess.run(cmd, check=True)
    print("%s: %d frames, %.1f MB" % (x.out, len(seq), os.path.getsize(x.out) / 1e6))


if __name__ == "__main__":
    main()
