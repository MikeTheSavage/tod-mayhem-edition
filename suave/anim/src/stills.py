"""stills.py - a row of wide stills of a pair npz (Blender, game meshes), for a quick look.

  python stills.py PARTNER NPZ OUT.png f1,f2,...   (frames of the npz; keys lead_G / follow_G ...)
"""
import os, sys
from PIL import Image, ImageDraw
sys.dont_write_bytecode = True
import closeup as cu

if __name__ == "__main__":
    partner, npz, out, frames = sys.argv[1], sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4].split(",")]
    fps = 30 if "_30" in npz else 25
    picks = [(f, "lead", "L") for f in frames]
    tag = "stills_" + os.path.basename(npz).replace(".npz", "")
    mode = sys.argv[5] if len(sys.argv) > 5 else True
    jp, od = cu.job_for(partner, npz, picks, tag, size=(480, 400), wide=mode)
    cu.render(jp, os.path.join(cu.WORK, tag + ".log"))
    ims = [Image.open(os.path.join(od, "f%05d.png" % k)).convert("RGB") for k in range(len(picks))]
    w, h = ims[0].size
    S = Image.new("RGB", (w * len(ims), h), (20, 20, 20))
    dr = ImageDraw.Draw(S)
    for k, im in enumerate(ims):
        S.paste(im, (k * w, 0))
        lab = "%s  frame %d  %.2f s  beat %.2f" % (os.path.basename(npz), frames[k], frames[k] / fps, frames[k] / fps / cu.pr.BEAT)
        dr.rectangle([k * w, 0, k * w + 8 + 6 * len(lab), 15], fill=(0, 0, 0))
        dr.text((k * w + 4, 2), lab, fill=(255, 255, 255))
    S.save(out)
    print(out)
