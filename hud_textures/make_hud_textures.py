r"""HD HUD icons: the approved art (straight-alpha PNGs) -> engine-ready texture payloads in hud_textures\art\.

  python -I hud_textures\make_hud_textures.py [--art <repo>\work\hud_art_gen\v2] [--game "C:\GOG Games\Total Overdose"]
                                              [--only NAME ...] [--sheets]

Developer step (Pillow + numpy). The data builder (data/gameplay/hud_textures.py, stdlib only) puts the payloads it
writes into overdose.main when the data option `hud_textures = on`; the player's machine never needs Pillow.

Per texture (list: hud_textures/textures.json; `enabled` false drops a texture from the data pack):
  1. source PNG; `clean` = the same clean-up as work/hud_art_gen/v2/sheets.py clean() for art that has no *_clean.png;
     alpha 248 and above becomes 255 (the generated art's solid parts are 249-253, i.e. 2 % see-through)
  2. placement: the art's content box (alpha > 16) is fitted (contain, centred) into the ORIGINAL texture's content box
     (alpha > 16, read from the game's own overdose.main, read only) scaled to the new size, so every icon keeps its
     on-screen size and place. The combo strip is resampled from polar art instead (see combo_strip()).
  3. level 0 by a premultiplied LANCZOS resize; mips by premultiplied 2x2 box averages down to 1x1; per level the
     texels whose alpha rounds to 0 get a push-pull fill colour (no dark fringes under bilinear filtering). The combo
     strip keeps colour 0 where alpha is 0, like the original (it is drawn additively).
  4. A8R8G8B8 payload (B, G, R, A per texel, levels largest first), zlib-compressed into art/<name>.bin.z, listed with
     its size, levels and sha256 in art/manifest.json.
Also writes hud_textures/proof/offline_*.png (each new texture next to the original, and the combo checks) with --sheets.
"""
import argparse
import hashlib
import json
import math
import os
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'data', 'lib'))
sys.dont_write_bytecode = True
import todblock as tb      # noqa: E402
import todtexture as tt    # noqa: E402

ART_OUT = os.path.join(HERE, 'art')
PROOF = os.path.join(HERE, 'proof')
LIST = os.path.join(HERE, 'textures.json')
MAIN = 'data_pc/overdose_the_game/overdose/shared/overdose.main'
BOX_ALPHA = 16
OPAQUE_FROM = 248.0
FORMAT_VERSION = 1

# The combo timer strip (64 x 256 original): columns = radius, rows = angle. The approved art is the upper
# semicircle in the layout of work/hud_art_gen/ref/combo_disc_ref.png, which was made from the original strip with
# (fitted to 0.13 grey levels mean error over the whole image):
#   disc centre C, radius R; r = |p - C| / R; phi = angle from 12 o'clock, clockwise, -90 (9 o'clock) .. +90 (3 o'clock)
#   u = 1 - r            (u = 0 at the rim, 1 at the centre)
#   v = 0.5 + 0.5 * t,   t = (phi + 90) / 180   (rows 128..255 of the strip; the upper half stays transparent)
# The new strip inverts exactly that mapping, so it carries the same layout as the original strip and the game's own
# disc model and UV animation show it the way they showed the original.
COMBO_RIM_ALPHA_U = 9.19 / 64     # where the original strip's radial alpha ramp crosses 128 (column 8.69, texel centres)


# ------------------------------------------------------------------ helpers
def clean(path, pad_square=True, holes=False):
    """Same as work/hud_art_gen/v2/sheets.py clean() (holes=False path): drop alpha < 24, erode 2 px, feather 1 px,
    crop to content, pad square with a 2 % margin."""
    im = Image.open(path).convert('RGBA')
    a = im.split()[3]
    a = a.point(lambda v: 0 if v < 24 else v)
    a = a.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.GaussianBlur(1))
    im.putalpha(Image.composite(a, Image.new('L', a.size, 0), im.split()[3]))
    bb = im.getbbox()
    if bb:
        im = im.crop(bb)
    if pad_square:
        s = max(im.size)
        m = int(s * 0.02)
        out = Image.new('RGBA', (s + 2 * m, s + 2 * m), (0, 0, 0, 0))
        out.alpha_composite(im, ((s - im.width) // 2 + m, (s - im.height) // 2 + m))
        im = out
    return im


def full_opacity(im):
    """The generated art's solid parts carry alpha 249-253, not 255 (its background cut): stretch alpha so that
    OPAQUE_FROM and above is fully opaque (the soft edges scale by the same 3 %)."""
    a = np.asarray(im.getchannel('A'), dtype=np.float64)
    a = np.clip(np.floor(a * 255.0 / OPAQUE_FROM + 0.5), 0, 255).astype(np.uint8)
    out = im.copy()
    out.putalpha(Image.fromarray(a, 'L'))
    return out


def load_source(art_root, e):
    p = os.path.join(art_root, e['source'])
    if not os.path.exists(p):
        raise SystemExit('missing source for %s: %s' % (e['name'], p))
    if e.get('clean') == 'square':
        im = clean(p, pad_square=True)
    elif e.get('clean') == 'crop':
        im = clean(p, pad_square=False)
    elif e.get('clean') == 'additive':
        return additive_rgba(Image.open(p)), p
    else:
        im = Image.open(p).convert('RGBA')
    return full_opacity(im), p


def alpha_box(a, thr=BOX_ALPHA):
    ys, xs = np.nonzero(a > thr)
    if not len(xs):
        raise ValueError('empty alpha')
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def premul(im):
    f = np.asarray(im.convert('RGBA'), dtype=np.float64)
    f[..., :3] *= f[..., 3:4] / 255.0
    return f


def resize_premul(p, w, h):
    """Premultiplied float (H, W, 4) -> LANCZOS to (h, w), clamped (alpha 0..255, colour 0..alpha)."""
    out = np.empty((h, w, 4))
    for c in range(4):
        out[..., c] = np.asarray(Image.fromarray(p[..., c].astype(np.float32), 'F').resize((w, h), Image.LANCZOS))
    out[..., 3] = np.clip(out[..., 3], 0, 255)
    out[..., :3] = np.clip(out[..., :3], 0, out[..., 3:4])
    return out


def place(art, dst_w, dst_h, box):
    """Fit the art's content box (alpha > 16) into box (x0, y0, x1, y1, target pixels), contain + centred; the art's
    soft edge outside its box comes along at the same scale. -> premultiplied float (dst_h, dst_w, 4)."""
    src = premul(art)
    ax0, ay0, ax1, ay1 = alpha_box(src[..., 3])
    aw, ah = ax1 - ax0, ay1 - ay0
    bx0, by0, bx1, by1 = box
    s = min((bx1 - bx0) / aw, (by1 - by0) / ah)
    cx, cy = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
    # map art pixel x -> dst x = cx + (x - (ax0 + aw/2)) * s ; render the whole art (with its margin) then crop
    W, H = art.size
    fw, fh = max(1, int(round(W * s))), max(1, int(round(H * s)))
    s_x, s_y = fw / W, fh / H
    ox = cx - (ax0 + aw / 2.0) * s_x
    oy = cy - (ay0 + ah / 2.0) * s_y
    ix, iy = int(round(ox)), int(round(oy))
    scaled = resize_premul(src, fw, fh)
    out = np.zeros((dst_h, dst_w, 4))
    x0, y0 = max(0, ix), max(0, iy)
    x1, y1 = min(dst_w, ix + fw), min(dst_h, iy + fh)
    out[y0:y1, x0:x1] = scaled[y0 - iy:y1 - iy, x0 - ix:x1 - ix]
    return out, dict(scale=s, box=[float(v) for v in box], placed=[ix, iy, fw, fh])


def bilinear(p, xs, ys):
    """Sample premultiplied float image p (H, W, 4) at float pixel coordinates (texel centres at +0.5); 0 outside."""
    H, W = p.shape[:2]
    x = xs - 0.5
    y = ys - 0.5
    x0 = np.floor(x).astype(int)
    y0 = np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    pad = np.zeros((H + 2, W + 2, 4))
    pad[1:-1, 1:-1] = p
    x0c, y0c = np.clip(x0 + 1, 0, W + 1), np.clip(y0 + 1, 0, H + 1)
    x1c, y1c = np.clip(x0 + 2, 0, W + 1), np.clip(y0 + 2, 0, H + 1)
    return (pad[y0c, x0c] * (1 - fx) * (1 - fy) + pad[y0c, x1c] * fx * (1 - fy) +
            pad[y1c, x0c] * (1 - fx) * fy + pad[y1c, x1c] * fx * fy)


def combo_geometry(art):
    """Disc centre and radius (art pixels) of the approved semicircle art, fitted to its alpha-128 outline."""
    a = np.asarray(art)[..., 3].astype(float)
    H, W = a.shape
    pts = []
    for y in range(0, H, 4):
        xs = np.nonzero(a[y] > 128)[0]
        if len(xs):
            pts += [(xs[0], y), (xs[-1] + 1, y)]
    for x in range(0, W, 4):
        ys = np.nonzero(a[:, x] > 128)[0]
        if len(ys):
            pts.append((x, ys[0]))
    P = np.array(pts, float)
    A = np.c_[2 * P[:, 0], 2 * P[:, 1], np.ones(len(P))]
    cx, cy, c = np.linalg.lstsq(A, (P ** 2).sum(1), rcond=None)[0]
    r128 = math.sqrt(c + cx * cx + cy * cy)
    res = np.hypot(P[:, 0] - cx, P[:, 1] - cy) - r128
    return cx, cy, r128, float(np.abs(res).max())


def combo_strip(art, w, h, ss=4):
    """Polar art (ref layout) -> the sweep strip (h rows = angle, w columns = radius), premultiplied float."""
    cx, cy, r128, worst = combo_geometry(art)
    R = r128 / (1.0 - COMBO_RIM_ALPHA_U)       # the art's alpha-128 rim lands where the original's did
    src = premul(art)
    out = np.zeros((h, w, 4))
    half = h // 2
    j, i = np.mgrid[half:h, 0:w]
    acc = np.zeros((h - half, w, 4))
    for sy in range(ss):
        for sx in range(ss):
            u = (i + (sx + 0.5) / ss) / w
            v = (j + (sy + 0.5) / ss) / h
            r = 1.0 - u
            t = (v - 0.5) / 0.5
            phi = np.radians(t * 180.0 - 90.0)
            X = cx + R * r * np.sin(phi)
            Y = cy - R * r * np.cos(phi)
            acc += bilinear(src, X, Y)
    out[half:] = acc / (ss * ss)
    out[..., 3] = np.clip(out[..., 3], 0, 255)
    out[..., :3] = np.clip(out[..., :3], 0, out[..., 3:4])
    return out, dict(centre=[round(cx, 2), round(cy, 2)], radius_alpha128=round(r128, 2), radius_full=round(R, 2),
                     circle_fit_worst_px=round(worst, 2))


def additive_rgba(img):
    """Art drawn as glow on black (the combo badge is blended additively): alpha = brightest channel, colour
    un-premultiplied, so black is fully transparent and the glow keeps its colour."""
    rgb = np.asarray(img.convert('RGB'), dtype=np.float64)
    a = rgb.max(axis=2)
    with np.errstate(divide='ignore', invalid='ignore'):
        col = np.where(a[..., None] > 0, rgb * 255.0 / a[..., None], 0.0)
    out = np.dstack([np.clip(col, 0, 255), a]).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')


def combo_strip_disc(art, w, h, ss=4):
    """A full front-view disc picture (the badge exactly as the game should show it, centred, filling the square)
    -> the sweep strip in the disc model's own UV layout (see disc_from_strip 'game'): u 0.072 at the rim .. 0.928 at
    the centre, v 0.986 at 12 o'clock decreasing clockwise to 0.506 after a full turn. Rows above v 0.5 stay
    transparent, as in the original strip (the timer animation scrolls into them)."""
    S = min(art.size)
    art = art.crop(((art.width - S) // 2, (art.height - S) // 2, (art.width + S) // 2, (art.height + S) // 2))
    src = premul(art)
    cx = cy = S / 2.0
    R = S / 2.0
    half = h // 2
    j, i = np.mgrid[half:h, 0:w]
    acc = np.zeros((h - half, w, 4))
    for sy in range(ss):
        for sx in range(ss):
            u = (i + (sx + 0.5) / ss) / w
            v = (j + (sy + 0.5) / ss) / h
            r = np.clip(1.0 - (u - 0.072) / (0.928 - 0.072), 0.0, 1.0)
            phi = np.radians((0.986 - v) / 0.48 * 360.0)
            X = cx + R * r * np.sin(phi)
            Y = cy - R * r * np.cos(phi)
            smp = bilinear(src, X, Y)
            smp[u < 0.072 - 1e-9] *= np.clip((u[u < 0.072 - 1e-9] / 0.072)[..., None], 0, 1)   # fade past the rim
            acc += smp
    out = np.zeros((h, w, 4))
    out[half:] = acc / (ss * ss)
    out[..., 3] = np.clip(out[..., 3], 0, 255)
    out[..., :3] = np.clip(out[..., :3], 0, out[..., 3:4])
    return out, dict(mapping='disc', size=S)


def box_down(p):
    H, W = p.shape[:2]
    h, w = max(1, H // 2), max(1, W // 2)
    if H == 1:
        return (p[:, 0::2] + p[:, 1::2]) / 2.0
    if W == 1:
        return (p[0::2] + p[1::2]) / 2.0
    return (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2]).reshape(h, w, 4) / 4.0


def to_levels(level0, fill=True):
    """Premultiplied float level 0 -> list of uint8 straight-alpha RGBA arrays, full chain."""
    pyr = [level0]
    while pyr[-1].shape[0] > 1 or pyr[-1].shape[1] > 1:
        pyr.append(box_down(pyr[-1]))
    cols, alphas = [], []
    for p in pyr:
        a = p[..., 3]
        a8 = np.floor(a + 0.5).clip(0, 255).astype(np.uint8)
        with np.errstate(divide='ignore', invalid='ignore'):
            c = np.where(a[..., None] > 0, p[..., :3] * 255.0 / a[..., None], 0.0)
        c = np.where(a8[..., None] > 0, c, 0.0)
        cols.append(np.floor(c + 0.5).clip(0, 255))
        alphas.append(a8)
    if fill:
        filled = [None] * len(cols)
        filled[-1] = cols[-1]
        for L in range(len(cols) - 2, -1, -1):
            h, w = cols[L].shape[:2]
            coarse = np.asarray(Image.fromarray(filled[L + 1].astype(np.uint8), 'RGB').resize((w, h), Image.BILINEAR),
                                dtype=np.float64)
            filled[L] = np.where(alphas[L][..., None] > 0, cols[L], coarse)
        cols = filled
    return [np.dstack([c.astype(np.uint8), a]) for c, a in zip(cols, alphas)]


def payload_of(levels):
    out = bytearray()
    for lv in levels:
        out += lv[..., [2, 1, 0, 3]].tobytes()          # B, G, R, A
    return bytes(out)


def originals(game):
    arch = tb.open_naz(game)
    blk = tb.Block(arch.read(MAIN), '.main')
    return blk, tt.textures(blk)


def original_rgba(blk, rec):
    w, h, rgba = tt.decode_rgba_level0(rec, blk.payloads[rec['index']])
    return np.frombuffer(rgba, np.uint8).reshape(h, w, 4)


# ------------------------------------------------------------------ build
def build_one(e, art_root, blk, recs):
    rec = recs.get(e['resource'].lower())
    if rec is None:
        raise SystemExit('%s: %s not in overdose.main' % (e['name'], e['resource']))
    W, H = e['size']
    if (rec['w'], rec['h']) != tuple(e['original']):
        raise SystemExit('%s: original is %dx%d, the list says %s' % (e['name'], rec['w'], rec['h'], e['original']))
    art, src_path = load_source(art_root, e)
    orig = original_rgba(blk, rec)
    if e['group'] == 'combo':
        level0, info = (combo_strip_disc if e.get('mapping') == 'disc' else combo_strip)(art, W, H)
        levels = to_levels(level0, fill=False)
    else:
        k = W // rec['w']
        ob = alpha_box(orig[..., 3])
        box = [ob[0] * k, ob[1] * k, ob[2] * k, ob[3] * k]
        level0, info = place(art, W, H, box)
        info['original_box'] = [int(v) for v in ob]
        levels = to_levels(level0, fill=True)
    payload = payload_of(levels)
    n = tt.full_chain(W, H)
    assert len(levels) == n and len(payload) == tt.payload_size('RGBA32', W, H, n)
    tt.texture_patches(rec, 'RGBA32', W, H, n)        # the rules the data builder will apply
    if len(payload) > tt.TEXTURE_CAP:
        raise SystemExit('%s: %d bytes over the cap' % (e['name'], len(payload)))
    z = zlib.compress(payload, 9)
    fn = e['name'] + '.bin.z'
    with open(os.path.join(ART_OUT, fn), 'wb') as f:
        f.write(z)
    with open(src_path, 'rb') as f:
        src_sha = hashlib.sha256(f.read()).hexdigest()
    row = dict(name=e['name'], group=e['group'], resource=e['resource'], enabled=bool(e.get('enabled', True)),
               status=e.get('status', 'approved'), original=[rec['w'], rec['h']], fmt='RGBA32', w=W, h=H, levels=n,
               bytes=len(payload), file=fn, file_bytes=len(z), sha256=hashlib.sha256(payload).hexdigest(),
               source=e['source'].replace('\\', '/'), source_sha256=src_sha, clean=e.get('clean', 'none'),
               placement={k2: (round(v, 4) if isinstance(v, float) else v) for k2, v in info.items()})
    return row, levels, orig


def sheet_png(rows, path, title):
    """Original (nearest, enlarged) next to the new level 0, on dark and light backgrounds."""
    cell = 200
    font = ImageFont.truetype(r'C:\Windows\Fonts\arialbd.ttf', 14)
    cols = 4
    n = len(rows)
    W = cols * (2 * cell + 30) + 20
    Hh = 40 + ((n + cols - 1) // cols) * (cell + 40)
    s = Image.new('RGB', (W, Hh), (28, 28, 32))
    d = ImageDraw.Draw(s)
    d.text((10, 10), title, font=font, fill=(240, 200, 90))
    for k, (name, orig, new) in enumerate(rows):
        x = 10 + (k % cols) * (2 * cell + 30)
        y = 40 + (k // cols) * (cell + 40)
        d.text((x, y), name, font=font, fill=(220, 220, 220))
        for j, img in enumerate((orig, new)):
            im = Image.fromarray(img, 'RGBA')
            f = min(cell / im.width, cell / im.height)
            im = im.resize((max(1, int(im.width * f)), max(1, int(im.height * f))),
                           Image.NEAREST if j == 0 else Image.LANCZOS)
            bg = Image.new('RGBA', (cell, cell), (60, 60, 66, 255) if j == 0 else (60, 60, 66, 255))
            bg.alpha_composite(im, ((cell - im.width) // 2, (cell - im.height) // 2))
            s.paste(bg.convert('RGB'), (x + j * (cell + 6), y + 20))
    s.save(path)
    print('sheet', path)


def disc_from_strip(strip, size, mapping):
    """Re-wrap a strip (H, W, 4 uint8, straight alpha) to a disc image. mapping 'ref': the ref sheet's layout (upper
    semicircle, rows 0.5..1); 'game': the disc model's own UVs (u 0.072 rim .. 0.928 centre, v 0.986 at 12 o'clock
    decreasing clockwise to 0.506 after a full turn)."""
    H, W = strip.shape[:2]
    ys, xs = np.mgrid[0:size, 0:size]
    dx = (xs + 0.5) / size * 2 - 1
    dy = (ys + 0.5) / size * 2 - 1
    r = np.hypot(dx, dy)
    phi = np.degrees(np.arctan2(dx, -dy))
    if mapping == 'ref':
        u = 1 - r
        v = 0.5 + 0.5 * (phi + 90) / 180.0
        ok = (r <= 1) & (dy < 0)
    else:
        u = 0.072 + (1 - r) * (0.928 - 0.072)
        v = 0.986 - (phi % 360) / 360.0 * 0.48
        ok = r <= 1
    p = strip.astype(np.float64)
    p[..., :3] *= p[..., 3:4] / 255.0
    smp = bilinear(p, u * W, v * H)
    bg = np.array([24, 22, 22.0])
    a = smp[..., 3:4] / 255.0
    col = smp[..., :3] + bg * (1 - a)
    col[~ok] = bg
    return Image.fromarray(np.clip(col + 0.5, 0, 255).astype(np.uint8), 'RGB')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--art', default=os.path.join(HERE, '..', 'work', 'hud_art_gen', 'v2'))
    ap.add_argument('--game', default=r'C:\GOG Games\Total Overdose', help='read only: the original textures')
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--sheets', action='store_true', help='also write hud_textures/proof/offline_*.png')
    a = ap.parse_args(argv)
    entries = json.load(open(LIST, encoding='utf-8'))['textures']
    os.makedirs(ART_OUT, exist_ok=True)
    man_path = os.path.join(ART_OUT, 'manifest.json')
    old = {}
    if os.path.exists(man_path):
        old = {r['name']: r for r in json.load(open(man_path, encoding='utf-8'))['textures']}
    blk, recs = originals(a.game)
    rows, sheets = [], {}
    for e in entries:
        if a.only and e['name'] not in a.only:
            if e['name'] in old:
                r = dict(old[e['name']], enabled=bool(e.get('enabled', True)), status=e.get('status', 'approved'))
                rows.append(r)
            continue
        row, levels, orig = build_one(e, a.art, blk, recs)
        rows.append(row)
        sheets.setdefault(e['group'], []).append((e['name'], orig, levels[0]))
        print('%-28s %-7s %4dx%-4d %2d levels %9d B (zip %8d B)' % (row['name'], row['group'], row['w'], row['h'],
                                                                      row['levels'], row['bytes'], row['file_bytes']))
        if e['group'] == 'combo' and a.sheets:
            os.makedirs(PROOF, exist_ok=True)
            ref = Image.open(os.path.join(os.path.dirname(a.art.rstrip('\\/')), 'ref', 'combo_disc_ref.png')).convert('RGB')
            new_ref = disc_from_strip(levels[0], 768, 'ref')
            cmp_ = Image.new('RGB', (768 * 2 + 10, 400), (255, 255, 255))
            cmp_.paste(ref.crop((0, 0, 768, 400)), (0, 0))
            cmp_.paste(new_ref.crop((0, 0, 768, 400)), (778, 0))
            cmp_.save(os.path.join(PROOF, 'offline_combo_rewrap_vs_ref.png'))
            # the same check on the original strip: re-wrapping it must reproduce the ref
            o_ref = disc_from_strip(orig, 768, 'ref')
            err = np.abs(np.asarray(o_ref, float) - np.asarray(ref, float)).mean()
            print('  original strip re-wrapped vs ref: mean error %.2f grey levels' % err)
            g = Image.new('RGB', (512 * 2 + 10, 512), (255, 255, 255))
            g.paste(disc_from_strip(orig, 512, 'game'), (0, 0))
            g.paste(disc_from_strip(levels[0], 512, 'game'), (522, 0))
            g.save(os.path.join(PROOF, 'offline_combo_game_uv_full_timer.png'))
            Image.fromarray(levels[0], 'RGBA').save(os.path.join(PROOF, 'offline_combo_strip_128x512.png'))
    man = dict(format=FORMAT_VERSION, block=MAIN, note='made by hud_textures/make_hud_textures.py; payload = '
               'A8R8G8B8 mip chain (B, G, R, A), zlib', textures=rows)
    with open(man_path, 'w', encoding='utf-8') as f:
        json.dump(man, f, indent=1)
    tot = sum(r['bytes'] for r in rows if r['enabled'])
    print('manifest: %d textures (%d enabled), %.1f MB of payload in overdose.main, %.1f MB in art/' % (
        len(rows), sum(r['enabled'] for r in rows), tot / 1e6, sum(r['file_bytes'] for r in rows) / 1e6))
    if a.sheets:
        os.makedirs(PROOF, exist_ok=True)
        for g, rs in sheets.items():
            if g != 'combo':
                sheet_png(rs, os.path.join(PROOF, 'offline_%s.png' % g), '%s: original (left, nearest) / new level 0 (right)' % g)
    return 0


if __name__ == '__main__':
    sys.exit(main())
