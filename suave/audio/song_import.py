r"""song_import.py - cut the Suave music segments from the PLAYER'S OWN copy of "Suavemente" (no ffmpeg).

The Suave move's music is four 17.4 s segments of a commercial song, so they are not shipped. The installer (or this
script) takes the player's own file, finds where the song sits in it, cuts the same four segments on this PC and
writes the same build folder `make_segments.py` writes (`suave_mus_<k>.ogg` + `manifest.json`).

  python song_import.py SONG_FILE [--out DIR] [--fingerprint FILE]

What ships instead of audio (`fingerprint.json`, made once from the reference mp3 by `make_fingerprint.py`):
  * the beat grid and the segment list (beat numbers, lengths, fades, loudness target `reference_rms_db`),
  * a coarse loudness envelope (3 ms hop, 10 ms window, 1 dB steps, an int8 per frame) of the first 60 s and of the
    windows around the two late segments. It holds no phase and no spectrum, so it cannot be turned back into audio.
Import steps:
  1. soundfile (libsndfile 1.2.x: mp3, wav, flac, ogg/vorbis/opus, aiff, w64, caf ...) reads the file; stereo is mixed
     to mono as ffmpeg does (0.7071 * (L + R)); other rates are resampled to 44,100 Hz (windowed-sinc polyphase,
     Kaiser beta 9, 36+ taps: no scipy).
  2. the player's envelope is cross-correlated with the shipped one: the offset of the song inside the player's file.
     A weak match (wrong song, heavy edit, other mix), anchors that disagree (cut / extended in the middle) or a file
     of the wrong length is refused with a plain message.  The reference mp3 itself lands on offset 0.
  3. the segments are cut exactly as make_segments.py does (beat grid, 0.25 s fade in, cos^2 0.45 s tail, RMS matched to
     `reference_rms_db`, 0.98 peak limit), 16-bit mono, and encoded as Ogg Vorbis with soundfile (libvorbis).
  4. every ogg is checked (data/lib/ogginfo.py: one stream, mono, 44.1 kHz, no bad CRC, granule == sample count) and
     the manifest is written in the shape `data/gameplay/suave_data.py` expects.

The level target is stored in the fingerprint (the game's own El Mariachi stream, measured once), so the player's game
files are not needed here.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FINGERPRINT = os.path.join(HERE, 'fingerprint.json')
RATE = 44100
HOP = 147                      # 3.333 ms
WIN = 441                      # 10 ms
FRAME_HZ = RATE / HOP          # 300
HP_FRAMES = 300                # high-pass: minus the 1 s moving average
FLOOR_DB = -70.0
# thresholds (measured on the reference mp3 and on wrong / edited files)
MIN_SCORE = 0.60               # normalized envelope correlation at the best lag
MIN_RATIO = 1.4                # best / the largest value outside +-0.5 s of it (the real match scores about 2.2)
MAX_ANCHOR_DRIFT_S = 0.060     # the late anchors must agree with the head within this
MAX_LEN_MISMATCH_S = 12.0      # file length (minus the offset) vs the reference length
SNAP_S = 0.002                 # |offset| below this is 0 (the reference file itself)


class SongImportError(Exception):
    """The message is for the player: it says what is wrong with the file."""


# ------------------------------------------------------------------------------------------------- reading
def _sf():
    try:
        import soundfile as sf
    except Exception as e:                                       # noqa: BLE001
        raise SongImportError('the audio reader (soundfile / libsndfile) is not available: %s' % e)
    return sf


def read_song(path, progress=None):
    """-> (mono float64 samples at 44,100 Hz, info dict). Raises SongImportError."""
    sf = _sf()
    if not path or not os.path.isfile(path):
        raise SongImportError('The song file was not found: %s' % path)
    try:
        f = sf.SoundFile(path)
    except Exception as e:                                       # noqa: BLE001
        ext = os.path.splitext(path)[1].lower()
        hint = ' (m4a / aac / wma files are not supported: convert it to mp3, flac or wav first)' if ext in (
            '.m4a', '.aac', '.wma', '.mp4', '.opus') and ext != '.opus' else ''
        raise SongImportError('This file cannot be read as audio%s: %s' % (hint, str(e).strip()))
    with f:
        sr, ch, frames = f.samplerate, f.channels, f.frames
        if sr < 8000 or ch < 1:
            raise SongImportError('Unusable audio format (%d Hz, %d channels).' % (sr, ch))
        parts = []
        done = 0
        for blk in f.blocks(blocksize=1 << 20, dtype='float32', always_2d=True):
            parts.append(_mono(blk))
            done += len(blk)
            if progress:
                progress(min(done, frames), max(frames, 1))
    x = np.concatenate(parts).astype(np.float64) if parts else np.zeros(0)
    info = dict(path=os.path.basename(path), rate=sr, channels=ch, frames=int(len(x)), format=f.format,
                subtype=f.subtype, seconds=len(x) / sr)
    if len(x) < sr * 5:
        raise SongImportError('The file is only %.1f s long: not the song.' % (len(x) / sr))
    if sr != RATE:
        x = resample(x, sr, RATE)
    return x, info


def _mono(blk):
    """frames x channels -> mono like ffmpeg's -ac 1 for stereo (0.7071 * (L + R)); more channels: front pair / mean."""
    ch = blk.shape[1]
    if ch == 1:
        return blk[:, 0]
    if ch == 2:
        return (blk[:, 0] + blk[:, 1]) * np.float32(math.sqrt(0.5))
    return blk[:, :min(ch, 3)].mean(axis=1) * np.float32(math.sqrt(2.0))


def resample(x, sr_in, sr_out, taps=32, beta=9.0):
    """Windowed-sinc polyphase resampler (rational ratio): Kaiser window, cutoff 0.46 * min(rates) (transition about
    4 % of the lower Nyquist), unity DC gain. Output length floor(len * out / in)."""
    g = math.gcd(sr_in, sr_out)
    L, M = sr_out // g, sr_in // g
    n_out = int(len(x) * L // M)
    fc = 0.46 * min(1.0, sr_out / sr_in)                          # cycles per input sample
    W = int(math.ceil(taps / 2 * max(1.0, sr_in / sr_out)))      # half width in input samples
    i = np.arange(-W + 1, W + 1)
    xp = np.concatenate([np.zeros(W), x, np.zeros(W + M + 2)])
    swv = np.lib.stride_tricks.sliding_window_view(xp, 2 * W)
    out = np.zeros(n_out)
    for r in range(L):
        base, frac = divmod(r * M, L)
        frac = frac / L
        t = frac - i                                              # h at (tau - k), k = base + i
        h = 2 * fc * np.sinc(2 * fc * t) * np.i0(beta * np.sqrt(np.clip(1 - (t / W) ** 2, 0, 1))) / np.i0(beta)
        # dot with x[q*M + base + i] for i = -W+1..W: xp index = that + W
        m_idx = np.arange(r, n_out, L)
        q = m_idx // L
        start = base + 1                                          # xp index of i = -W+1 is base - W + 1 + W
        rows = swv[start::M][:len(q)]
        if len(rows) < len(q):
            q = q[:len(rows)]
            m_idx = m_idx[:len(rows)]
        out[m_idx] = rows @ h
    return out


# ------------------------------------------------------------------------------------------------- fingerprint
def envelope_db(x):
    """Frame energy in dB: 10 ms rectangular windows every 3.33 ms (exact block sums), floor FLOOR_DB."""
    nb = len(x) // HOP
    if nb < 3:
        return np.zeros(0)
    s = (x[:nb * HOP] ** 2).reshape(nb, HOP).sum(axis=1)
    e = s[:-2] + s[1:-1] + s[2:]
    db = 10 * np.log10(np.maximum(e / WIN, 1e-12))
    return np.maximum(db, FLOOR_DB)


def quantize(db):
    return np.clip(np.round(db), -128, 127).astype(np.int8)


def feature(db):
    """Level envelope -> gain- and EQ-tolerant feature: minus its 1 s moving average, clipped to +-20 dB."""
    db = np.asarray(db, dtype=np.float64)
    k = HP_FRAMES
    pad = np.concatenate([np.full(k // 2, db[0]), db, np.full(k - k // 2 - 1, db[-1])])
    c = np.concatenate([[0.0], np.cumsum(pad)])
    ma = (c[k:] - c[:-k]) / k
    return np.clip(db - ma[:len(db)], -20.0, 20.0)


def _b64(a):
    return base64.b64encode(np.asarray(a, dtype=np.int8).tobytes()).decode('ascii')


def _unb64(s):
    return np.frombuffer(base64.b64decode(s), dtype=np.int8).astype(np.float64)


def load_fingerprint(path=None):
    path = path or FINGERPRINT
    try:
        with open(path, encoding='utf-8') as f:
            fp = json.load(f)
    except (OSError, ValueError) as e:
        raise SongImportError('The song fingerprint is missing or damaged (%s): %s' % (path, e))
    fp['head_db'] = _unb64(fp['head'])
    for a in fp['anchors']:
        a['db'] = _unb64(a['env'])
    return fp


def make_fingerprint(song44, cfg, grid, reference_rms_db, head_s=60.0, margin_s=1.5, anchors=(3, 4)):
    """song44: the reference at 44.1 kHz mono. cfg = segments.json, grid = bpm.json. -> the fingerprint dict (JSON)."""
    db = envelope_db(song44)
    period, first = grid['beat_period_s'], grid['first_beat_s']
    fp = dict(version=1, rate=RATE, hop=HOP, win=WIN, hp_frames=HP_FRAMES, floor_db=FLOOR_DB,
              note='Loudness envelope of a reference copy, 1 dB steps, 3.3 ms hop: no phase, no spectrum, not audio. '
                   'Used only to find where the song starts in the player\'s own file.',
              duration_s=round(len(song44) / RATE, 3), bpm=grid['bpm'], beat_period_s=period, first_beat_s=first,
              reference_rms_db=reference_rms_db,
              fade_in_s=cfg['fade_in_s'], fade_out_s=cfg['fade_out_s'], rate_out=cfg['rate'],
              segments=[dict(name=s['name'], label=s['label'], start_beat=s['start_beat'], beats=s['beats'])
                        for s in cfg['segments']],
              head=_b64(quantize(db[:int(head_s * FRAME_HZ)])), head_s=head_s, anchors=[])
    for k in anchors:
        s = cfg['segments'][k - 1]
        t0 = first + s['start_beat'] * period - margin_s
        t1 = first + (s['start_beat'] + s['beats']) * period + margin_s
        i0 = int(round(t0 * FRAME_HZ))
        i1 = int(round(t1 * FRAME_HZ))
        fp['anchors'].append(dict(segment=k, start_s=round(i0 / FRAME_HZ, 6), env=_b64(quantize(db[i0:i1]))))
    return fp


# ------------------------------------------------------------------------------------------------- matching
def _xcorr(a, b, lag_lo, lag_hi, min_overlap):
    """Normalized cross-correlation of b (reference, length nb) inside a (player, length na) for lags in
    [lag_lo, lag_hi]: b[i] pairs with a[i + lag]. Returns (lags, ncc) over the lags with enough overlap."""
    na, nb = len(a), len(b)
    lags = np.arange(lag_lo, lag_hi + 1)
    lo = np.maximum(0, -lags)                                    # first valid i
    hi = np.minimum(nb, na - lags)                               # one past the last valid i
    ok = (hi - lo) >= min_overlap
    lags, lo, hi = lags[ok], lo[ok], hi[ok]
    if not len(lags):
        return lags, np.zeros(0)
    size = 1 << int(math.ceil(math.log2(na + nb)))
    fa, fb = np.fft.rfft(a, size), np.fft.rfft(b, size)
    cc = np.fft.irfft(fa * np.conj(fb), size)                    # cc[l] = sum_i a[i + l] b[i]
    num = cc[np.mod(lags, size)]
    ca = np.concatenate([[0.0], np.cumsum(a * a)])
    cb = np.concatenate([[0.0], np.cumsum(b * b)])
    ea = ca[hi + lags] - ca[lo + lags]
    eb = cb[hi] - cb[lo]
    # the FFT sum runs over the overlap only (both signals are zero outside), so num is already restricted
    den = np.sqrt(np.maximum(ea * eb, 1e-9))
    return lags, num / den


def _peak(lags, ncc, guard):
    k = int(np.argmax(ncc))
    peak = float(ncc[k])
    far = np.abs(lags - lags[k]) > guard
    second = float(np.max(ncc[far])) if far.any() else 0.0
    # parabolic refinement
    frac = 0.0
    if 0 < k < len(ncc) - 1:
        y0, y1, y2 = ncc[k - 1], ncc[k], ncc[k + 1]
        d = y0 - 2 * y1 + y2
        if d < 0:
            frac = float(np.clip(0.5 * (y0 - y2) / d, -0.5, 0.5))
    return lags[k] + frac, peak, second


def locate(song, fp, log=None):
    """The offset (seconds) of the reference song inside `song` (player's samples: player time = ref time + offset).
    -> dict(offset_s, score, ratio, anchors=[...]). Raises SongImportError when this is not the song."""
    log = log or (lambda *a: None)
    ref_len = fp['duration_s']
    play_len = len(song) / RATE
    db = envelope_db(song)
    fh = feature(fp['head_db'])
    fa = feature(db[:int(150 * FRAME_HZ)]) if len(db) > fh.size else feature(db)
    guard = int(0.5 * FRAME_HZ)
    lags, ncc = _xcorr(fa, fh, int(-25 * FRAME_HZ), int(90 * FRAME_HZ), int(30 * FRAME_HZ))
    if not len(lags):
        raise SongImportError('The file is too short or too different to be "Suavemente".')
    lag, score, second = _peak(lags, ncc, guard)
    ratio = score / max(second, 1e-6)
    offset = lag / FRAME_HZ
    log('envelope match: score %.3f, next best %.3f (ratio %.1f), offset %+.4f s' % (score, second, ratio, offset))
    if score < MIN_SCORE or ratio < MIN_RATIO:
        raise SongImportError(
            'This does not look like the song: its first minute does not match the reference (match %.2f, needs %.2f). '
            'Use the studio version of "Suavemente" by Elvis Crespo (the 4:28 album / single version), not a live, '
            'remix or radio edit.' % (score, MIN_SCORE))
    # the late anchors: the same offset must hold (a cut or a longer intro in the middle would shift it)
    drifts = []
    for a in fp['anchors']:
        fan = feature(a['db'])
        win = int(1.0 * FRAME_HZ)
        lo = int(round(a['start_s'] * FRAME_HZ + lag)) - win          # the anchor's expected start, 1 s earlier
        need = len(fan) + 2 * win
        if lo < 0 or lo + need > len(db):
            raise SongImportError('The file ends too early: segment %d of the song is not in it (the file is %.0f s '
                                  'long, the song %.0f s). Use the full version.' % (a['segment'], play_len, ref_len))
        p0, p1 = max(0, lo - 300), min(len(db), lo + need + 300)
        seg = feature(db[p0:p1])[lo - p0:lo - p0 + need]
        l2, n2 = _xcorr(seg, fan, 0, 2 * win, int(len(fan) * 0.9))
        pl, ps, p2 = _peak(l2, n2, guard)
        d = (pl - win) / FRAME_HZ
        drifts.append(d)
        log('anchor segment %d: score %.3f, drift %+.4f s' % (a['segment'], ps, d))
        if ps < MIN_SCORE or abs(d) > MAX_ANCHOR_DRIFT_S:
            raise SongImportError(
                'The start of this file matches "Suavemente" but segment %d does not (match %.2f, off by %+.2f s): it '
                'is an edited, shortened or different version. Use the full 4:28 studio version.' % (
                    a['segment'], ps, d))
    if abs(play_len - offset - ref_len) > MAX_LEN_MISMATCH_S:
        raise SongImportError('The file is %.0f s long, the song is %.0f s: this is not the same version.' % (
            play_len, ref_len))
    if abs(offset) < SNAP_S:
        offset = 0.0
    return dict(offset_s=offset, score=score, ratio=ratio, anchor_drift_s=drifts)


# ------------------------------------------------------------------------------------------------- cutting
def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-12)


def cut_segment(song, cfg_seg, fp, offset_s):
    """-> (int16 samples, dict(t0, n, gain_db)): exactly make_segments.py's cut / fades / gain / limiter."""
    rate = fp['rate_out']
    period, first = fp['beat_period_s'], fp['first_beat_s']
    if cfg_seg['start_beat'] % 4:
        raise SongImportError('%s: start_beat is not a downbeat' % cfg_seg['name'])
    t0 = first + cfg_seg['start_beat'] * period
    n = int(round(cfg_seg['beats'] * period * rate))
    i0 = int(round((t0 + offset_s) * rate))
    if i0 < 0 or i0 + n > len(song):
        raise SongImportError('The file ends too early: %s needs %.1f s of the song.' % (cfg_seg['name'], t0 + n / rate))
    x = song[i0:i0 + n].copy()
    fi, fo = int(fp['fade_in_s'] * rate), int(fp['fade_out_s'] * rate)
    x[:fi] *= np.linspace(0.0, 1.0, fi, endpoint=False)
    x[n - fo:] *= np.cos(np.linspace(0.0, np.pi / 2, fo)) ** 2
    gain_db = fp['reference_rms_db'] - rms_db(x)
    x *= 10 ** (gain_db / 20)
    peak = np.max(np.abs(x))
    if peak > 0.98:
        x *= 0.98 / peak
        gain_db += 20 * np.log10(0.98 / peak)
    pcm = (np.clip(x, -1, 1) * 32767).round().astype('<i2')
    return pcm, dict(t0=t0, n=n, gain_db=gain_db)


def encode_ogg(pcm, path, rate=RATE, quality=0.3):
    """Mono int16 -> Ogg Vorbis (libvorbis VBR through libsndfile). Written in small blocks: one big write of the whole
    segment overflows libsndfile's stack on Windows."""
    sf = _sf()
    with sf.SoundFile(path, 'w', samplerate=rate, channels=1, format='OGG', subtype='VORBIS',
                      compression_level=1.0 - quality) as f:
        for i in range(0, len(pcm), 4096):
            f.write(pcm[i:i + 4096])


def _ogginfo():
    try:
        import ogginfo                                         # the installer has put data/lib on sys.path
    except ImportError:
        p = os.path.normpath(os.path.join(HERE, '..', '..', 'data', 'lib'))
        if p not in sys.path:
            sys.path.insert(0, p)
        import ogginfo
    return ogginfo


def import_song(path, out_dir, fingerprint=None, log=None, progress=None):
    """Read the player's file, find the song, cut + encode the four segments into out_dir, write manifest.json.
    Returns the manifest dict. Raises SongImportError."""
    log = log or (lambda *a: None)
    fp = fingerprint if isinstance(fingerprint, dict) else load_fingerprint(fingerprint)
    song, info = read_song(path, progress)
    log('read %s: %s %s, %d Hz, %d ch, %.1f s%s' % (info['path'], info['format'], info['subtype'], info['rate'],
                                                   info['channels'], info['seconds'],
                                                   '' if info['rate'] == RATE else ' (resampled to 44100 Hz)'))
    loc = locate(song, fp, log)
    log('the song starts %+.3f s into your file' % loc['offset_s'])
    os.makedirs(out_dir, exist_ok=True)
    oi_mod = _ogginfo()
    rate = fp['rate_out']
    with open(path, 'rb') as f:
        h = hashlib.sha256()
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    segs = []
    for k, s in enumerate(fp['segments'], 1):
        pcm, m = cut_segment(song, s, fp, loc['offset_s'])
        ogg_path = os.path.join(out_dir, s['name'] + '.ogg')
        encode_ogg(pcm, ogg_path, rate)
        with open(ogg_path, 'rb') as f:
            ogg = f.read()
        oi = oi_mod.info(ogg)
        n = m['n']
        if not (oi['channels'] == 1 and oi['rate'] == rate and oi['bad_crc_pages'] == 0 and oi['streams'] == 1
                and oi['samples'] == n):
            raise SongImportError('internal error: %s did not encode cleanly (ch %d, rate %d, samples %d of %d, bad '
                                  'pages %d)' % (s['name'], oi['channels'], oi['rate'], oi['samples'], n,
                                                 oi['bad_crc_pages']))
        segs.append(dict(name=s['name'], label=s['label'], start_beat=s['start_beat'], beats=s['beats'],
                         song_start_s=round(m['t0'], 4), song_end_s=round(m['t0'] + n / rate, 4),
                         seconds=round(n / rate, 4), samples=n, rate=rate, channels=1,
                         gain_db=round(m['gain_db'], 2), ogg_bytes=len(ogg), sha256=hashlib.sha256(ogg).hexdigest(),
                         vendor=oi['vendor'], archive_entry='data/sounds/streamedpowermoves/%s.ogg' % s['name'],
                         slot_path='/Sounds/StreamedPowerMoves/%s.wav' % s['name'].capitalize().replace('_mus_',
                                                                                                         '_Mus_')))
        log('%-12s song %7.3f..%7.3f s  %7d samples  gain %+5.1f dB  %6d B' % (
            s['name'], m['t0'], m['t0'] + n / rate, n, m['gain_db'], len(ogg)))
    man = dict(tool='suave/audio/song_import.py', source=dict(name=os.path.basename(path), sha256=h.hexdigest(),
                                                                format=info['format'], rate=info['rate'],
                                                                channels=info['channels']),
               offset_s=round(loc['offset_s'], 5), match_score=round(loc['score'], 3),
               anchor_drift_s=[round(d, 4) for d in loc['anchor_drift_s']],
               mp3='(the player\'s file; not stored)', mp3_sha256=h.hexdigest(), beat_period_s=fp['beat_period_s'],
               first_beat_s=fp['first_beat_s'], bpm=fp['bpm'], reference='data/sounds/streamedpowermoves/mariachi_02.ogg',
               reference_rms_db=fp['reference_rms_db'], reference_sha256=None, segments=segs)
    with open(os.path.join(out_dir, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(man, f, indent=1)
    return man


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('song')
    ap.add_argument('--out', default=os.path.join(HERE, 'build'))
    ap.add_argument('--fingerprint', default=FINGERPRINT)
    a = ap.parse_args(argv)
    try:
        man = import_song(a.song, a.out, a.fingerprint, log=print)
    except SongImportError as e:
        print('REFUSED: %s' % e)
        return 2
    print('manifest: %s' % os.path.join(a.out, 'manifest.json'), '(offset %+.3f s)' % man['offset_s'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
