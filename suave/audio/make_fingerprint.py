r"""make_fingerprint.py - write suave/audio/fingerprint.json from the reference mp3 (run once; the result ships).

  python make_fingerprint.py [--mp3 PATH] [--game "C:\GOG Games\Total Overdose"] [--ffmpeg PATH] [--rms-db X]

The fingerprint is what lets the installer cut the Suave segments out of the PLAYER'S OWN copy of the song without the
repo or the installer holding any of the recording: the beat grid + segment list (segments.json, suave/anim/work/
bpm.json), the loudness target (the game's El Mariachi stream measured the way make_segments.py does; read only; or
--rms-db), and a coarse loudness envelope (no phase, no spectrum: not audio) of the first 60 s and of the windows of
segments 3 and 4 (see song_import.py). The mp3 itself is only read.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_segments as ms    # noqa: E402
import song_import as si      # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mp3', default=ms.DEF_MP3)
    ap.add_argument('--ffmpeg', default=ms.DEF_FFMPEG)
    ap.add_argument('--game', default=r'C:\GOG Games\Total Overdose')
    ap.add_argument('--rms-db', type=float, default=None)
    ap.add_argument('--bpm', default=ms.BPM_JSON)
    ap.add_argument('--out', default=si.FINGERPRINT)
    a = ap.parse_args()
    cfg = json.load(open(os.path.join(HERE, 'segments.json')))
    grid = json.load(open(a.bpm))
    ref_db = a.rms_db
    if ref_db is None:
        ref_db, _ = ms.reference_level(a.ffmpeg, a.game, cfg['rate'])
    song, info = si.read_song(a.mp3)
    fp = si.make_fingerprint(song, cfg, grid, round(float(ref_db), 4))
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump(fp, f, indent=1)
    print('%s: %d bytes, reference %.1f s, reference_rms_db %.4f, head %d frames, anchors %s' % (
        a.out, os.path.getsize(a.out), fp['duration_s'], fp['reference_rms_db'], len(fp['head']) * 3 // 4,
        [(x['segment'], x['start_s']) for x in fp['anchors']]))


if __name__ == '__main__':
    main()
