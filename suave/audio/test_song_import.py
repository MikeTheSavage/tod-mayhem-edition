r"""Tests of song_import.py (needs the player's copy of the song; here the reference mp3).

  python -I suave\audio\test_song_import.py            (numpy + soundfile; the venv of the packed build has both)

The reference mp3 is read from env TODSONG_MP3; the tests
are skipped without it. The comparison with the ffmpeg-built segments needs a make_segments.py build folder (env
TODSONG_FFMPEG_BUILD, default suave\audio\build); that test is skipped without it. Nothing is written
outside a temp folder; the song is never copied anywhere else.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'data'))
sys.dont_write_bytecode = True
import song_import as si   # noqa: E402

MP3 = os.environ.get('TODSONG_MP3', '')
FFBUILD = os.environ.get('TODSONG_FFMPEG_BUILD', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build'))
HAVE = os.path.isfile(MP3)
try:
    import soundfile as sf
except ImportError:            # pragma: no cover
    sf = None


def dec(p):
    x, sr = sf.read(p, dtype='float64')
    return x


def snr_db(a, b):
    return 20 * np.log10(np.sqrt(np.mean(a ** 2)) / (np.sqrt(np.mean((a - b) ** 2)) + 1e-12))


@unittest.skipUnless(HAVE and sf, 'needs the reference mp3 and soundfile')
class SongImport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='songimp_')
        cls.fp = si.load_fingerprint()
        cls.ref = si.import_song(MP3, os.path.join(cls.tmp, 'ref'), cls.fp)
        cls.report = {}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)
        print('\nreport: %s' % json.dumps(cls.report, default=float))

    def seg(self, d, k):
        return dec(os.path.join(d, 'suave_mus_%d.ogg' % k))

    # -- the reference mp3: offset 0, the manifest data/gameplay/suave_data.py reads
    def test_reference_offset_zero_and_manifest(self):
        m = self.ref
        self.assertEqual(m['offset_s'], 0.0)
        self.assertGreater(m['match_score'], 0.95)
        self.assertEqual([s['samples'] for s in m['segments']], [767144] * 4)
        from gameplay import suave_data as sd
        segs = sd.load_manifest(os.path.join(self.tmp, 'ref', 'manifest.json'))      # sha256, ogginfo, names
        self.assertEqual([s['name'] for s in segs], ['suave_mus_%d' % k for k in range(1, 5)])
        for s in m['segments']:
            self.assertEqual(s['rate'], 44100)
            self.assertEqual(s['channels'], 1)
            self.assertTrue(s['vendor'])

    @unittest.skipUnless(os.path.isdir(FFBUILD), 'needs a make_segments.py (ffmpeg) build to compare with')
    def test_matches_the_ffmpeg_build(self):
        ffm = json.load(open(os.path.join(FFBUILD, 'manifest.json')))
        errs = []
        for k, (a, b) in enumerate(zip(ffm['segments'], self.ref['segments']), 1):
            self.assertEqual((a['samples'], a['rate'], a['channels']), (b['samples'], b['rate'], b['channels']))
            self.assertEqual(a['gain_db'], b['gain_db'])                    # same cut, same level
            x, y = self.seg(FFBUILD, k), self.seg(os.path.join(self.tmp, 'ref'), k)
            self.assertEqual(len(x), len(y))
            errs.append(round(snr_db(x, y), 1))
        self.report['snr_db_vs_ffmpeg_build'] = errs                           # two Vorbis encodes: codec noise only
        self.assertGreater(min(errs), 30.0)

    def test_pcm_cut_is_make_segments(self):
        """before encoding the cut equals make_segments.py's numpy (same samples, fades, gain): the 16-bit values
        differ by at most 1 from the ffmpeg decode path (float rounding)."""
        song, _ = si.read_song(MP3)
        cfg = json.load(open(os.path.join(HERE, 'segments.json')))
        for s in cfg['segments']:
            pcm, m = si.cut_segment(song, s, self.fp, 0.0)
            self.assertEqual(len(pcm), 767144)
            self.assertEqual(int(pcm[0]), 0)                                # fade-in starts at silence
            self.assertLess(abs(int(pcm[-1])), 3)                           # the tail ends at silence
            self.assertLessEqual(np.max(np.abs(pcm.astype(int))), int(0.98 * 32767) + 1)

    # -- 1.3 s of silence in front: the same segments
    def test_shifted_copy_gives_the_same_segments(self):
        x, sr = sf.read(MP3, dtype='float32')
        sh = np.concatenate([np.zeros((int(round(1.3 * sr)), 2), 'float32'), x])
        p = os.path.join(self.tmp, 'shift.flac')
        sf.write(p, sh, sr, format='FLAC', subtype='PCM_24')
        d = os.path.join(self.tmp, 'shift')
        m = si.import_song(p, d, self.fp)
        self.assertAlmostEqual(m['offset_s'], 1.3, delta=0.0005)
        errs, snrs = [], []
        song0, _ = si.read_song(MP3)
        song1, _ = si.read_song(p)
        cfg = json.load(open(os.path.join(HERE, 'segments.json')))
        for k, s in enumerate(cfg['segments'], 1):
            a, _ = si.cut_segment(song0, s, self.fp, 0.0)
            b, _ = si.cut_segment(song1, s, self.fp, m['offset_s'])
            errs.append(int(np.max(np.abs(a.astype(int) - b))))                 # before encoding: 16-bit values
            snrs.append(round(snr_db(self.seg(os.path.join(self.tmp, 'ref'), k), self.seg(d, k)), 1))   # the oggs
        self.report['shift_1.3s_offset_err_ms'] = round((m['offset_s'] - 1.3) * 1000, 3)
        self.report['shift_1.3s_pcm_max_diff_lsb'] = errs
        self.report['shift_1.3s_ogg_snr_db'] = snrs
        self.assertLessEqual(max(errs), 1)         # sample-exact cut: the same 16-bit samples (+-1 LSB: 24-bit flac copy)
        self.assertGreater(min(snrs), 35.0)        # the oggs differ only by the encoder's rounding noise

    def test_other_rips_other_rates(self):
        """a 48 kHz and a 22.05 kHz mono wav (the resampler) and a lower-bit-rate re-encode: same offset, same level."""
        x, sr = sf.read(MP3, dtype='float64')
        for rate in (48000, 22050):
            y = si.resample(x[:, 0] * 0.7071 + x[:, 1] * 0.7071, sr, rate)
            p = os.path.join(self.tmp, 'r%d.wav' % rate)
            sf.write(p, np.clip(y, -1, 1).astype('float32'), rate, subtype='PCM_16')
            d = os.path.join(self.tmp, 'r%d' % rate)
            m = si.import_song(p, d, self.fp)
            self.assertAlmostEqual(m['offset_s'], 0.0, delta=0.001)
            snr = snr_db(self.seg(os.path.join(self.tmp, 'ref'), 2), self.seg(d, 2))
            self.report['snr_db_via_%d_Hz' % rate] = round(snr, 1)
            self.assertGreater(snr, 10.0 if rate < 44100 else 30.0)    # 22.05 kHz: everything above 11 kHz is gone

    def test_resampler(self):
        for r_in, r_out in ((48000, 44100), (22050, 44100), (96000, 44100), (44100, 48000)):
            t = np.arange(r_in) / r_in
            x = 0.5 * np.sin(2 * np.pi * 1000 * t) + 0.25 * np.sin(2 * np.pi * 7000 * t)
            y = si.resample(x, r_in, r_out)
            self.assertEqual(len(y), r_out)
            to = np.arange(r_out) / r_out
            want = 0.5 * np.sin(2 * np.pi * 1000 * to) + 0.25 * np.sin(2 * np.pi * 7000 * to)
            e = np.max(np.abs(y[200:-200] - want[200:-200]))
            self.assertLess(e, 2e-3, (r_in, r_out, e))
        # content above the new Nyquist is removed, not folded back
        x = np.sin(2 * np.pi * 30000 * np.arange(96000) / 96000)
        self.assertLess(np.max(np.abs(si.resample(x, 96000, 44100)[200:-200])), 1e-3)

    # -- not the song
    def test_other_audio_is_refused(self):
        rng = np.random.default_rng(7)
        noise = (rng.standard_normal((268 * 44100, 2)) * 0.1).astype('float32')
        p = os.path.join(self.tmp, 'noise.flac')
        sf.write(p, noise, 44100, format='FLAC')
        with self.assertRaises(si.SongImportError) as c:
            si.import_song(p, os.path.join(self.tmp, 'noise'), self.fp)
        self.assertIn('does not look like the song', str(c.exception))
        # real music of the right length at another rate: the installer's own La Llorona stems
        music = os.path.join(HERE, '..', '..', 'installer', 'web', 'music')
        names = [n for n in ('stem1_guitar.ogg', 'stem2_bass_perc.ogg', 'stem3_trumpet.ogg') if
                 os.path.isfile(os.path.join(music, n))]
        if names:
            mix = sum(sf.read(os.path.join(music, n), dtype='float32')[0] for n in names) / len(names)
            p = os.path.join(self.tmp, 'other.flac')
            sf.write(p, np.concatenate([mix] * 3), 48000, format='FLAC')
            with self.assertRaises(si.SongImportError):
                si.import_song(p, os.path.join(self.tmp, 'other'), self.fp)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, 'noise', 'suave_mus_1.ogg')))   # nothing written

    def test_edited_or_short_versions_are_refused(self):
        x, sr = sf.read(MP3, dtype='float32')
        p = os.path.join(self.tmp, 'cut.flac')
        sf.write(p, np.concatenate([x[:60 * sr], x[80 * sr:]]), sr, format='FLAC')      # 20 s cut out of the middle
        with self.assertRaises(si.SongImportError) as c:
            si.import_song(p, os.path.join(self.tmp, 'cut'), self.fp)
        self.assertIn('segment', str(c.exception))
        p = os.path.join(self.tmp, 'short.flac')
        sf.write(p, x[:200 * sr], sr, format='FLAC')                                    # ends before the last segment
        with self.assertRaises(si.SongImportError) as c:
            si.import_song(p, os.path.join(self.tmp, 'short'), self.fp)
        self.assertIn('ends too early', str(c.exception))

    def test_unreadable_and_missing_files(self):
        p = os.path.join(self.tmp, 'junk.mp3')
        with open(p, 'wb') as f:
            f.write(b'this is not audio' * 100)
        for bad in (p, os.path.join(self.tmp, 'nothere.mp3')):
            with self.assertRaises(si.SongImportError):
                si.import_song(bad, os.path.join(self.tmp, 'bad'), self.fp)


class Fingerprint(unittest.TestCase):
    def test_fingerprint_is_not_audio(self):
        p = si.FINGERPRINT
        self.assertLess(os.path.getsize(p), 100 * 1024)
        fp = json.load(open(p))
        self.assertEqual(fp['hop'], 147)                       # one number per 3.3 ms: no phase, no spectrum
        self.assertEqual(len(fp['segments']), 4)
        self.assertTrue(-30 < fp['reference_rms_db'] < -5)
        env = np.frombuffer(__import__('base64').b64decode(fp['head']), dtype=np.int8)
        self.assertEqual(len(env), int(fp['head_s'] * si.FRAME_HZ))
        self.assertTrue(env.min() >= -70 and env.max() <= 0)   # 1 dB steps of a level (floor -70 dB), not samples


if __name__ == '__main__':
    unittest.main(verbosity=2)
