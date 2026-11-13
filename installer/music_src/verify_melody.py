"""Check the rendered trumpet stem really plays the La Llorona tune.

For every trumpet note in compose.py, the decoded stem3_trumpet.ogg is analysed
in a window just after the attack and the strongest pitch (harmonic sum over the
spectrum, candidates E3..E5) is compared with the written note.
Prints the written tune per phrase in note names and the detected pitches.

Usage: python -I verify_melody.py
"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compose  # noqa: E402
import render   # noqa: E402

SR = render.SR


def detect(seg):
    """Harmonic-mean pitch pick with an octave check (odd harmonics of the octave below)."""
    win = np.hanning(len(seg))
    n_fft = 1 << 15
    spec = np.abs(np.fft.rfft(seg * win, n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1 / SR)

    def peak(f):
        band = (freqs > f * 2 ** (-0.3 / 12)) & (freqs < f * 2 ** (0.3 / 12))
        return spec[band].max() if band.any() else 0.0

    def hz(m):
        return 440 * 2 ** ((m - 69) / 12)

    scores = {m: np.mean([peak(h * hz(m)) for h in range(1, 7)]) for m in range(52, 77)}
    best = max(scores, key=scores.get)
    lower = hz(best - 12)
    odd = np.mean([peak(k * lower) for k in (1, 3, 5)])
    if best - 12 >= 47 and odd > 0.3 * scores[best]:
        best -= 12
    return best


def main():
    x = render.decode(os.path.join(render.OUT, 'stem3_trumpet.ogg')).mean(axis=1)
    notes = compose.trumpet_notes()
    ok = 0
    lines = [[] for _ in range(5)]
    for b, bt, nm, ln in notes:
        t = ((b - 1) * compose.BAR + bt) * compose.SEC_PER_BEAT
        dur = ln * compose.SEC_PER_BEAT
        a = int((t + 0.12) * SR)
        e = int((t + min(0.55, dur * 0.8)) * SR)
        got = compose.name_of(detect(x[a:e]))
        hit = got == nm
        ok += hit
        sec = min(4, (b - 8) // 8) if b < 41 else 4
        lines[sec].append(f'{nm}' + ('' if hit else f'[heard {got}]'))
    labels = ['couplet 1 pass 1 (pickup bar 8)', 'couplet 1 pass 2 (pickup bar 16)',
              'couplet 2 pass 1 (pickup bar 24)', 'couplet 2 pass 2 (pickup bar 32)',
              'outro lament (bars 43-48)']
    for i, ph in enumerate(lines):
        print(f'{labels[i] if i < len(labels) else i}: ' + ' '.join(ph))
    print(f'pitch check: {ok}/{len(notes)} notes detected at the written pitch')


if __name__ == '__main__':
    main()
