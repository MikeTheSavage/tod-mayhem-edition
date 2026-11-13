"""burn_model.py - the [mayhem_title] burn-through as numpy, line for line the pixel shader dll/src/gp_mayhem_ps.hlsl
(the preview's ground truth for the look; the DLL computes the same constants in gp_mayhem_logic.cpp burn_*).

Spaces: art = title picture pixels (1024 x 512 texture, art in 640 x 512); E = the effect region (the lettering's bbox
plus margins, see build_mayhem_textures.py), uv 0..1 over E. Distances are measured in E heights.
"""
import math
import numpy as np

PLACE = (103.0, 238.0, 254.0)      # lettering bbox: left, top (art px), width (art px)  ~ 760 px wide at 1080p
IGNITE = (288.0, 350.0)            # art px: the right barrel's muzzle
DELAY, REVEAL = 0.6, 2.0           # s: vanilla title, then ignition -> full reveal
P_END = 1.0                        # front value when the reveal is done (T is scaled so the farthest hole pixel ~0.82)

# widths in E heights (scale-free); turned into T units with kT
RIM = 0.016        # the hot rim (burning edge)
CURL = 0.014       # curled-back paper just outside the rim
SCORCH = 0.050     # browned, darkened paper ahead of the curl
RAMP = 0.16        # letters: dark -> glowing -> final over this much front travel (T units)
NOISE_T = 0.055    # ragged front: noise amplitude in E heights


class Region:
    def __init__(self, res):
        self.aspect = res[0]
        self.bx0, self.by0, self.bx1, self.by1 = res[2], res[3], res[4], res[5]
        self.fire = (res[7], res[8], res[9])

    def e_rect(self, place):
        bx, by, bw = place.bx, place.by, place.bw
        ew = bw / (self.bx1 - self.bx0); eh = ew / self.aspect
        return bx - self.bx0 * ew, by - self.by0 * eh, ew, eh


class Place:
    def __init__(self, bx, by, bw):
        self.bx, self.by, self.bw = bx, by, bw


class Consts:
    def __init__(self, region, place, ign, fx):
        ex0, ey0, ew, eh = region.e_rect(place)
        self.top0, self.slope, self.fband = region.fire
        self.aspect = region.aspect
        self.iu, self.iv = (ign[0] - ex0) / ew, (ign[1] - ey0) / eh
        # the ignition point must be in the hole: else the nearest hole texel
        H, W = fx.shape[:2]
        hole = fx[..., 0] < 0.5
        iy, ix = min(H - 1, max(0, int(self.iv * H))), min(W - 1, max(0, int(self.iu * W)))
        if not hole[iy, ix]:
            yy, xx = np.nonzero(hole)
            k = np.argmin(((xx - ix) * self.aspect / W * H) ** 2 + (yy - iy) ** 2)
            self.iu, self.iv = (xx[k] + 0.5) / W, (yy[k] + 0.5) / H
        yy, xx = np.nonzero(hole)
        dmax = np.sqrt((((xx + 0.5) / W - self.iu) * self.aspect) ** 2 + ((yy + 0.5) / H - self.iv) ** 2).max()
        self.kT = 0.82 / dmax                           # T per E height
        self.band = fx_band = 40.0
        self.ke = self.kT * (2 * fx_band) / H           # hole field (R - 0.5) -> T units (BAND px of FH)
        self.rim, self.curl, self.scorch = RIM * self.kT, CURL * self.kT, SCORCH * self.kT
        self.noise = NOISE_T * self.kT
        rng = np.random.default_rng(5)
        g = rng.random((34, 34)).astype(np.float32)
        from PIL import Image
        n1 = np.asarray(Image.fromarray(g, "F").resize((272, 272), Image.BICUBIC))[8:264, 8:264]
        g2 = rng.random((66, 66)).astype(np.float32)
        n2 = np.asarray(Image.fromarray(g2, "F").resize((264, 264), Image.BICUBIC))[4:260, 4:260]
        self.noise_tex = np.dstack([n1, n2]).astype(np.float32)


def state_at(t, delay, reveal):
    """None before the delay; else (front p, settle time s since the reveal ended, time)"""
    if t < delay:
        return None
    x = (t - delay) / reveal
    p = P_END * (1 - (1 - min(x, 1.0)) ** 1.6) if x < 1 else P_END   # fast ignition, slowing as it spreads
    p = 0.02 + p * 1.0
    after = max(0.0, t - delay - reveal)
    return p, after, t


def sat(x):
    return np.clip(x, 0, 1)


def tex_wrap(n, u, v):
    H, W = n.shape[:2]
    x = (u * W) % W; y = (v * H) % H
    x0 = np.floor(x).astype(int) % W; y0 = np.floor(y).astype(int) % H
    x1 = (x0 + 1) % W; y1 = (y0 + 1) % H
    fx = (x - np.floor(x))[..., None]; fy = (y - np.floor(y))[..., None]
    return (n[y0, x0] * (1 - fx) + n[y0, x1] * fx) * (1 - fy) + (n[y1, x0] * (1 - fx) + n[y1, x1] * fx) * fy


def shade(u, v, m, letters, fire, c, state):
    """premultiplied colour (HxWx3) and alpha (HxW) of the overlay at E uv (u, v); m = fx texture sample (R G B A)"""
    p, after, t = state
    R, G, B, A = m[..., 0], m[..., 1], m[..., 2], m[..., 3]
    # burn time of this pixel: distance from the ignition (in E heights) + ragged noise
    du = (u - c.iu) * c.aspect; dv = v - c.iv
    T = np.sqrt(du * du + dv * dv) * c.kT + (A - 0.5) * c.noise * 2.0
    dfront = p - T                                  # > 0: the front has passed
    dedge = (0.5 - R) * c.ke                        # > 0: inside the final hole
    d = np.minimum(dfront, dedge)                   # distance behind the burning edge
    settle = sat((dfront - dedge) / (c.curl * 2.0)) # the edge reached the hole's border and rests there
    calm = sat(after / 1.2)                         # after the reveal: the rim cools, the curl lies down
    n = tex_wrap(c.noise_tex, u * 7.0, v * 7.0 * 0.63)[..., 0]
    # ---- unburnt side (d < 0): curled paper (pale back of the paper, browned toward the edge), then scorch
    w_curl = c.curl * (1.0 - 0.75 * np.maximum(settle, calm))
    x = -d
    in_curl = (x >= 0) & (x < w_curl)
    cu = sat(x / np.maximum(w_curl, 1e-5))         # 0 at the rim .. 1 at the fold
    paper = np.array([0.66, 0.46, 0.27]) * (0.55 + 0.45 * np.sin(cu * 3.1416))[..., None]
    paper = paper * (0.65 + 0.35 * cu)[..., None] + np.array([0.55, 0.22, 0.04]) * (1 - cu)[..., None]
    sc = sat(1.0 - (x - w_curl) / c.scorch) * (x >= w_curl)
    sc = sc * sc * (0.85 - 0.15 * n)
    scorch_col = np.array([0.10, 0.05, 0.02])
    # the fire's glow just outside the burning edge (additive, while it burns)
    gl = sat(1.0 - x / (c.curl * 4.0)) * (1.0 - np.maximum(settle, calm))
    gl = gl * gl * (0.8 + 0.2 * np.sin(t * 17.0 + n * 8.0))
    far = sat((0.985 - R) * 60.0)                   # beyond the field's reach: untouched
    out_col = np.where(in_curl[..., None], paper, scorch_col * sc[..., None]) + np.array([1.0, 0.45, 0.08]) * (gl * 1.6)[..., None]
    out_col = out_col * far[..., None]
    out_a = np.where(in_curl, 1.0, sc * 0.92) * far
    # ---- burnt side (d >= 0): hot rim, then char
    rimw = c.rim * (1.0 + 0.6 * settle)
    rim = sat(1.0 - d / rimw)
    flick = 0.75 + 0.25 * np.sin(t * 13.0 + u * 40.0 + n * 6.0)
    rim_on = (1.0 - 0.88 * np.maximum(settle, calm) * (0.6 + 0.4 * flick))
    hot = np.array([1.0, 0.38, 0.05]) * (1 - rim)[..., None] + np.array([1.0, 0.92, 0.55]) * rim[..., None]
    char = np.array([0.055, 0.038, 0.028]) * (0.75 + 0.5 * A)[..., None]
    age = np.maximum(dfront - rimw, 0.0)
    speck = sat((n - 0.72) * 6.0) * np.exp(-age * 6.0) * (1.0 - calm * 0.7)
    char = char + np.array([1.0, 0.35, 0.04]) * (speck * 0.8)[..., None]
    glow = rim * rim_on
    bloom = sat(1.0 - d / (c.curl * 2.5)) * (1.0 - np.maximum(settle, calm))      # the ring's glow on the char side
    in_col = char * (1 - glow)[..., None] + hot * glow[..., None] * 1.6 + np.array([1.0, 0.40, 0.06]) * (bloom * bloom * 0.6)[..., None]
    # letters inside the hole: dark -> glowing -> final, with heat shimmer on the flames
    k = sat((dfront - rimw) / RAMP)
    sh_u = (tex_wrap(c.noise_tex, u * 4.0, v * 2.5 + t * 0.55)[..., 0] - 0.5) * 0.010 * G
    sh_v = (tex_wrap(c.noise_tex, u * 4.0 + 0.37, v * 2.5 + t * 0.70)[..., 1] - 0.5) * 0.014 * G
    L = letters(u + sh_u, v + sh_v)
    la = L[..., 3] * sat(k * 3.0)
    heat = 4.0 * k * (1.0 - k)
    lc = L[..., :3] * (k * k)[..., None] + np.array([1.0, 0.42, 0.06]) * (heat * L[..., 3] * 0.9)[..., None]
    # fire flipbook over the flame tips (additive), the cracks flicker (additive)
    fl = fire_sample(fire, u, v, t, c) * (G * k * 0.55)[..., None]
    cf = 0.55 + 0.45 * np.sin(t * 9.0 + n * 9.0)
    crack = np.array([1.0, 0.45, 0.08]) * (B * cf * k * 0.6)[..., None]
    in_col = lc + in_col * (1 - la)[..., None] + fl + crack
    burnt = d >= 0
    col = np.where(burnt[..., None], in_col, out_col)
    alpha = np.where(burnt, 1.0, out_a)
    return col, alpha


TOP0, TOP1, BAND = 0.36, 0.10, 0.34   # the flames' base line (v at u = 0 / u = 1) and the flame band height (E heights)


def fire_sample(fire, u, v, t, c):
    """the looping flipbook (48 frames, 12 fps, 8x8 atlas), rising from the word's top line"""
    fr = t * 12.0
    f0 = int(fr) % 48; f1 = (f0 + 1) % 48; w = fr - int(fr)
    top = c.top0 + c.slope * u
    fy = 1.0 + (v - top) / c.fband                     # 1 at the base line, 0 one band above
    fx = (u * 1.05) % 1.0
    ok = (fy > 0) & (fy < 1)
    fyc = np.clip(fy, 0, 1)
    def cell(f):
        r, cc = divmod(f, 8)
        uu = (cc + fx * 0.996 + 0.002) / 8.0; vv = (r + fyc * 0.996 + 0.002) / 8.0
        H, W = fire.shape[:2]
        return fire[np.clip((vv * H).astype(int), 0, H - 1), np.clip((uu * W).astype(int), 0, W - 1)]
    s = cell(f0) * (1 - w) + cell(f1) * w
    return s * ok[..., None] * sat(fy * 4.0)[..., None]
