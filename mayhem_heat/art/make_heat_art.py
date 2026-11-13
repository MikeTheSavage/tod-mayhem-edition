"""make_heat_art.py - builds the [mayhem_heat] HUD art: heat_icons.png, an RGBA atlas (straight alpha) embedded in
TODGameplay.dll as the resource HEAT_ICONS, and heat_glyphs.h (the atlas layout for the DLL).

The chili icons are generated art supplied for this mod (art/chili/: chili_cold.png and chili_flame_1..5.png, 1254 x 1254
RGBA, made with an image generator; the prompts are not included). This
script only scales, darkens and shadows them. The edge ring and the label figures are drawn here from code; no game art
is copied.

Layout (1024 x 512):
  y   0..159  six icon cells of CELL x CELL (160) px at x = 160 * i:
                i = 0      cold: the no-flame chili darkened (greyscale, brightness x0.42, alpha x0.75)
                i = 1..5   lit:  the chili with flame level i (slot k of the arc always shows flame k)
              In each cell the FULL square source art is ICON (128) px, inset PAD (16) px, so the soft drop shadow
              (offset 8, 10 px, blur 6, alpha 0.55 at this size = 4, 5 / 3 px for a 64 px icon at 1080p) is not clipped.
              The pepper sits at the same place in every variant (only the flame grows); its centre in the square is
              written to heat_glyphs.h (HEAT_PEPPER_U / _V) so the DLL can put the pepper on the slot centre.
  x 0..255, y 160..415  edge ring - 256 x 256 in the radar texture's UV space (radar.BMP is 128 x 128): a soft ring on the
              disc's rim (centre 0.504 / 0.516, rim radius 0.435: numbers measured from radar.BMP, the art itself is not
              used), white; the DLL colours it red / dark for the top-heat pulse
  x 256..507, y 160..287  the score multiplier label figures "x0123456789." (off by default,
              [mayhem_heat] score_label = 0): two rows of six 42 x 64 cells, bold italic cream-to-gold figures with a dark
              outline, drawn from Windows' Arial Black (rendered, then shaded)

The scaled art gets a mild unsharp mask (60 %, 1 px). The DLL premultiplies the atlas and builds the mip chain itself (box filter in premultiplied space); every downscale here
is done in premultiplied space too, so edges do not fringe.

usage: python -I make_heat_art.py [--preview out.png]   (Pillow + numpy)
"""
import os, sys
import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "chili")
OUT = os.path.join(HERE, "heat_icons.png")
HDR = os.path.join(HERE, "heat_glyphs.h")
AW, AH = 1024, 512
CELL, ICON = 160, 128
PAD = (CELL - ICON) // 2
RING_X, RING_Y = 0, 160
GX0, GY0 = 256, 160
# the darkened cold chili and the drop shadow (the numbers picked on the mockup, for a 64 px icon)
COLD_BRIGHT, COLD_ALPHA = 0.42, 0.75
SH_DX, SH_DY, SH_BLUR, SH_ALPHA = 4.0, 5.0, 3.0, 0.55
SH_K = ICON / 64.0
SHARPEN = 60                # unsharp mask percent on the scaled art (radius 1 px)


def premul_resize(img, size):
    a = np.asarray(img.convert("RGBA")).astype(np.float32)
    pm = a.copy(); pm[..., :3] *= a[..., 3:4] / 255.0
    pm_img = Image.fromarray(np.clip(pm + 0.5, 0, 255).astype(np.uint8), "RGBA")
    s = np.asarray(pm_img.resize(size, Image.LANCZOS)).astype(np.float32)
    return s                                             # premultiplied float, 0..255


def unpremul(pm):
    al = pm[..., 3:4]
    rgb = np.where(al > 0, pm[..., :3] * 255.0 / np.maximum(al, 1e-3), 0)
    return Image.fromarray(np.dstack([np.clip(rgb + 0.5, 0, 255), np.clip(pm[..., 3:4] + 0.5, 0, 255)]).astype(np.uint8), "RGBA")


def load_src(name):
    im = Image.open(os.path.join(SRC, name)).convert("RGBA")
    a = np.asarray(im).astype(np.float32)
    # the generator leaves faint speckle (alpha 1..5) over the empty canvas: clear it with a soft knee
    al = a[..., 3]
    a[..., 3] = np.clip((al - 4.0) * (255.0 / 251.0), 0, 255)
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGBA")


def cell(name, cold):
    """one CELL x CELL icon: the source square at ICON px, inset PAD, over its drop shadow (premultiplied float)"""
    pm = premul_resize(load_src(name), (ICON, ICON))
    # a mild unsharp mask: the HUD shows the cell at about half this size (mip 1, box filtered), which softens it
    img = Image.fromarray(np.clip(pm + 0.5, 0, 255).astype(np.uint8), "RGBA")
    pm = np.asarray(img.filter(ImageFilter.UnsharpMask(radius=1.0, percent=SHARPEN, threshold=0))).astype(np.float32)
    pm[..., :3] = np.minimum(pm[..., :3], pm[..., 3:4])     # still a valid premultiplied colour
    if cold:
        rgb = pm[..., :3]
        grey = rgb[..., 0:1] * 0.299 + rgb[..., 1:2] * 0.587 + rgb[..., 2:3] * 0.114
        pm = np.dstack([np.repeat(grey, 3, -1) * COLD_BRIGHT, pm[..., 3:4]]) * COLD_ALPHA
    art = np.zeros((CELL, CELL, 4), np.float32)
    art[PAD:PAD + ICON, PAD:PAD + ICON] = pm
    # shadow: the icon's alpha moved down-right and blurred, black
    al = Image.fromarray(np.clip(art[..., 3] + 0.5, 0, 255).astype(np.uint8), "L")
    al = al.transform(al.size, Image.AFFINE, (1, 0, -SH_DX * SH_K, 0, 1, -SH_DY * SH_K), resample=Image.BILINEAR)
    al = al.filter(ImageFilter.GaussianBlur(SH_BLUR * SH_K))
    sh = np.asarray(al).astype(np.float32) * SH_ALPHA
    out = art.copy()                                      # art over shadow (premultiplied "over")
    out[..., 3] = art[..., 3] + sh * (1.0 - art[..., 3] / 255.0)
    return out


def pepper_centre():
    """the pepper's box centre in the square (the cold variant: no flame), as 0..1 fractions"""
    a = np.asarray(Image.open(os.path.join(SRC, "chili_cold.png")))[..., 3] > 128
    ys, xs = np.where(a)
    n = a.shape[0]
    return (xs.min() + xs.max() + 1) * 0.5 / n, (ys.min() + ys.max() + 1) * 0.5 / n


def ring():
    n = 256
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    u = (x + 0.5) / n; v = (y + 0.5) / n
    d = np.sqrt((u - 0.504) ** 2 + (v - 0.516) ** 2)
    r0 = 0.435
    # a crisp band on the rim plus a soft glow spilling inward (and a little outward)
    band = np.exp(-((d - r0) / 0.016) ** 2)
    inner = np.exp(-((d - (r0 - 0.03)) / 0.05) ** 2) * (d < r0) * 0.55
    outer = np.exp(-((d - (r0 + 0.012)) / 0.02) ** 2) * 0.6
    a = np.clip(np.maximum(band, np.maximum(inner, outer)), 0, 1)
    img = np.dstack([np.full_like(a, 255), np.full_like(a, 255), np.full_like(a, 255), a * 255]).astype(np.uint8)
    return Image.fromarray(img, "RGBA")


GLYPHS = "x0123456789."
CELL_W, CELL_H = 42, 64
FONT = os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts", "ariblk.ttf")


def glyph(ch):
    """one figure: bold italic, cream (top) to gold (bottom) fill, dark outline, soft drop shadow; returns the
    straight-alpha RGBA cell (CELL_W x CELL_H, figure left-aligned) and its ink width in pixels"""
    from PIL import ImageDraw, ImageFont
    S = 4
    W, H = CELL_W * S * 2, CELL_H * S
    size = int(H * (0.62 if ch != "x" else 0.50))
    f = ImageFont.truetype(FONT, size)
    m = Image.new("L", (W, H), 0)
    asc, _ = f.getmetrics()
    base = int(H * 0.80)                                    # common baseline for every figure
    ImageDraw.Draw(m).text((S * 8, base - asc), ch, font=f, fill=255)
    k = 0.18                                                # italic: sheared to the right going up (~10 degrees)
    m = m.transform(m.size, Image.AFFINE, (1, k, -k * base, 0, 1, 0), resample=Image.BICUBIC)
    fill = np.asarray(m).astype(np.float32) / 255.0
    stroke = int(S * 3.2)
    ol = np.asarray(m.filter(ImageFilter.MaxFilter(2 * stroke + 1)).filter(ImageFilter.GaussianBlur(S * 0.6))).astype(np.float32) / 255.0
    sh = np.roll(np.roll(ol, int(S * 1.6), 0), int(S * 1.2), 1)
    sh = np.asarray(Image.fromarray((sh * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(S * 1.5))).astype(np.float32) / 255.0
    yy = np.mgrid[0:H, 0:W][0].astype(np.float32) / H
    t = np.clip((yy - 0.30) / 0.50, 0, 1)[..., None]       # 0 at the cap height .. 1 at the baseline
    top = np.array([255, 247, 205], np.float32); bot = np.array([236, 172, 52], np.float32)
    col = top * (1 - t) + bot * t
    up = np.clip(fill - np.roll(fill, int(S * 1.5), 0), 0, 1)[..., None]   # a thin bright bevel on the stroke tops
    col = col + up * 30
    dark = np.array([46, 24, 8], np.float32)
    rgb = dark * (1 - fill[..., None]) + col * fill[..., None]
    a = np.maximum(ol, fill)
    a_sh = sh * 0.45                                        # the shadow: black, under everything
    out_a = a + a_sh * (1 - a)
    rgb = rgb * a[..., None] / np.maximum(out_a, 1e-4)[..., None]
    img = np.dstack([np.clip(rgb, 0, 255), np.clip(out_a * 255, 0, 255)]).astype(np.uint8)
    cols = np.where(out_a.max(0) > 0.02)[0]
    x0, x1 = (int(cols[0]), int(cols[-1]) + 1) if len(cols) else (0, S)
    crop = Image.fromarray(img, "RGBA").crop((x0, 0, x1, H))
    w = max(1, min(CELL_W, int(round((x1 - x0) / S))))
    small = unpremul(premul_resize(crop, (w, CELL_H)))
    c = Image.new("RGBA", (CELL_W, CELL_H), (0, 0, 0, 0))
    c.paste(small, (0, 0))
    return c, w


def glyphs(atlas):
    rows = []
    for i, ch in enumerate(GLYPHS):
        c, w = glyph(ch)
        x, y = GX0 + (i % 6) * CELL_W, GY0 + (i // 6) * CELL_H
        atlas.paste(c, (x, y))
        rows.append((ch, x, y, w, CELL_H, max(4, w - 9)))  # advance: tight, the outlines overlap like the game's numbers
    return rows


def write_header(rows, pu, pv):
    lines = ["// heat_glyphs.h - written by mayhem_heat/art/make_heat_art.py: the layout of heat_icons.png (atlas pixels).",
             "#pragma once",
             "// the chili icons: cell i (0 = cold, 1..5 = flame level) at x = i * HEAT_CELL, y = 0; the full square source",
             "// art is HEAT_ICON px inset HEAT_PAD px in its cell (the margin holds the drop shadow)",
             "static const int HEAT_ATLAS_W = %d, HEAT_ATLAS_H = %d;" % (AW, AH),
             "static const int HEAT_CELL = %d, HEAT_ICON = %d, HEAT_PAD = %d;" % (CELL, ICON, PAD),
             "// the pepper's box centre in the square art (fractions): the DLL puts it on the slot centre",
             "static const float HEAT_PEPPER_U = %.4ff, HEAT_PEPPER_V = %.4ff;" % (pu, pv),
             "// the edge ring: 256 x 256 = radar UV 0..1",
             "static const int HEAT_RING_X = %d, HEAT_RING_Y = %d, HEAT_RING_N = 256;" % (RING_X, RING_Y),
             "// the score multiplier label figures: {char, x, y, w, h, advance}; every cell has its baseline at 80 % of h",
             "struct HeatGlyph { char c; short x, y, w, h, adv; };",
             "static const HeatGlyph HEAT_GLYPHS[%d] = {" % len(rows)]
    lines += ["    {'%s', %d, %d, %d, %d, %d}," % r for r in rows]
    lines += ["};", ""]
    with open(HDR, "w", encoding="utf-8", newline="") as fh:
        fh.write("".join(l + chr(13) + chr(10) for l in lines))   # CRLF like the sources
    print("wrote", HDR)


def main():
    atlas = Image.new("RGBA", (AW, AH), (0, 0, 0, 0))
    names = ["chili_cold.png"] + ["chili_flame_%d.png" % k for k in range(1, 6)]
    for i, n in enumerate(names):
        atlas.paste(unpremul(cell(n, i == 0)), (i * CELL, 0))
    atlas.paste(ring(), (RING_X, RING_Y))
    rows = glyphs(atlas)
    pu, pv = pepper_centre()
    write_header(rows, pu, pv)
    atlas.save(OUT, optimize=True)
    print("wrote", OUT, os.path.getsize(OUT), "bytes; pepper centre %.4f %.4f" % (pu, pv))
    if "--preview" in sys.argv:
        out = sys.argv[sys.argv.index("--preview") + 1]
        bg = Image.new("RGBA", (1100, 260), (176, 150, 112, 255))
        for i in range(6):
            c = atlas.crop((i * CELL, 0, (i + 1) * CELL, CELL))
            bg.alpha_composite(c, (10 + i * 170, 10))
            bg.alpha_composite(c.resize((80, 80), Image.LANCZOS), (10 + i * 170, 175))
        bg.save(out); print("preview", out)


if __name__ == "__main__":
    main()
