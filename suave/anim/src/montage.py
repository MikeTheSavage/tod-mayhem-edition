"""montage.py - tile PNG frames into one sheet with time labels.

  python montage.py OUT.png COLS WIDTH frame1.png frame2.png ...   (labels = frame number / 30 fps)
"""
import os, re, sys
from PIL import Image, ImageDraw


def main():
    out, cols, w = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    files = sys.argv[4:]
    ims = [Image.open(f).convert("RGB") for f in files]
    h = int(ims[0].height * w / ims[0].width)
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w, rows * h), (20, 20, 20))
    dr = ImageDraw.Draw(sheet)
    for k, (f, im) in enumerate(zip(files, ims)):
        x, y = (k % cols) * w, (k // cols) * h
        sheet.paste(im.resize((w, h)), (x, y))
        m = re.search(r"(\d+)\.png$", f)
        lab = "%s  %.2f s" % (os.path.basename(os.path.dirname(f)).replace("frames_", ""), int(m.group(1)) / 30.0) if m else ""
        dr.rectangle([x, y, x + 8 + 7 * len(lab), y + 16], fill=(0, 0, 0))
        dr.text((x + 4, y + 2), lab, fill=(255, 255, 255))
    sheet.save(out)
    print(out, sheet.size)


if __name__ == "__main__":
    main()
