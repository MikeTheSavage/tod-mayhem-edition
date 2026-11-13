"""sheet.py RUN_DIR PREFIX OUT.jpg [--frames 0,2,4,...] [--crop x0,y0,x1,y1] [--cols 6] [--w 240] [--label TEXT]
Contact sheet of harness burst frames (shots/PREFIX_NN.png), each labelled with its frame number (30 ms apart in the
ragdoll-hits runs), for picking stills and comparing before / after side by side."""
import argparse, os
from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run"); ap.add_argument("prefix"); ap.add_argument("out")
    ap.add_argument("--frames", default="0,2,4,6,8,10,12,14,16,20,26,34")
    ap.add_argument("--crop", default=None); ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--w", type=int, default=240); ap.add_argument("--label", default="")
    a = ap.parse_args()
    ims = []
    for f in [int(v) for v in a.frames.split(",")]:
        p = os.path.join(a.run, "shots", "%s_%02d.png" % (a.prefix, f))
        if not os.path.exists(p):
            continue
        im = Image.open(p).convert("RGB")
        if a.crop:
            im = im.crop([int(v) for v in a.crop.split(",")])
        im = im.resize((a.w, int(im.height * a.w / im.width)))
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, 70, 14], fill=(0, 0, 0)); d.text((3, 1), "%s %02d" % (a.label, f), fill=(255, 255, 255))
        ims.append(im)
    rows = (len(ims) + a.cols - 1) // a.cols
    h = ims[0].height
    out = Image.new("RGB", (a.w * a.cols, h * rows))
    for k, im in enumerate(ims):
        out.paste(im, ((k % a.cols) * a.w, (k // a.cols) * h))
    out.save(a.out, quality=90)
    print(a.out, len(ims), "frames")


if __name__ == "__main__":
    main()
