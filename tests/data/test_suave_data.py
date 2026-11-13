"""Static verification of the `suave` data option against the GOG game's blocks (read only).

The uk HUD texts and the appended music stream records (gameplay/suave_data.py), the set ops (icon stand-in, start
charges), the raw ogg entries and the zip written through the engine's FindFile port. Needs the music build
(suave/audio/make_segments.py); skipped without it.
"""
import os
import struct
import unittest
import zipfile

from common import GOG, HAVE_GOG, out

from gameplay import builder, suave_data as sd
from gameplay.gamefs import GameFS

HAVE_MUSIC = os.path.exists(sd.MANIFEST)


@unittest.skipUnless(HAVE_GOG and HAVE_MUSIC, 'needs the GOG game and suave/audio/build')
class SuaveData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        cls.rep, cls.entries = builder.build_blocks(cls.ctx, {'suave': 'on_start2'}, log=lambda *a: None)

    def test_set_ops(self):
        ch = {(c['path'], c['prop']): c for c in self.rep['changes']}
        self.assertEqual(ch[(sd.ICON_ENTITY, 'texture')]['new'], sd.ICON_NEW)
        self.assertEqual(ch[('MasterCtrlAux/PlayerStatusCtrl', 'Default_Tequillas')]['new'], 2)
        self.assertEqual(len(ch), 2)

    def test_uk_block(self):
        s = self.rep['suave']
        self.assertEqual(s['records_after'], s['records_before'] + 4)
        self.assertEqual([t['ordinal'] for t in s['texts']], [28, 231])
        self.assertTrue(s['texts'][0]['new'].startswith('SUAVE\\n'))
        self.assertEqual(s['texts'][1]['new'], 'Suave')
        for k, r in enumerate(s['stream_records'], 1):
            self.assertEqual(r['name'], '/data/Sounds/StreamedPowerMoves/Suave_Mus_%d.stream' % k)
            self.assertEqual(r['rate'], 44100)
        uk = dict(self.entries)[sd.UK]
        b = sd.tb.Block(uk, '.main')
        self.assertEqual(b.n, s['records_after'])
        self.assertEqual(b.sizes[-4:], [0, 0, 0, 0])

    def test_vanilla_and_twice(self):
        rep, ent = builder.build_blocks(self.ctx, {'suave': 'off'}, log=lambda *a: None)
        self.assertEqual(ent, [])
        uk = dict(self.entries)[sd.UK]
        with self.assertRaises(sd.SuaveDataError):
            sd.apply_uk(uk, sd.load_manifest())          # never stacked on an already built block

    def test_slot10_level(self):
        rep, ent = builder.build_blocks(self.ctx, {'suave': 'on_slot10'}, log=lambda *a: None)
        self.assertEqual(rep['changes'], [])                       # the Pinata keeps its icon, no start charges
        self.assertEqual([t['ordinal'] for t in rep['suave']['texts']], [27, 230])
        self.assertTrue(rep['suave']['texts'][0]['old'].startswith('PINATA'))
        self.assertEqual(rep['suave']['texts'][1]['new'], 'Suave')
        self.assertEqual(len(rep['suave']['stream_records']), 4)
        self.assertEqual(sorted(e for e, _ in ent if not e.endswith('.ogg')), [sd.UK])

    def test_without_music(self):
        """suave_music = off (no song file): the HUD texts and nothing of the music; works without any build folder."""
        rep, ent = builder.build_blocks(self.ctx, {'suave': 'on_slot10', 'suave_music': 'off'}, log=lambda *a: None)
        s = rep['suave']
        self.assertEqual(s['records_after'], s['records_before'])
        self.assertEqual(s['stream_records'], [])
        self.assertEqual(s['oggs'], [])
        self.assertEqual([t['ordinal'] for t in s['texts']], [27, 230])
        self.assertEqual([e for e, _ in ent], [sd.UK])                       # no ogg entries
        uk = dict(ent)[sd.UK]
        b = sd.tb.Block(uk, '.main')
        self.assertEqual(b.n, s['records_before'])
        info = bytes(b.info)
        names = [sd.tb.record_name(info, R) for R in sd.tb.record_starts(info, b.n)]
        self.assertFalse(any('suave_mus' in n.lower() for n in names))
        # the same block as the music build in everything but the 4 appended records
        rep2, ent2 = builder.build_blocks(self.ctx, {'suave': 'on_slot10'}, log=lambda *a: None)
        self.assertEqual(rep2['suave']['records_after'], s['records_before'] + 4)

    def test_manifest_from_another_folder(self):
        """ctx.suave_manifest (the installer's per-user cache) replaces suave/audio/build."""
        import shutil
        import tempfile
        d = tempfile.mkdtemp(prefix='suavemus_')
        try:
            shutil.copytree(os.path.dirname(sd.MANIFEST), d, dirs_exist_ok=True)
            ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
            ctx.suave_manifest = os.path.join(d, 'manifest.json')
            rep, ent = builder.build_blocks(ctx, {'suave': 'on_slot10'}, log=lambda *a: None)
            self.assertEqual(len(rep['suave']['stream_records']), 4)
            ctx.suave_manifest = os.path.join(d, 'nothere', 'manifest.json')
            with self.assertRaises(builder.BuildError):
                builder.build_blocks(ctx, {'suave': 'on_slot10'}, log=lambda *a: None)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_stream_record_layout(self):
        raw = self.ctx.block(sd.UK)['raw']
        b = sd.tb.Block(raw, '.main')
        info = bytes(b.info)
        st = sd.tb.record_starts(info, b.n)
        k = [i for i, R in enumerate(st) if sd.tb.record_name(info, R) == sd.TEMPLATE][0]
        tmpl = info[st[k]:st[k + 1]]
        rec = sd.make_stream_record(tmpl, st[k], 3, 12345, 44100, 1)
        self.assertEqual(len(rec), len(tmpl))
        r = sd.read_stream(rec, 0)
        self.assertEqual((r['name'], r['file'], r['samples'], r['pcm'], r['channels'], r['rate']),
                         ('/data/Sounds/StreamedPowerMoves/Suave_Mus_3.stream',
                          '/data/sounds/streamedpowermoves/suave_mus_3.ogg', 12345, 24690, 1, 44100))
        diff = [i for i in range(len(rec)) if rec[i] != tmpl[i]]
        self.assertTrue(all(0x40 <= i < 0x60 or 0x60 <= i < 0x90 or 0xB0 <= i < 0xD0 for i in diff), diff)

    def test_zip(self):
        p = out('sb_suave_test')
        rep = builder.build(GOG, {'suave': 'on'}, p, log=lambda *a: None, ctx=self.ctx)
        self.assertEqual(rep['result'], 'PASS')
        names = [e['entry'] for e in rep['zip']['entries']]
        for k in range(1, 5):
            self.assertIn('data/sounds/streamedpowermoves/suave_mus_%d.ogg' % k, names)
        self.assertIn(sd.UK, names)
        with zipfile.ZipFile(p) as z:
            self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))
        self.assertEqual(sd.sound_collisions(self.ctx.fs, names), [])


if __name__ == '__main__':
    unittest.main()
