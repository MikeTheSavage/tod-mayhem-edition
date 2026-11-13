"""burn_preview.py - offline look of the [mayhem_title] burn-through (numpy port of the pixel shader in
dll/src/gp_mayhem_ps.hlsl; same constants, same formulas), composited over the title art the way the engine draws it.
Used to tune the look before the game runs. Nothing touches the game.

  python burn_preview.py NAME [--size 1920x1080] [--seconds 6] [--fps 30] [--stills 0.5,1.2,1.7,2.3,4]
        [--place bx,by,bw] [--ignite ax,ay] [--delay 0.6] [--reveal 2.0] [--sheet x0,y0,x1,y1]
Output: <repo>/work/mayhem/burn/NAME/ (stills, NAME.mp4, sheet)
"""
import argparse, os, shutil, struct, subprocess, sys
import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ART = os.path.join(ROOT, "mayhem_title", "art", "build")
BG = os.environ.get("TOD_LOADSCREEN_DDS", "start_loadscreen.dds")  # the game's start_loadscreen.dds, extracted
OUT = os.path.join(ROOT, "work", "mayhem", "burn")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import burn_model as bm  # noqa: E402


def load_dds_argb(p):
    d = open(p, "rb").read()
    h, w = struct.unpack_from("<II", d, 12)
    a = np.frombuffer(d, np.uint8, h * w * 4, 128).reshape(h, w, 4)[..., [2, 1, 0, 3]].astype(np.float32) / 255.0
    res = struct.unpack_from("<11f", d, 32)
    return a, res


def bilinear(img, u, v):
    """img HxWxC, u,v in [0,1] texture coords (texel centres), clamp"""
    H, W = img.shape[:2]
    x = np.clip(u * W - 0.5, 0, W - 1); y = np.clip(v * H - 0.5, 0, H - 1)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    x1 = np.minimum(x0 + 1, W - 1); y1 = np.minimum(y0 + 1, H - 1)
    fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    return (img[y0, x0] * (1 - fx) + img[y0, x1] * fx) * (1 - fy) + (img[y1, x0] * (1 - fx) + img[y1, x1] * fx) * fy


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--stills", default="0.5,1.0,1.4,1.8,2.3,3.0,5.5")
    ap.add_argument("--place", default=None)
    ap.add_argument("--ignite", default=None)
    ap.add_argument("--delay", type=float, default=bm.DELAY)
    ap.add_argument("--reveal", type=float, default=bm.REVEAL)
    ap.add_argument("--sheet", default="250,420,1250,1000")
    ap.add_argument("--video", action="store_true")
    a = ap.parse_args()
    out = os.path.join(OUT, a.name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    W, H = (int(v) for v in a.size.split("x"))
    bg = np.asarray(Image.open(BG).convert("RGB"), np.float32) / 255.0
    lt = np.load(os.path.join(ART, "letters_pm.npy"))          # premultiplied, half res
    fx, res = load_dds_argb(os.path.join(ART, "TODGameplay_mayhem_fx.dds"))
    fire = np.asarray(Image.open(os.path.join(ART, "TODGameplay_mayhem_fire.dds")).convert("RGB"), np.float32) / 255.0
    region = bm.Region(res)
    place = bm.Place(*([float(v) for v in a.place.split(",")] if a.place else bm.PLACE))
    ign = [float(v) for v in a.ignite.split(",")] if a.ignite else bm.IGNITE
    # the title picture: full width, 4:3 high, centred (0x45D940), UV (0,0)-(0.625,1)
    pw, ph = W, 0.75 * W; py0 = (H - ph) / 2
    def art_to_screen(ax, ay):
        return ax / 640.0 * pw, py0 + ay / 512.0 * ph
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32) + 0.5
    au = xs / pw * 640.0; av = (ys - py0) / ph * 512.0
    bgimg = bilinear(bg, au / 1024.0, av / 512.0)
    bgimg[(av < 0) | (av > 512)] = 0
    # E on screen
    ex0, ey0, ew, eh = region.e_rect(place)
    sx0, sy0 = art_to_screen(ex0, ey0); sx1, sy1 = art_to_screen(ex0 + ew, ey0 + eh)
    ix0, ix1, iy0, iy1 = max(0, int(sx0)), min(W, int(np.ceil(sx1))), max(0, int(sy0)), min(H, int(np.ceil(sy1)))
    sub_y, sub_x = ys[iy0:iy1, ix0:ix1], xs[iy0:iy1, ix0:ix1]
    u = (sub_x - sx0) / (sx1 - sx0); v = (sub_y - sy0) / (sy1 - sy0)
    m = bilinear(fx, u, v)
    consts = bm.Consts(region, place, ign, fx)
    stills = [float(s) for s in a.stills.split(",") if s]
    nfr = int(a.seconds * a.fps)
    vid = None
    if a.video:
        vid = subprocess.Popen([FFMPEG, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H),
                                "-r", str(a.fps), "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
                                os.path.join(out, a.name + ".mp4")], stdin=subprocess.PIPE)
    sheet_imgs = []
    times = sorted(set([round(i / a.fps, 4) for i in range(nfr)] if a.video else stills))
    for t in times:
        state = bm.state_at(t, a.delay, a.reveal)
        frame = bgimg.copy()
        if state is not None:
            col, alpha = bm.shade(u, v, m, lambda uu, vv: bilinear(lt, uu, vv), fire, consts, state)
            region_bg = frame[iy0:iy1, ix0:ix1]
            frame[iy0:iy1, ix0:ix1] = col + region_bg * (1 - alpha[..., None])
        img = (np.clip(frame, 0, 1) * 255 + 0.5).astype(np.uint8)
        if vid:
            vid.stdin.write(img.tobytes())
        if any(abs(t - s) < 0.5 / a.fps for s in stills):
            Image.fromarray(img).save(os.path.join(out, "%s_t%05.2f.png" % (a.name, t)))
            x0, y0, x1, y1 = (int(q) for q in a.sheet.split(","))
            im = Image.fromarray(img).crop((x0, y0, x1, y1)); sheet_imgs.append((t, im))
    if vid:
        vid.stdin.close(); vid.wait()
    if sheet_imgs:
        cw, ch = sheet_imgs[0][1].size; n = len(sheet_imgs); cols = min(4, n); rows = (n + cols - 1) // cols
        sh = Image.new("RGB", (cw * cols, ch * rows))
        for i, (t, im) in enumerate(sheet_imgs):
            sh.paste(im, ((i % cols) * cw, (i // cols) * ch))
        sh.save(os.path.join(out, "%s_sheet.png" % a.name))
    print("wrote", out)


if __name__ == "__main__":
    main()
