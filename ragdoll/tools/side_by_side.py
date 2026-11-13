"""side_by_side.py BEFORE_RUN AFTER_RUN PREFIX OUT_RUN [--crop x0,y0,x1,y1] [--h 540]
Joins the burst frames of two harness runs (shots/PREFIX_NN.png) side by side, BEFORE left and AFTER right, labelled,
into OUT_RUN/shots/PREFIX_NN.png, so make_clip.py can turn them into one comparison clip (ragdoll-hits proof)."""
import argparse, glob, os
from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before"); ap.add_argument("after"); ap.add_argument("prefix"); ap.add_argument("out")
    ap.add_argument("--crop", default=None); ap.add_argument("--h", type=int, default=540)
    ap.add_argument("--left", default="BEFORE (installed build)"); ap.add_argument("--right", default="AFTER (ragdoll-hits)")
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "shots"), exist_ok=True)
    n = 0
    for p in sorted(glob.glob(os.path.join(a.before, "shots", a.prefix + "_[0-9][0-9].png"))):
        q = os.path.join(a.after, "shots", os.path.basename(p))
        if not os.path.exists(q):
            continue
        pair = []
        for f, label in ((p, a.left), (q, a.right)):
            im = Image.open(f).convert("RGB")
            if a.crop:
                im = im.crop([int(v) for v in a.crop.split(",")])
            im = im.resize((int(im.width * a.h / im.height), a.h))
            d = ImageDraw.Draw(im)
            d.rectangle([0, a.h - 20, 8 + 7 * len(label), a.h], fill=(0, 0, 0)); d.text((5, a.h - 17), label, fill=(255, 255, 255))
            pair.append(im)
        out = Image.new("RGB", (pair[0].width + pair[1].width + 6, a.h), (255, 255, 255))
        out.paste(pair[0], (0, 0)); out.paste(pair[1], (pair[0].width + 6, 0))
        out.save(os.path.join(a.out, "shots", os.path.basename(p)))
        n += 1
    print(a.prefix, n, "frames")


if __name__ == "__main__":
    main()
