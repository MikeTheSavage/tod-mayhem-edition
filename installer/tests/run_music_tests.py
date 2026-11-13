r"""Tests of the Mayhem stem player (installer\web\mayhem_music.js) with TEST-ONLY tone stems (never shipped).

  python -I installer\tests\run_music_tests.py

Makes four short tone stems (220 / 330 / 440 / 550 Hz, an impulse at sample 0, a 1000 Hz tail past the loop end) plus
a music.json in a temp folder, serves them with the installer's own server, renders the player offline in headless
Edge (tests\music_stems_test.html) and checks: the API, sample-locked stems, per-stem fades starting on bar lines,
the loop end from music.json, the bus fade out, a missing stem, and no music at all. Nothing under installer\web\music
is read or written.
"""
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import wave
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.dont_write_bytecode = True

from gpi import server  # noqa: E402

SR = 44100
LOOP = 3 * SR                     # 2 bars of 3/4 at 120 BPM
FREQS = [220, 330, 440, 550]
NAMES = ['stem1_guitar.wav', 'stem2_bass_perc.wav', 'stem3_trumpet.wav', 'stem4_strings_choir.wav']


def make_stems(d, skip=None):
    os.makedirs(d, exist_ok=True)
    for k, (n, f) in enumerate(zip(NAMES, FREQS)):
        if k == skip:
            continue
        w = wave.open(os.path.join(d, n), 'wb')
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        fr = bytearray()
        for i in range(LOOP + SR // 2):
            v = 0.9 if i == 0 else 0.2 * math.sin(2 * math.pi * (f if i < LOOP else 1000) * i / SR)
            fr += struct.pack('<h', int(v * 32767))
        w.writeframes(bytes(fr)); w.close()
    with open(os.path.join(d, 'music.json'), 'w') as f:
        json.dump(dict(credit='Test tones (installer test only)', bpm=120, time_signature='3/4',
                       loop_length_samples=LOOP, sample_rate=SR, stems=NAMES), f)


def find_edge():
    for base in (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles')):
        p = os.path.join(base or '', 'Microsoft', 'Edge', 'Application', 'msedge.exe')
        if os.path.isfile(p):
            return p
    return None


class Served:
    def __init__(self, music_dir):
        self.app = server.App(music_dir=music_dir, idle_exit=0, serve_tests=True, registry=False)
        self.httpd = server.serve(self.app)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class MusicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='gpi_music_')
        make_stems(os.path.join(cls.tmp, 'full'))
        make_stems(os.path.join(cls.tmp, 'missing3'), skip=2)
        os.makedirs(os.path.join(cls.tmp, 'none'))
        cls.edge = find_edge()
        cls.prof = os.path.join(cls.tmp, 'edge')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def render(self, case):
        if not self.edge:
            self.skipTest('no Edge')
        s = Served(os.path.join(self.tmp, case))
        p = subprocess.Popen([self.edge, '--headless=new', '--disable-gpu', '--user-data-dir=' + self.prof,
                              '--autoplay-policy=no-user-gesture-required',
                              'http://127.0.0.1:%d/tests/music_stems_test.html?case=%s' % (s.port, case)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            t = time.time()
            while not s.app.test_results and time.time() - t < 120:
                time.sleep(0.2)
        finally:
            p.kill()
            p.wait()
            s.close()
        self.assertTrue(s.app.test_results, 'no result from the test page')
        res = s.app.test_results[0]
        self.assertNotIn('exception', res, res.get('exception'))
        self.assertEqual(res['errors'], [])
        return res

    def test_api(self):
        app = server.App(music_dir=os.path.join(self.tmp, 'missing3'))
        m = app.music()
        self.assertTrue(m['available'])
        self.assertEqual([s['file'] for s in m['stems']], NAMES)
        self.assertIsNone(m['stems'][2]['url'])
        self.assertEqual(m['meta']['bpm'], 120)
        self.assertFalse(server.App(music_dir=os.path.join(self.tmp, 'none')).music()['available'])
        self.assertFalse(server.App(music_dir=os.path.join(self.tmp, 'does_not_exist')).music()['available'])

    def test_full(self):
        r = self.render('full')
        print('\n  full: bar %.3f s, loopEnd %.4f s, lock %s, probes %s' % (r['bar'], r['loopEnd'], r['lock'], r['probe']), file=sys.stderr)
        self.assertTrue(r['loaded'])
        self.assertAlmostEqual(r['bar'], 1.5, places=6)
        self.assertAlmostEqual(r['loopEnd'], 3.0, places=4)
        p = {k: v for k, v in r['probe'].items()}
        keys = list(p)
        on = lambda v, want: [abs(a - b) < 0.12 for a, b in zip(v, want)]          # noqa: E731
        self.assertTrue(all(on(p[keys[0]], [1, 0, 0, 0])), p[keys[0]])               # 1 toggle: stem 1
        self.assertTrue(all(on(p[keys[1]], [1, 0, 0, 0])), p[keys[1]])               # 2nd toggle waits for the bar
        self.assertTrue(0.2 < p[keys[2]][1] < 0.6, p[keys[2]])                        # ... fading in from the bar line
        self.assertTrue(all(on(p[keys[3]], [1, 1, 0, 0])), p[keys[3]])
        self.assertTrue(all(on(p[keys[4]], [1, 1, 0, 0])), p[keys[4]])               # 3 of 3: stems 3 and 4 wait ...
        self.assertTrue(all(on(p[keys[5]], [1, 1, 1, 1])), p[keys[5]])               # ... and come in together
        self.assertTrue(all(on(p[keys[6]], [1, 1, 1, 1])), p[keys[6]])               # back to 1: still on until the bar
        self.assertTrue(all(on(p[keys[7]], [1, 0, 0, 0])), p[keys[7]])
        self.assertTrue(0.45 < p[keys[8]][0] < 0.75, p[keys[8]])                      # bus fading out
        self.assertTrue(all(v < 0.02 for v in p[keys[9]] + p[keys[10]]), (p[keys[9]], p[keys[10]]))
        self.assertLess(r['tail1000'], 0.01)                                          # loop end honoured
        self.assertEqual(r['lock']['samples_over_1'], 1, r['lock'])                  # four impulses in ONE sample
        self.assertGreater(r['lock']['peak'], 2.6)
        self.assertEqual(r['lock']['offset_samples'], 0)
        self.assertLess(r['silence_after_fade'], 0.001)

    def test_missing_stem(self):
        r = self.render('missing3')
        self.assertTrue(r['loaded'])
        self.assertEqual([s['loaded'] for s in r['stems']], [True, True, False, True])
        last = list(r['probe'].values())[5]
        self.assertTrue(abs(last[0] - 1) < 0.12 and abs(last[3] - 1) < 0.12 and last[2] < 0.05, last)

    def test_no_music(self):
        r = self.render('none')
        self.assertFalse(r['loaded'])
        self.assertEqual(r['stems'], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
