r"""make_segments.py - cut the Suave music segments from the Suavemente mp3 and encode them for the game.

  python make_segments.py [--mp3 "<your copy of Suavemente>"] [--ffmpeg PATH]
                          [--game "C:\GOG Games\Total Overdose"] [--out build]

Reproducible from the mp3 path alone: segments.json (start beats, length, fades) + suave/anim/work/bpm.json (the beat
grid) fully determine the cut; ffmpeg decodes, cuts sample-exactly on the beat grid, fades, matches the loudness of the
game's own El Mariachi stream (read from sounds.naz, read only; skipped without --game) and encodes the format the
game's streamed music uses: Ogg Vorbis, 44,100 Hz, MONO (only one stereo stream may play at a time and mission music
is stereo), ~80 kbps, no loop tags (subsystem_streamed_sound.md; Mariachi_02.ogg is the model).

Output (never the mp3): <out>/suave_mus_<k>.ogg and <out>/manifest.json with, per segment, the song times, the exact
sample count of the ogg (last granule == decoded samples, checked), sha256. The data option `suave` of
data/build_gameplay_mod.py reads this manifest: the stream record of each segment must carry exactly these samples.
The build folder is git-ignored (the oggs are derived from a commercial recording; rebuild them locally).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_MP3 = os.environ.get('TODSONG_MP3', 'Suavemente.mp3')
DEF_FFMPEG = os.environ.get('FFMPEG', 'ffmpeg')
BPM_JSON = os.path.join(HERE, '..', 'anim', 'work', 'bpm.json')
SOUND_TOOLS = os.environ.get('TOD_SOUND_TOOLS', '')  # the author's sound tools (not included)
MARIACHI = 'data/sounds/streamedpowermoves/mariachi_02.ogg'


def run(cmd, data=None):
    p = subprocess.run(cmd, input=data, capture_output=True)
    if p.returncode:
        raise SystemExit('ffmpeg failed: %s\n%s' % (' '.join(cmd[:6]), p.stderr.decode('utf-8', 'replace')[-800:]))
    return p.stdout


def decode_pcm(ffmpeg, src, rate, data=None):
    """mono float64 samples at `rate` (src = path, or '-' with data = file bytes)."""
    raw = run([ffmpeg, '-v', 'error', '-i', src, '-ac', '1', '-ar', str(rate), '-f', 'f32le', '-'], data)
    return np.frombuffer(raw, dtype='<f4').astype(np.float64)


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-12)


def reference_level(ffmpeg, game, rate):
    sys.path.insert(0, SOUND_TOOLS)
    import todblock as tb   # noqa: E402  (read-only archive reader of the sound tools)
    a = tb.Archive(os.path.join(game, 'sounds.naz'))
    data = a.read(MARIACHI)
    x = decode_pcm(ffmpeg, '-', rate, data)
    return rms_db(x), hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mp3', default=DEF_MP3)
    ap.add_argument('--ffmpeg', default=DEF_FFMPEG)
    ap.add_argument('--game', default=r'C:\GOG Games\Total Overdose', help='read only: El Mariachi loudness reference')
    ap.add_argument('--no-reference', action='store_true')
    ap.add_argument('--segments', default=os.path.join(HERE, 'segments.json'))
    ap.add_argument('--out', default=os.path.join(HERE, 'build'))
    a = ap.parse_args()
    cfg = json.load(open(a.segments))
    grid = json.load(open(BPM_JSON))
    period, first = grid['beat_period_s'], grid['first_beat_s']
    rate, bitrate = cfg['rate'], cfg['bitrate']
    if cfg['channels'] != 1:
        raise SystemExit('the game plays one stereo stream at a time: segments must be mono')
    os.makedirs(a.out, exist_ok=True)
    song = decode_pcm(a.ffmpeg, a.mp3, rate)
    with open(a.mp3, 'rb') as f:
        mp3_sha = hashlib.sha256(f.read()).hexdigest()
    ref_db, ref_sha = (None, None)
    if not a.no_reference and os.path.exists(os.path.join(a.game, 'sounds.naz')):
        ref_db, ref_sha = reference_level(a.ffmpeg, a.game, rate)
    out = []
    for k, s in enumerate(cfg['segments'], 1):
        if s['start_beat'] % 4:
            raise SystemExit('%s: start_beat %d is not a downbeat (multiple of 4)' % (s['name'], s['start_beat']))
        t0 = first + s['start_beat'] * period
        dur = s['beats'] * period
        i0 = int(round(t0 * rate))
        n = int(round(dur * rate))
        x = song[i0:i0 + n].copy()
        if len(x) != n:
            raise SystemExit('%s runs past the end of the song' % s['name'])
        fi, fo = int(cfg['fade_in_s'] * rate), int(cfg['fade_out_s'] * rate)
        x[:fi] *= np.linspace(0.0, 1.0, fi, endpoint=False)
        x[n - fo:] *= np.cos(np.linspace(0.0, np.pi / 2, fo)) ** 2      # equal-power style tail to silence
        gain_db = 0.0
        if ref_db is not None:
            gain_db = ref_db - rms_db(x)
        x *= 10 ** (gain_db / 20)
        peak = np.max(np.abs(x))
        if peak > 0.98:                                                   # never clip
            x *= 0.98 / peak
            gain_db += 20 * np.log10(0.98 / peak)
        pcm = (np.clip(x, -1, 1) * 32767).round().astype('<i2').tobytes()
        ogg_path = os.path.join(a.out, s['name'] + '.ogg')
        run([a.ffmpeg, '-v', 'error', '-y', '-f', 's16le', '-ar', str(rate), '-ac', '1', '-i', '-',
             '-c:a', 'libvorbis', '-b:a', bitrate, '-map_metadata', '-1', '-fflags', '+bitexact',
             '-flags:a', '+bitexact', ogg_path], pcm)
        with open(ogg_path, 'rb') as f:
            ogg = f.read()
        sys.path.insert(0, SOUND_TOOLS)
        import ogginfo   # noqa: E402
        oi = ogginfo.info(ogg)
        dec = decode_pcm(a.ffmpeg, ogg_path, rate)
        ok = (oi['channels'] == 1 and oi['rate'] == rate and oi['bad_crc_pages'] == 0 and oi['streams'] == 1
              and oi['samples'] == len(dec) == n)
        if not ok:
            raise SystemExit('%s: ogg check failed (ch %d rate %d samples %d decoded %d expected %d, bad crc %d)' % (
                s['name'], oi['channels'], oi['rate'], oi['samples'], len(dec), n, oi['bad_crc_pages']))
        out.append(dict(name=s['name'], label=s['label'], start_beat=s['start_beat'], beats=s['beats'],
                        song_start_s=round(t0, 4), song_end_s=round(t0 + dur, 4), seconds=round(n / rate, 4),
                        samples=n, rate=rate, channels=1, gain_db=round(gain_db, 2), ogg_bytes=len(ogg),
                        sha256=hashlib.sha256(ogg).hexdigest(), vendor=oi['vendor'],
                        archive_entry='data/sounds/streamedpowermoves/%s.ogg' % s['name'],
                        slot_path='/Sounds/StreamedPowerMoves/%s.wav' % s['name'].capitalize().replace('_mus_', '_Mus_')))
        print('%-12s beats %3d..%3d  song %7.3f..%7.3f s  %6.3f s  %7d samples  gain %+5.1f dB  %6d B  %s' % (
            s['name'], s['start_beat'], s['start_beat'] + s['beats'], t0, t0 + dur, n / rate, n, gain_db, len(ogg),
            s['label']))
    man = dict(tool='suave/audio/make_segments.py', mp3=os.path.basename(a.mp3), mp3_sha256=mp3_sha,
               beat_period_s=period, first_beat_s=first, bpm=grid['bpm'], reference=MARIACHI if ref_db is not None else None,
               reference_rms_db=None if ref_db is None else round(ref_db, 2), reference_sha256=ref_sha,
               segments=out)
    with open(os.path.join(a.out, 'manifest.json'), 'w') as f:
        json.dump(man, f, indent=1)
    print('manifest: %s' % os.path.join(a.out, 'manifest.json'))


if __name__ == '__main__':
    main()
