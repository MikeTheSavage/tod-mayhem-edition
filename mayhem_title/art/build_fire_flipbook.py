"""build_fire_flipbook.py - the looping fire layer of [mayhem_title] from fire_loop.mp4 (real flames on black, 5 s, made
by the author with Higgsfield). Build time: ffmpeg + Python (Pillow, numpy).

  * the flame band of the video (the tongues, without the white-hot base) is cropped and scaled to FW x FH per frame;
  * N frames at FPS cover LOOP seconds; the last XFADE seconds are cross-faded into the frames before the loop start,
    so frame N-1 runs into frame 0 without a seam;
  * frames are packed into an atlas (COLS x ROWS), saved as DXT1 (TODGameplay_mayhem_fire.dds, one level: no mips, they
    would bleed between frames). Additive in game: black = nothing.

  python build_fire_flipbook.py [--out DIR] [--sheet]
"""
import argparse, os, subprocess
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "fire_loop.mp4")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
N, FPS, T0 = 48, 12.0, 0.55          # 48 frames at 12 fps = a 4 s loop starting at 0.55 s
XFADE = 0.5
FW, FH = 512, 256
COLS, ROWS = 8, 8                    # 4096 x 2048 atlas (48 of 64 cells used)
CROP_Y0, CROP_Y1 = 0.02, 0.80        # the tongues: the white-hot base (bottom 20 %) is cut off


def frames_at(times):
    out = []
    for t in times:
        raw = subprocess.run([FFMPEG, "-loglevel", "error", "-ss", "%.4f" % t, "-i", SRC, "-frames:v", "1",
                              "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
        im = np.frombuffer(raw, np.uint8).reshape(1440, 2560, 3)
        y0, y1 = int(CROP_Y0 * 1440), int(CROP_Y1 * 1440)
        out.append(np.asarray(Image.fromarray(im[y0:y1]).resize((FW, FH), Image.LANCZOS), np.float32) / 255.0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "build"))
    ap.add_argument("--sheet", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    loop = N / FPS
    times = [T0 + i / FPS for i in range(N)]
    main_f = frames_at(times)
    nx = int(round(XFADE * FPS))
    pre = frames_at([t - loop for t in times[N - nx:]])          # what comes before the loop start
    for k in range(nx):
        i = N - nx + k
        w = (k + 1) / (nx + 1); w = w * w * (3 - 2 * w)
        main_f[i] = main_f[i] * (1 - w) + pre[k] * w
    # black stays black: lift the floor so the video's dark grain does not glow when added
    for i in range(N):
        f = np.clip((main_f[i] - 0.04) / 0.96, 0, 1)
        main_f[i] = f
    atlas = np.zeros((ROWS * FH, COLS * FW, 3), np.float32)
    for i, f in enumerate(main_f):
        r, c = divmod(i, COLS)
        atlas[r * FH:(r + 1) * FH, c * FW:(c + 1) * FW] = f
    img = Image.fromarray((atlas * 255 + 0.5).astype(np.uint8)).convert("RGBA")
    p = os.path.join(a.out, "TODGameplay_mayhem_fire.dds")
    img.save(p, pixel_format="DXT1")
    print(p, os.path.getsize(p), "bytes, %d frames %dx%d at %.0f fps (%.2f s loop), atlas %dx%d" % (N, FW, FH, FPS, loop, COLS * FW, ROWS * FH))
    if a.sheet:
        sh = Image.fromarray((atlas[: 2 * FH] * 255).astype(np.uint8)).resize((COLS * FW // 4, FH // 2))
        sh.save(os.path.join(a.out, "fire_sheet.png"))
        seam = np.concatenate([main_f[N - 2], main_f[N - 1], main_f[0], main_f[1]], axis=1)
        Image.fromarray((seam * 255).astype(np.uint8)).resize((FW * 2, FH // 2)).save(os.path.join(a.out, "fire_seam.png"))


if __name__ == "__main__":
    main()
