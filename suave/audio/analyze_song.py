"""analyze_song.py - structure analysis of the Suavemente mp3 to choose the Suave music segments (read only).

Decodes the mp3 with ffmpeg (mono 22050 Hz), lays the beat grid of suave/anim/work/bpm.json (124.17 BPM, first
beat 0.373 s, downbeats every 4 beats from beat 0), and prints one line per 2-bar block (8 beats): loudness (dB RMS),
low / mid / high band energy, spectral centroid, onset strength, and a repetition cluster id from chroma similarity
(blocks with the same id sound alike: the chorus repeats, the horn riff repeats). Writes analysis.json next to this
script. Used once to pick the segment start beats in segments.json; not needed to rebuild the oggs.

usage: python analyze_song.py [--mp3 PATH] [--ffmpeg PATH]
"""
import argparse
import json
import os
import subprocess

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_MP3 = os.environ.get('TODSONG_MP3', 'Suavemente.mp3')
DEF_FFMPEG = os.environ.get('FFMPEG', 'ffmpeg')
SR = 22050
BPM_JSON = os.path.join(HERE, '..', 'anim', 'work', 'bpm.json')


def decode(ffmpeg, path):
    raw = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR), '-f', 'f32le', '-'],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype='<f4').astype(np.float64)


def chroma_of(seg):
    n = len(seg)
    if n < 2048:
        return np.zeros(12)
    win = np.hanning(4096)
    acc = np.zeros(12)
    freqs = np.fft.rfftfreq(4096, 1.0 / SR)
    valid = (freqs > 60) & (freqs < 4000)
    pitch = np.zeros_like(freqs)
    pitch[valid] = np.round(12 * np.log2(freqs[valid] / 440.0)) % 12
    for s in range(0, n - 4096, 2048):
        mag = np.abs(np.fft.rfft(seg[s:s + 4096] * win))
        for p in range(12):
            acc[p] += mag[valid & (pitch == p)].sum()
    return acc / (np.linalg.norm(acc) + 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mp3', default=DEF_MP3)
    ap.add_argument('--ffmpeg', default=DEF_FFMPEG)
    a = ap.parse_args()
    bpm = json.load(open(BPM_JSON))
    period, first = bpm['beat_period_s'], bpm['first_beat_s']
    x = decode(a.ffmpeg, a.mp3)
    dur = len(x) / SR
    nbeats = int((dur - first) / period)
    blocks = []
    spec_win = np.hanning(2048)
    freqs = np.fft.rfftfreq(2048, 1.0 / SR)
    for b0 in range(0, nbeats - 8, 8):
        t0, t1 = first + b0 * period, first + (b0 + 8) * period
        seg = x[int(t0 * SR):int(t1 * SR)]
        rms = 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-9)
        bands = np.zeros(3)
        cen = []
        flux = []
        prev = None
        for s in range(0, len(seg) - 2048, 1024):
            mag = np.abs(np.fft.rfft(seg[s:s + 2048] * spec_win))
            bands += [mag[freqs < 200].sum(), mag[(freqs >= 200) & (freqs < 2000)].sum(), mag[freqs >= 2000].sum()]
            cen.append((mag * freqs).sum() / (mag.sum() + 1e-9))
            if prev is not None:
                flux.append(np.maximum(mag - prev, 0).sum())
            prev = mag
        bands = bands / (bands.sum() + 1e-9)
        blocks.append(dict(beat=b0, t=round(t0, 3), rms_db=round(float(rms), 1), low=round(float(bands[0]), 3),
                           mid=round(float(bands[1]), 3), high=round(float(bands[2]), 3),
                           centroid=round(float(np.mean(cen)), 0), flux=round(float(np.mean(flux)), 1),
                           chroma=chroma_of(seg)))
    # repetition clusters: greedy, cosine similarity of chroma > 0.97 joins an existing cluster
    reps = []
    for bl in blocks:
        cid = None
        for k, r in enumerate(reps):
            if float(np.dot(r, bl['chroma'])) > 0.97:
                cid = k
                break
        if cid is None:
            reps.append(bl['chroma'])
            cid = len(reps) - 1
        bl['cluster'] = cid
    counts = {}
    for bl in blocks:
        counts[bl['cluster']] = counts.get(bl['cluster'], 0) + 1
    print('duration %.2f s, %d beats, %d two-bar blocks, %d clusters' % (dur, nbeats, len(blocks), len(reps)))
    print('beat   time    dB   low   mid  high centroid  flux  cluster(n)')
    for bl in blocks:
        print('%4d %7.2f %5.1f %5.3f %5.3f %5.3f %7.0f %6.1f  %2d (%d)' % (
            bl['beat'], bl['t'], bl['rms_db'], bl['low'], bl['mid'], bl['high'], bl['centroid'], bl['flux'],
            bl['cluster'], counts[bl['cluster']]))
    out = [dict({k: v for k, v in bl.items() if k != 'chroma'}, cluster_size=counts[bl['cluster']]) for bl in blocks]
    with open(os.path.join(HERE, 'analysis.json'), 'w') as f:
        json.dump(dict(mp3=os.path.basename(a.mp3), duration_s=dur, beat_period_s=period, first_beat_s=first,
                       blocks=out), f, indent=1)


if __name__ == '__main__':
    main()
