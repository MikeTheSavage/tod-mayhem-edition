"""bpm.py - tempo (BPM) and beat grid of the Suavemente mp3 (numpy only; ffmpeg decodes).

  python bpm.py [mp3] [--out ../work/bpm.json]
Onset envelope = half-wave rectified log-spectral flux (hop 512 @ 22050 Hz); tempo = autocorrelation peak in 80-200 BPM
refined by a comb-filter search; beat phase = the offset that maximises the onset energy on the grid. Reports the
global BPM, the first beat time and per-30-s-window BPM (to check the tempo is steady).
"""
import json, os, subprocess, sys
import numpy as np

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
MP3 = os.environ.get("TODSONG_MP3", "Suavemente.mp3")
SR, HOP, NFFT = 22050, 512, 2048


def decode(path):
    raw = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype="<f4").astype(np.float64)


def onset_env(x):
    n = 1 + (len(x) - NFFT) // HOP
    idx = np.arange(NFFT)[None, :] + HOP * np.arange(n)[:, None]
    win = np.hanning(NFFT)
    S = np.abs(np.fft.rfft(x[idx] * win, axis=1))
    L = np.log1p(100 * S)
    flux = np.maximum(0, np.diff(L, axis=0)).sum(axis=1)
    flux = np.concatenate([[0], flux])
    flux -= np.convolve(flux, np.ones(16) / 16, mode="same")       # remove slow trend
    return np.maximum(flux, 0)


def tempo(env, lo=80, hi=200):
    fps = SR / HOP
    env = env - env.mean()
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    best = None
    for bpm in np.arange(lo, hi, 0.05):
        lag = fps * 60 / bpm
        # comb: lags 1..4 beats
        s = sum(np.interp(lag * k, np.arange(len(ac)), ac) / k for k in (1, 2, 3, 4))
        if best is None or s > best[1]:
            best = (bpm, s)
    return best[0]


def phase(env, bpm):
    fps = SR / HOP
    per = fps * 60 / bpm
    t = np.arange(len(env))
    best = None
    for off in np.arange(0, per, 0.25):
        pos = np.arange(off, len(env) - 1, per)
        s = np.interp(pos, t, env).sum()
        if best is None or s > best[1]:
            best = (off, s)
    return best[0] / fps


def main():
    mp3 = MP3
    out = os.path.join(os.path.dirname(__file__), "..", "work", "bpm.json")
    a = sys.argv[1:]
    if a and not a[0].startswith("--"):
        mp3 = a[0]
    if "--out" in a:
        out = a[a.index("--out") + 1]
    x = decode(mp3)
    dur = len(x) / SR
    env = onset_env(x)
    bpm = tempo(env)
    fps = SR / HOP
    # refine: the BPM whose 10-s window beat phases agree best over the whole song (phase coherence)
    best = None
    for B in np.arange(bpm - 2, bpm + 2, 0.01):
        per = 60 / B
        offs = []
        for s in range(0, int(dur) - 15, 10):
            o = phase(env[int(s * fps): int((s + 10) * fps)], B)
            offs.append(((s + o) % per) / per)
        m = np.exp(2j * np.pi * np.array(offs)).mean()
        if best is None or abs(m) > best[1]:
            best = (B, abs(m), (np.angle(m) / (2 * np.pi)) % 1 * per)
    bpm = best[0]
    # flux frame t compares windows starting at t-1 and t: its time is about t*HOP + NFFT/2
    off = (best[2] + NFFT / 2 / SR) % (60 / bpm)
    print("coherence %.3f" % best[1])
    win = []
    for s in range(0, int(dur) - 30, 15):
        e = env[int(s * fps): int((s + 30) * fps)]
        win.append([s, round(float(tempo(e, bpm - 8, bpm + 8)), 2)])
    # downbeat guess: which of 4 beat phases has the most low-frequency energy (kick on 1)
    res = {"file": mp3, "duration_s": round(dur, 3), "bpm": round(float(bpm), 2), "beat_period_s": round(60 / bpm, 5),
           "first_beat_s": round(float(off), 4), "window_bpm": win}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(res, open(out, "w"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
