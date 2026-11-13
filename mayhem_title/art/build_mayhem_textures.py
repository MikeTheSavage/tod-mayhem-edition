"""build_mayhem_textures.py - the two textures [mayhem_title] loads at run time (build time, Python 3.12 + Pillow + numpy
+ scipy). Source: mayhem_lettering_v3.png (2688 x 1520 RGBA, lettering made by the author with
Higgsfield). Nothing is taken from the game.

The effect region E = the lettering's alpha bounding box grown by MARGIN of its width on every side (room for the
burnt hole, its rim and the scorch around it). Both textures cover E exactly (stretched to their power-of-two size; the
DLL maps them back with E's real aspect, written into the fx file's header).

  TODGameplay_mayhem.dds      2048 x 1024 A8R8G8B8, full mip chain, PREMULTIPLIED alpha: the lettering
  TODGameplay_mayhem_fx.dds   1024 x 512 A8R8G8B8, one level, the effect channels:
      R  hole field: 0.5 at the edge of the burnt hole, < 0.5 inside, > 0.5 outside (signed distance / BAND px,
         clamped); the hole = the lettering's alpha dilated and roughened with noise, so it hugs the word
      G  shimmer weight: the painted flames (heat-haze distortion only there)
      B  ember-crack weight: the glowing cracks / hot spots inside the letters (they flicker)
      A  burn noise: fBm with fine detail (the burn front's ragged, paper-like edge)
  The fx file's 128-byte header carries, in its reserved words (dwReserved1[0..3], as floats): E's aspect (width /
  height), the margin, and the lettering's bounding box inside E (x0, y0 as fractions) for reference.

  python build_mayhem_textures.py [--out DIR] [--preview]
Output default: <repo>/mayhem_title/art/build/ (the DLL build copies them to dist/).
"""
import argparse, os, struct
import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "mayhem_lettering_v3.png")
MARGIN = 0.06            # of the bbox width, every side
LW, LH = 2048, 1024      # lettering texture
FW, FH = 1024, 512       # fx texture
BAND = 40.0              # fx px: the hole field goes 0 -> 1 over +-BAND px around the edge


def dds_header(w, h, mips, flags_extra=0):
    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PITCH, DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT = 1, 2, 4, 8, 0x1000, 0x20000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (DDSD_MIPMAPCOUNT if mips > 1 else 0)
    caps = 0x1000 | ((0x8 | 0x400000) if mips > 1 else 0)   # TEXTURE (| COMPLEX | MIPMAP)
    pf = struct.pack("<IIIIIIII", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)   # RGB | ALPHAPIXELS, A8R8G8B8
    return flags, caps, pf


def write_dds(path, levels, reserved=(0.0,) * 11):
    """levels: list of HxWx4 uint8 arrays in RGBA order (written as BGRA = A8R8G8B8)"""
    h, w = levels[0].shape[:2]
    flags, caps, pf = dds_header(w, h, len(levels))
    res = struct.pack("<11f", *reserved)
    hdr = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels)) + res + pf + struct.pack("<IIIII", caps, 0, 0, 0, 0)
    assert len(hdr) == 128
    with open(path, "wb") as f:
        f.write(hdr)
        for lv in levels:
            f.write(np.ascontiguousarray(lv[..., [2, 1, 0, 3]]).tobytes())


def fbm(shape, base, octaves, seed):
    rng = np.random.default_rng(seed)
    out = np.zeros(shape, np.float32); amp, tot, sc = 1.0, 0.0, base
    for _ in range(octaves):
        gh, gw = max(2, int(shape[0] / sc) + 2), max(2, int(shape[1] / sc) + 2)
        g = rng.random((gh, gw)).astype(np.float32)
        up = np.asarray(Image.fromarray(g, "F").resize((int((gw - 1) * sc) + 1, int((gh - 1) * sc) + 1), Image.BICUBIC))
        out += amp * up[: shape[0], : shape[1]]; tot += amp; amp *= 0.55; sc = max(1.5, sc / 2)
    out /= tot
    lo, hi = np.percentile(out, 0.5), np.percentile(out, 99.5)
    return np.clip((out - lo) / (hi - lo), 0, 1)


def build(out_dir, preview):
    os.makedirs(out_dir, exist_ok=True)
    src = np.asarray(Image.open(SRC).convert("RGBA")).astype(np.float32) / 255.0
    al = src[..., 3]
    ys, xs = np.where(al > 0.03)
    x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    bw, bh = x1 - x0, y1 - y0
    m = int(round(MARGIN * bw))
    ew, eh = bw + 2 * m, bh + 2 * m
    E = np.zeros((eh, ew, 4), np.float32)
    E[m:m + bh, m:m + bw] = src[y0:y1, x0:x1]
    aspect = ew / eh
    print("bbox %d,%d %dx%d, region E %dx%d (aspect %.4f, margin %d px)" % (x0, y0, bw, bh, ew, eh, aspect, m))

    # ---- lettering: premultiplied, LW x LH, mip chain (box filter in premultiplied space)
    pm = E.copy(); pm[..., :3] *= pm[..., 3:4]
    chans = [np.asarray(Image.fromarray(pm[..., c], "F").resize((LW, LH), Image.LANCZOS)) for c in range(4)]
    lv = np.clip(np.dstack(chans), 0, 1)
    lv[..., :3] = np.minimum(lv[..., :3], lv[..., 3:4])          # premultiplied: colour <= alpha
    levels = []
    cur = lv
    while True:
        levels.append(np.clip(cur * 255 + 0.5, 0, 255).astype(np.uint8))
        h, w = cur.shape[:2]
        if w == 1 and h == 1:
            break
        nh, nw = max(1, h // 2), max(1, w // 2)
        cur = cur[: nh * 2 if h > 1 else 1, : nw * 2 if w > 1 else 1]
        cur = cur.reshape(nh, cur.shape[0] // nh, nw, cur.shape[1] // nw, 4).mean(axis=(1, 3))
    write_dds(os.path.join(out_dir, "TODGameplay_mayhem.dds"), levels)

    # ---- fx channels at FW x FH over E
    a = np.asarray(Image.fromarray(E[..., 3], "F").resize((FW, FH), Image.LANCZOS))
    rgb = np.dstack([np.asarray(Image.fromarray(E[..., c], "F").resize((FW, FH), Image.LANCZOS)) for c in range(3)])
    sx = FW / ew                                             # fx px per E px (x); y differs (stretch), use the mean
    # hole: the lettering dilated (about 1.4 % of the bbox width) and roughened, then smoothed a little
    solid = a > 0.25
    dist_out = ndimage.distance_transform_edt(~solid)        # fx px outside the lettering
    rough = fbm((FH, FW), 22, 4, 7)
    r0 = 0.014 * bw * sx
    hole = dist_out < r0 * (0.55 + 0.9 * rough)
    hole = ndimage.binary_closing(hole, iterations=3)
    hole = ndimage.binary_fill_holes(hole)
    hole = ndimage.binary_opening(hole, iterations=2)
    di = ndimage.distance_transform_edt(hole)
    do = ndimage.distance_transform_edt(~hole)
    sd = np.where(hole, -di, do)                             # signed: < 0 inside
    R = np.clip(0.5 + sd / (2 * BAND), 0, 1)
    # shimmer: the painted flames sit on the top edge of the word: per column, a band from the top of the lettering
    # down by FLAME of the bbox height (full strength in its upper half, fading out below), only where there is paint
    FLAME = 0.30
    has = ndimage.binary_opening(a > 0.3, iterations=2)       # no loose sparks: only the painted body + flames
    lab, n = ndimage.label(has)
    if n:
        sizes = ndimage.sum(has, lab, range(1, n + 1))
        has = np.isin(lab, 1 + np.where(sizes > 400)[0])
    top = np.where(has.any(axis=0), has.argmax(axis=0), FH).astype(np.float32)
    top = ndimage.minimum_filter1d(ndimage.median_filter(top, 15), 9)
    yy = np.arange(FH)[:, None].astype(np.float32)
    fh = FLAME * bh * FH / eh
    rel = (yy - top[None, :]) / max(fh, 1.0)
    G = np.clip(1.5 - 1.5 * rel, 0, 1) * (rel >= -0.02) * np.clip(a * 2.0, 0, 1)
    G = ndimage.gaussian_filter(G.astype(np.float32), 2.0)
    # ember cracks: thin lines redder than their surroundings, inside the letter bodies (not their rims, not the flames)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    red = np.clip(r - 0.75 * g - 0.2 * b, 0, 1) * (r > 0.45)
    local = ndimage.gaussian_filter(red, 5.0)
    inner = ndimage.binary_erosion(a > 0.9, iterations=5)
    crack = np.clip((red - local - 0.10) * 7, 0, 1) * inner * (G < 0.3)
    B = np.clip(ndimage.gaussian_filter(crack, 0.7) * 1.5, 0, 1)
    A = fbm((FH, FW), 48, 6, 11)
    fx = np.dstack([R, G, B, A])
    # the fire layer's base line: the flame tops fitted with a line (v = base0 + slope u), moved down by the flame band
    cols = np.where((top < FH) & (np.arange(FW) > 0.10 * FW) & (np.arange(FW) < 0.90 * FW))[0]
    slope, inter = np.polyfit(cols / FW, top[cols] / FH, 1)
    base0 = inter + FLAME * bh / eh
    res = [aspect, MARGIN, m / ew, m / eh, (m + bw) / ew, (m + bh) / eh, BAND, base0, slope, FLAME * bh / eh * 1.3, 0.0]
    print("fire base line v = %.4f + %.4f u, band %.4f" % (base0, slope, res[9]))
    write_dds(os.path.join(out_dir, "TODGameplay_mayhem_fx.dds"), [np.clip(fx * 255 + 0.5, 0, 255).astype(np.uint8)], res)
    np.save(os.path.join(out_dir, "fx.npy"), fx.astype(np.float32))          # for the offline preview
    np.save(os.path.join(out_dir, "letters_pm.npy"), lv.astype(np.float32)[::2, ::2])
    open(os.path.join(out_dir, "region.txt"), "w").write("aspect %.6f\nmargin %.4f\nbbox_in_E %.6f %.6f %.6f %.6f\n" % tuple(res[:6]))
    if preview:
        sheet = Image.new("RGB", (FW * 2, FH * 2))
        for i, ch in enumerate([R, G, B, A]):
            sheet.paste(Image.fromarray((ch * 255).astype(np.uint8)).convert("RGB"), ((i % 2) * FW, (i // 2) * FH))
        sheet.save(os.path.join(out_dir, "fx_channels.png"))
    for n in ("TODGameplay_mayhem.dds", "TODGameplay_mayhem_fx.dds"):
        print(n, os.path.getsize(os.path.join(out_dir, n)), "bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "build"))
    ap.add_argument("--preview", action="store_true")
    a = ap.parse_args()
    build(a.out, a.preview)


if __name__ == "__main__":
    main()
