"""Render, loop, balance, normalise and encode the La Llorona stems.

Steps
  1. python compose.py writes midi/*.mid (3 identical passes of the 48-bar loop).
  2. FluidSynth renders each stem (48 kHz, float, same hall reverb for every stem).
  3. The middle pass is kept; the first 50 ms of it are crossfaded with the start of
     pass 3, so the last sample of the loop runs straight into the first one.
  4. Per-stem mix gains (MIX_GAIN) set the intended balance; one shared gain then
     puts the full mix peak at -1 dBFS. Stems are therefore meant to be summed at
     unity gain, from the same start sample.
  5. ffmpeg encodes Ogg Vorbis q5; the .ogg files are decoded again and measured.
  6. Previews (30 s WAV), a spectrogram PNG and a measurement report are written.

Usage:  python -I render.py [--reuse]  (--reuse: skip FluidSynth, use cached loops)
            (paths below; nothing is read from the downloads
                                   except the SoundFont and the FluidSynth binary)
"""
import json
import os
import struct
import subprocess
import sys
import zlib

import numpy as np
from scipy.signal import butter, lfilter, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compose  # noqa: E402  (our own module, same folder)

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))                   # the repo root
FLUIDSYNTH = os.path.join(ROOT, r'work\music\downloads\fluidsynth\fs'
                          r'\fluidsynth-v2.6.1-win10-x64-cpp11\bin\fluidsynth.exe')
SF2 = os.path.join(ROOT, r'work\music\downloads\generaluser_gs\GeneralUser-GS.sf2')
FFMPEG = os.environ.get('FFMPEG', 'ffmpeg')
WORK = os.path.join(ROOT, r'work\music\render')            # big intermediate WAVs (git-ignored)
OUT = os.path.join(ROOT, r'installer\web\music')
PROOF = os.path.join(ROOT, r'work\music_preview')   # previews (git-ignored)

SR = 48000
LOOP = int(round(compose.LOOP_BEATS * compose.SEC_PER_BEAT * SR))      # 5,184,000
XFADE = int(0.050 * SR)
TARGET_PEAK_DB = -1.15     # Vorbis adds ~0.15 dB of overshoot

# One reverb space for every stem (a big, dark hall / canyon); the per-channel
# send (CC91) in compose.py decides how far away each instrument sits.
FS_SETTINGS = {
    'synth.reverb.room-size': 0.78, 'synth.reverb.damp': 0.45,
    'synth.reverb.width': 1.0, 'synth.reverb.level': 0.85,
    'synth.chorus.active': 0, 'synth.polyphony': 512, 'synth.device-id': 16,
}
FS_GAIN = 0.4
# intended balance, applied before the shared normalisation (linear)
MIX_GAIN = {'stem1_guitar': 1.0, 'stem2_bass_perc': 0.8,
            'stem3_trumpet': 1.6, 'stem4_strings_choir': 0.7}
# rumble filter (2nd-order high-pass, Hz) per stem: keeps the low end acoustic
HIGH_PASS = {'stem1_guitar': 60, 'stem2_bass_perc': 48, 'stem3_trumpet': 120,
             'stem4_strings_choir': 50}

INSTRUMENTS = {
    'stem1_guitar': 'Nylon Guitar (GM 25 / bank 0 prog 24)',
    'stem2_bass_perc': 'Acoustic Bass as guitarron, doubled in octaves (bank 0 prog 32); '
                       'Timpani heartbeat and roll (bank 0 prog 47); Standard kit low/high '
                       'conga as cajon + maracas (ch 10); Orchestral kit concert bass drum '
                       '(bank 120 prog 48)',
    'stem3_trumpet': 'Trumpet (bank 0 prog 56), pitch-bend vibrato and falls, CC11 phrasing, '
                     'plus a filtered two-tap canyon echo',
    'stem4_strings_choir': 'Slow Strings (bank 0 prog 49), Tremolo Strings (bank 0 prog 44), '
                           'Voice Oohs (bank 0 prog 53)',
}


# --------------------------------------------------------------------------- wav io
def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    pos, fmt, raw = 12, None, None
    while pos < len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            raw = body
        pos += 8 + size + (size & 1)
    tag, chans, sr, _, _, bits = fmt
    assert sr == SR and chans == 2, fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(raw, dtype='<f4')
    elif tag == 3 and bits == 64:
        x = np.frombuffer(raw, dtype='<f8')
    elif tag in (1, 0xFFFE) and bits == 16:
        x = np.frombuffer(raw, dtype='<i2') / 32768.0
    else:
        raise ValueError(fmt)
    return x.reshape(-1, 2).astype(np.float64)


def write_wav16(path, x, seed=7):
    rng = np.random.default_rng(seed)
    tpdf = (rng.random(x.shape) - rng.random(x.shape)) / 32768.0
    q = np.clip(np.round((x + tpdf) * 32767.0), -32768, 32767).astype('<i2')
    data = q.tobytes()
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, SR, SR * 4, 4, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def write_wav_float(path, x):
    data = x.astype('<f4').tobytes()
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 3, 2, SR, SR * 8, 8, 32))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def decode(path):
    raw = subprocess.run([FFMPEG, '-v', 'error', '-i', path, '-f', 'f32le', '-ac', '2',
                          '-ar', str(SR), '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<f4').reshape(-1, 2).astype(np.float64)


# --------------------------------------------------------------------------- dsp
def one_pole_lp(x, fc):
    a = np.exp(-2 * np.pi * fc / SR)
    return lfilter([1 - a], [1, -a], x, axis=0)


def one_pole_hp(x, fc):
    return x - one_pole_lp(x, fc)


def canyon_echo(x):
    """Two filtered echo taps (1 beat and 2 beats later), opposite sides."""
    mono = x.mean(axis=1)
    y = one_pole_hp(one_pole_lp(one_pole_lp(mono, 2400), 2400), 380)
    beat = int(round(compose.SEC_PER_BEAT * SR))
    out = x.copy()
    for d, gl, gr in ((beat, 0.17, 0.07), (2 * beat, 0.05, 0.11)):
        out[d:, 0] += gl * y[:-d]
        out[d:, 1] += gr * y[:-d]
    return out


def extract_loop(x):
    """Middle pass, with its head crossfaded from the start of the next pass."""
    assert len(x) >= 2 * LOOP + XFADE, (len(x), 2 * LOOP + XFADE)
    loop = x[LOOP:2 * LOOP].copy()
    w = (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, XFADE)))[:, None]
    loop[:XFADE] = w * x[LOOP:LOOP + XFADE] + (1 - w) * x[2 * LOOP:2 * LOOP + XFADE]
    return loop


def db(v):
    return 20 * np.log10(max(v, 1e-12))


def rms(x):
    return float(np.sqrt(np.mean(x ** 2)))


# --------------------------------------------------------------------------- spectrogram png
FONT = {  # 3x5 bitmap glyphs
    '0': '111101101101111', '1': '010110010010111', '2': '111001111100111',
    '3': '111001111001111', '4': '101101111001001', '5': '111100111001111',
    '6': '111100111101111', '7': '111001001001001', '8': '111101111101111',
    '9': '111101111001111', 'k': '100101110101101', 'H': '101101111101101',
    'z': '000111010100111', 's': '000011010001110', ' ': '000000000000000',
    '.': '000000000000010', 'L': '100100100100111', 'O': '111101101101111',
    'P': '111101111100100', '|': '010010010010010',
}


def draw_text(img, x, y, text, col, scale=2):
    for ch in text:
        g = FONT.get(ch, FONT[' '])
        for i, bit in enumerate(g):
            if bit == '1':
                r, c = divmod(i, 3)
                img[y + r * scale:y + (r + 1) * scale, x + c * scale:x + (c + 1) * scale] = col
        x += 4 * scale


def colormap(v):
    anchors = np.array([[0, 0, 4], [40, 11, 84], [101, 21, 110], [159, 42, 99],
                        [212, 72, 66], [245, 125, 21], [250, 193, 39], [252, 255, 164]],
                       dtype=np.float64)
    pos = np.clip(v, 0, 1) * (len(anchors) - 1)
    i = np.minimum(pos.astype(int), len(anchors) - 2)
    f = (pos - i)[..., None]
    return (anchors[i] * (1 - f) + anchors[i + 1] * f).astype(np.uint8)


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[r].tobytes() for r in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def spectrogram_png(path, x, seam_at=None, width=1400, height=520):
    mono = x.mean(axis=1)
    n_fft, hop = 8192, max(256, len(mono) // width)
    win = np.hanning(n_fft)
    frames = []
    for s0 in range(0, len(mono) - n_fft, hop):
        frames.append(np.abs(np.fft.rfft(mono[s0:s0 + n_fft] * win)))
    S = np.array(frames).T                     # freq x time
    freqs = np.fft.rfftfreq(n_fft, 1 / SR)
    fmin, fmax = 30.0, 16000.0
    rows = np.geomspace(fmax, fmin, height)    # top = high
    idx = np.clip(np.searchsorted(freqs, rows), 1, len(freqs) - 1)
    Sdb = 20 * np.log10(S[idx] + 1e-9)
    Sdb -= Sdb.max()
    v = (Sdb + 90) / 90.0
    cols = np.linspace(0, Sdb.shape[1] - 1, width).astype(int)
    img_core = colormap(v[:, cols])
    pad_l, pad_b = 70, 30
    img = np.zeros((height + pad_b, width + pad_l, 3), np.uint8)
    img[:height, pad_l:] = img_core
    for f in (50, 100, 200, 500, 1000, 2000, 4000, 8000, 16000):
        r = int(np.argmin(np.abs(rows - f)))
        img[r, pad_l:pad_l + width:4] = (120, 200, 255)
        label = f'{f // 1000}k' if f >= 1000 else str(f)
        draw_text(img, 4, max(0, r - 5), label + 'Hz' if f < 1000 else label, (230, 230, 230))
    secs = len(mono) / SR
    for t in range(0, int(secs) + 1, 5):
        c = pad_l + int(t / secs * (width - 1))
        img[height:height + 6, c] = (230, 230, 230)
        draw_text(img, c + 2, height + 10, f'{t}s', (230, 230, 230))
    if seam_at is not None:
        c = pad_l + int(seam_at / secs * (width - 1))
        img[:height:2, c] = (80, 255, 120)
        draw_text(img, c + 4, 4, 'LOOP', (80, 255, 120))
    write_png(path, img)


# --------------------------------------------------------------------------- main
def render_stem(name):
    mid = os.path.join(HERE, 'midi', name + '.mid')
    wav = os.path.join(WORK, name + '_raw.wav')
    cfg = os.path.join(WORK, 'fluidsynth.cfg')
    with open(cfg, 'w') as f:
        f.write('interp 7\n')               # highest-quality sample interpolation
    cmd = [FLUIDSYNTH, '-ni', '-q', '-f', cfg, '-F', wav, '-T', 'wav', '-O', 'float',
           '-r', str(SR), '-g', str(FS_GAIN)]
    for k, v in FS_SETTINGS.items():
        cmd += ['-o', f'{k}={v}']
    cmd += [SF2, mid]
    subprocess.run(cmd, check=True)
    x = read_wav(wav)
    os.remove(wav)
    return x


def band_fractions(x):
    mono = x.mean(axis=1)
    spec = np.abs(np.fft.rfft(mono)) ** 2
    f = np.fft.rfftfreq(len(mono), 1 / SR)
    tot = spec.sum()
    bands = [(0, 60), (60, 100), (100, 4000), (4000, 24001)]
    return {f'{a}-{b}Hz': float(spec[(f >= a) & (f < b)].sum() / tot) for a, b in bands}


def seam_report(x):
    """Continuity across the wrap point (end -> start)."""
    joined = np.concatenate([x[-SR // 2:], x[:SR // 2]])
    d = np.abs(np.diff(joined, axis=0))
    seam_jump = float(np.abs(x[0] - x[-1]).max())
    typical = float(np.percentile(np.abs(np.diff(x, axis=0)), 99.9))
    maxdiff = float(np.abs(np.diff(x, axis=0)).max())
    win = int(0.010 * SR)
    env = [db(rms(joined[i:i + win])) for i in range(0, len(joined) - win + 1, win)]
    mid = len(env) // 2
    around = env[mid - 4:mid + 4]
    steps = np.abs(np.diff(env))
    # 2nd difference (click detector): largest |x[n+1]-2x[n]+x[n-1]| near the seam vs everywhere
    d2 = np.abs(np.diff(x, n=2, axis=0)).max(axis=1)
    j2 = np.abs(np.diff(joined, n=2, axis=0)).max(axis=1)
    c = len(joined) // 2
    return {
        'seam_sample_jump': seam_jump, 'p99.9_sample_step': typical, 'max_sample_step': maxdiff,
        'seam_d2': float(j2[c - 3:c + 2].max()), 'p99.99_d2': float(np.percentile(d2, 99.99)),
        'rms10ms_around_seam_dB': [round(e, 1) for e in around],
        'max_10ms_rms_step_at_seam_dB': float(steps[mid - 3:mid + 2].max()),
        'p99_10ms_rms_step_dB_overall': float(np.percentile(steps, 99)),
    }


def level_report(x):
    return {'peak_dBFS': round(db(np.abs(x).max()), 2), 'rms_dBFS': round(db(rms(x)), 2),
            'clipped_samples': int((np.abs(x) >= 0.99997).sum())}


def main():
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PROOF, exist_ok=True)
    names = [n for n, _ in compose.STEMS]
    stems = {}
    for name in names:
        cache = os.path.join(WORK, name + '_loop_cache.npy')
        if '--reuse' in sys.argv and os.path.exists(cache):
            loop = np.load(cache).astype(np.float64)
        else:
            print('render', name, flush=True)
            x = render_stem(name)
            if name == 'stem3_trumpet':
                x = canyon_echo(x)
            x = sosfilt(butter(2, HIGH_PASS[name], 'highpass', fs=SR, output='sos'), x, axis=0)
            loop = extract_loop(x)
            del x
            np.save(cache, loop.astype(np.float32))
        stems[name] = loop * MIX_GAIN[name]
    mix = sum(stems.values())
    g = 10 ** (TARGET_PEAK_DB / 20) / np.abs(mix).max()
    # leave a little room for Vorbis overshoot; checked again after decoding
    for k in stems:
        stems[k] *= g
    mix *= g
    report = {'loop_samples': LOOP, 'loop_seconds': LOOP / SR, 'shared_gain_dB': round(db(g), 2)}

    # encode
    sizes = {}
    for name in names:
        tmp = os.path.join(WORK, name + '_loop.wav')
        write_wav_float(tmp, stems[name])
        ogg = os.path.join(OUT, name + '.ogg')
        subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', tmp, '-c:a', 'libvorbis', '-q:a', '5',
                        '-metadata', 'title=La Llorona (traditional), arranged for Gameplay+',
                        '-metadata', f'comment={name}', ogg], check=True)
        sizes[name] = os.path.getsize(ogg)

    # measure the decoded oggs (what the installer actually plays)
    dec = {n: decode(os.path.join(OUT, n + '.ogg')) for n in names}
    lengths = {n: len(v) for n, v in dec.items()}
    report['decoded_lengths'] = lengths
    report['ogg_bytes'] = sizes
    combos = {'1': names[:1], '1-2': names[:2], '1-3': names[:3], 'full': names}
    report['levels_pcm'] = {n: level_report(stems[n]) for n in names}
    report['levels_pcm']['full'] = level_report(mix)
    report['levels_ogg'] = {}
    for c, ns in combos.items():
        m = sum(dec[n] for n in ns)
        report['levels_ogg'][c] = level_report(m)
        report['levels_ogg'][c]['seam'] = seam_report(m)
    report['seam_pcm_full'] = seam_report(mix)
    report['bands_full'] = band_fractions(mix)
    report['bands_per_stem'] = {n: band_fractions(stems[n]) for n in names}
    # loudness per 8-bar section (dB RMS) for each stem
    bar_s = compose.BAR * compose.SEC_PER_BEAT
    secs = {'intro 1-8': (1, 8), 'couplet1 9-24': (9, 24), 'couplet2a 25-32': (25, 32),
            'couplet2b 33-40': (33, 40), 'outro 41-48': (41, 48)}
    stems['full'] = mix
    report['section_rms_dB'] = {
        n: {s: round(db(rms(stems[n][int((a - 1) * bar_s * SR):int(b * bar_s * SR)])), 1)
            for s, (a, b) in secs.items()} for n in names + ['full']}

    # previews (30 s): 1, 1-2, 1-3 from bar 8 (trumpet pickup) ; full crosses the seam
    n30 = 30 * SR
    fade = int(0.3 * SR)
    ramp = np.linspace(0, 1, fade)[:, None]
    start = int(7 * bar_s * SR)
    for tag, ns in (('1', names[:1]), ('1-2', names[:2]), ('1-3', names[:3])):
        m = sum(dec[n] for n in ns)[start:start + n30].copy()
        m[:fade] *= ramp
        m[-fade:] *= ramp[::-1]
        write_wav16(os.path.join(PROOF, f'llorona_preview_{tag}.wav'), m)
    full = sum(dec[n] for n in names)
    pre = 15 * SR
    m = np.concatenate([full[-pre:], full[:n30 - pre]]).copy()
    m[:fade] *= ramp
    m[-fade:] *= ramp[::-1]
    write_wav16(os.path.join(PROOF, 'llorona_preview_full.wav'), m)
    spectrogram_png(os.path.join(PROOF, 'llorona_preview_full_spectrogram.png'), m, seam_at=15.0)
    spectrogram_png(os.path.join(PROOF, 'llorona_full_loop_spectrogram.png'), full)
    report['preview_full_seam_at_s'] = 15.0

    # melody check
    report['trumpet_melody'] = [f'bar {b} beat {bt + 1:g}: {nm} ({ln:g})'
                                for b, bt, nm, ln in compose.trumpet_notes()]

    # music.json
    meta = {
        'title': 'La Llorona (traditional), arranged for Gameplay+',
        'credit': 'Traditional Mexican song, public domain. Arrangement: Gameplay+.',
        'instruments_source': 'Rendered with the GeneralUser GS SoundFont (S. Christian '
                              'Collins); its licence allows free use of rendered audio and '
                              'does not require a credit.',
        'license': 'Public-domain melody; arrangement and recording are part of Gameplay+.',
        'bpm': compose.BPM, 'time_signature': '3/4', 'key': 'E minor',
        'bars': compose.BARS, 'sample_rate': SR,
        'loop_samples': LOOP, 'loop_seconds': LOOP / SR,
        'loopStart': 0.0, 'loopEnd': LOOP / SR,
        'mixing': 'Start every stem on the same sample and sum at unity gain; the full '
                  'mix peaks at about -1 dBFS. Each Mayhem toggle adds the next stem.',
        'stems': [{'order': i + 1, 'file': n + '.ogg', 'instrument': INSTRUMENTS[n]}
                  for i, n in enumerate(names)],
    }
    with open(os.path.join(OUT, 'music.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=2)
        f.write('\n')
    with open(os.path.join(HERE, 'last_render_report.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2)
    print(json.dumps(report, indent=1))
    for n in os.listdir(WORK):
        if n.endswith('_loop.wav'):
            os.remove(os.path.join(WORK, n))


if __name__ == '__main__':
    main()
