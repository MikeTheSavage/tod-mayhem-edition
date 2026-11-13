"""radar_sim.py - what the radar will look like, without the game (dev tool: needs numpy and Pillow, not used by the installer).

Takes a mosaic made by `TODMinimapBake --preview` (straight-alpha ink, north up) and draws the disc the way the DLL does:
heading-up rotation, the DLL's level / mip choice, trilinear sampling, the rim fade, over a parchment like the radar's.

  python radar_sim.py MOSAIC.png --nw X Z --texel T --at X Z [--heading DEG] [--res 1080 | 1964] [--zoom 0.75] [--out a.png]

The picture is 1:1 pixels at that resolution; --scale N enlarges it with nearest for looking at single pixels.
"""
import argparse
import math

import numpy as np
from PIL import Image

DISC_R, BLIP_R = 52.5, 54.0


def mips(rgba):
    """premultiplied box chain, as the DLL builds it (alpha-weighted colour)"""
    a = rgba.astype(np.float32) / 255.0
    a[..., :3] *= a[..., 3:4]
    out = [a]
    while min(out[-1].shape[:2]) > 8:
        x = out[-1]
        h, w = x.shape[0] // 2 * 2, x.shape[1] // 2 * 2
        x = x[:h, :w]
        out.append((x[0::2, 0::2] + x[1::2, 0::2] + x[0::2, 1::2] + x[1::2, 1::2]) * 0.25)
    return out


def bilinear(img, u, v):
    h, w = img.shape[:2]
    x = u * w - 0.5
    y = v * h - 0.5
    x0 = np.floor(x).astype(int)
    y0 = np.floor(y).astype(int)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    x0c, x1c = np.clip(x0, 0, w - 1), np.clip(x0 + 1, 0, w - 1)
    y0c, y1c = np.clip(y0, 0, h - 1), np.clip(y0 + 1, 0, h - 1)
    return (img[y0c, x0c] * (1 - fx) + img[y0c, x1c] * fx) * (1 - fy) + (img[y1c, x0c] * (1 - fx) + img[y1c, x1c] * fx) * fy


def parchment(n, r):
    """a disc like radar.BMP's: warm paper, the orange rim glow; outside dark so the disc edge shows"""
    yy, xx = np.mgrid[0:n, 0:n] + 0.5
    d = np.hypot(xx - n / 2, yy - n / 2) / r
    base = np.array([246, 228, 176], np.float32) / 255
    rim = np.array([233, 164, 74], np.float32) / 255
    k = np.clip((d - 0.82) / 0.18, 0, 1)[..., None] ** 2
    img = base * (1 - k) + rim * k
    img = img * (1 - (d > 1.0)[..., None]) + np.array([0.08, 0.05, 0.03]) * (d > 1.0)[..., None]
    return img


def sim(mosaic, nw, texel, at, heading, res, zoom, opacity=0.9, lod_bias=-0.25):
    unit = res / 480.0                                # the radar's unit at this height (HUD canvas 480)
    range_m = 50.0 / zoom
    px_per_m = BLIP_R / range_m * unit
    r_px = DISC_R * unit
    n = int(r_px * 2.2) | 1
    yy, xx = np.mgrid[0:n, 0:n] + 0.5
    ux = (xx - n / 2) / px_per_m
    uy = (yy - n / 2) / px_per_m
    h = math.radians(heading)
    fx, fz = math.sin(h), math.cos(h)
    side, fwd = ux, -uy
    wx = at[0] + side * fz + fwd * fx
    wz = at[1] - side * fx + fwd * fz
    pyr = mips(mosaic)
    H, W = mosaic.shape[:2]
    u = (wx - nw[0]) / (W * texel)
    v = (nw[1] - wz) / (H * texel)
    lod = max(0.0, math.log2(max(1e-6, 1.0 / px_per_m / texel)) + lod_bias)
    l0 = min(int(lod), len(pyr) - 1)
    l1 = min(l0 + 1, len(pyr) - 1)
    f = lod - int(lod)
    c = bilinear(pyr[l0], u, v) * (1 - f) + bilinear(pyr[l1], u, v) * f
    d = np.hypot(xx - n / 2, yy - n / 2) / r_px
    mask = np.clip((1.0 - d) / 0.10, 0, 1)[..., None]
    paper = parchment(n, r_px)
    a = c[..., 3:4] * opacity * mask
    return paper * (1 - a) + c[..., :3] * opacity * mask, lod


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('mosaic')
    ap.add_argument('--nw', nargs=2, type=float, required=True)
    ap.add_argument('--texel', type=float, required=True)
    ap.add_argument('--at', nargs=2, type=float, required=True)
    ap.add_argument('--heading', type=float, default=0.0)
    ap.add_argument('--res', type=int, default=1080)
    ap.add_argument('--zoom', type=float, default=0.75)
    ap.add_argument('--scale', type=int, default=1)
    ap.add_argument('--out', default='sim.png')
    a = ap.parse_args()
    m = np.asarray(Image.open(a.mosaic).convert('RGBA'))
    img, lod = sim(m, a.nw, a.texel, a.at, a.heading, a.res, a.zoom)
    im = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8))
    if a.scale > 1:
        im = im.resize((im.width * a.scale, im.height * a.scale), Image.NEAREST)
    im.save(a.out)
    print('%s %dx%d lod %.2f' % (a.out, im.width, im.height, lod))


if __name__ == '__main__':
    main()
