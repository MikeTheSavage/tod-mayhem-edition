"""Static verification of every data option against the GOG game's blocks (read only).

Per option: the block re-parses, header checksums are valid, create records are unchanged (all inside
builder.verify), only the intended properties differ (independent diff), and the values are the documented ones.
Vanilla levels must give byte-identical blocks. Cross-checks: four options reproduce the research labs' own
packages byte for byte. Set TODGP_SLOW=1 for the 22 cutscene blocks and the TOD HD base.
"""
import hashlib
import os
import struct
import unittest

from common import GOG, HD, HAVE_GOG, HAVE_HD, SAVES, SLOW, out  # noqa: F401

from gameplay import builder, options as opt
from gameplay.gamefs import GameFS, MAIN

_CTX = {}


def ctx(game=GOG):
    if game not in _CTX:
        _CTX[game] = builder.Context(GameFS(game, skip=[builder.ZIP_NAME]))
    return _CTX[game]


def run(sel, game=GOG):
    return builder.build_blocks(ctx(game), sel, log=lambda *a: None)


def changes(rep):
    return {(c['block'], c['fragment'], c['node'], c['prop']): c for c in rep['changes']}


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def value(rep, path, prop, fragment=None):
    hits = [c for c in rep['changes'] if c['path'] == path and c['prop'] == prop
            and (fragment is None or c['fragment'].endswith(fragment))]
    assert len(hits) == 1, (path, prop, len(hits))
    return hits[0]


@unittest.skipUnless(HAVE_GOG, 'GOG game folder not found')
class Options(unittest.TestCase):

    def test_all_vanilla_builds_nothing(self):
        rep, blocks = run(opt.defaults())
        self.assertEqual(blocks, [])
        self.assertEqual(rep['changes'], [])

    def test_vanilla_levels_are_byte_identical(self):
        # the generators of the vanilla levels emit no op at all, and an explicit no-op set keeps the block identical
        c = ctx()
        for o in opt.OPTIONS:
            self.assertEqual(o['gen'](c, o['levels'][0]), [], o['key'])
        # a value written back unchanged: block stays byte-identical (proves the pipeline is identity on no-ops)
        ess = c.doc(MAIN, opt.F_ESS)
        ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
        op = opt.Op(MAIN, opt.F_ESS, ps, 'DamageNormal', ess.get(ps, 'DamageNormal'), None, 'identity')
        old, new, docs, _ = builder.apply_block(c, MAIN, [op])
        self.assertEqual(new, c.block(MAIN)['raw'])
        self.assertEqual(docs, {})

    def test_driving_matches_research_p1(self):
        rep, blocks = run({'driving': 'better_feel'})
        self.assertEqual(len(rep['changes']), 51)
        self.assertEqual(value(rep, 'MoveCtrl', 'turn_rate')['new'], f32(0.12))
        self.assertEqual(value(rep, 'Sedan', 'cornering_stiffness')['new'], f32(1.08))
        self.assertEqual(value(rep, 'Sedan', 'control_speed')['new'], 4.0)
        self.assertFalse(any(c['path'] in ('Dirtbike', 'Chopper') for c in rep['changes']))
        # research package P1 (builds/proposal, overdose.main sha256 ab6dfe7e65f3e06e...)
        self.assertTrue(hashlib.sha256(blocks[0][1]).hexdigest().startswith('ab6dfe7e65f3e06e'))

    def test_difficulty_presets(self):
        base = {}
        ess = ctx().doc(MAIN, opt.F_ESS)
        ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
        for p in ('DamageEasy', 'DamageNormal', 'DamageHard', 'PlayerDamageMod'):
            base[p] = ess.get(ps, p)
        for level in ('easy', 'hard', 'brutal'):
            f = opt.DIFFICULTY[level]
            rep, blocks = run({'difficulty': level})
            self.assertEqual(len(blocks), 1)
            self.assertEqual({c['option'] for c in rep['changes']}, {'difficulty'})
            self.assertAlmostEqual(value(rep, 'MasterCtrlAux/PlayerStatusCtrl', 'DamageNormal')['new'],
                                   1.1 * f['dmg_taken'], places=5)
            self.assertAlmostEqual(value(rep, 'MasterCtrlAux/PlayerStatusCtrl', 'PlayerDamageMod')['new'],
                                   f['dmg_dealt'], places=5)
            mac10 = value(rep, 'SubMachineGun/sub_automatic_light', 'SpreadEnemy_m')['new']
            exp = 1.4 + 1.5 if level == 'easy' else 1.4 * f['spread'][1]
            self.assertAlmostEqual(mac10, exp, places=5)
            self.assertAlmostEqual(value(rep, 'MasterCtrlAux/BulletDrop', 'VeryManyDropTo')['new'],
                                   250 * f['ammo'], places=3)
            props = {c['prop'] for c in rep['changes']}
            self.assertEqual(props, {'DamageEasy', 'DamageNormal', 'DamageHard', 'PlayerDamageMod', 'SpreadEnemy_m',
                                     'MinPauseTime', 'MaxPauseTime', 'Wait_time_from', 'Wait_time_to'}
                             | set(opt.DROP_BANDS))
            # player rows never touched; only the ten firearms get spread / pause changes
            self.assertFalse(any('Ram_ModelStat' in c['path'] or c['path'] == 'Player' for c in rep['changes']))
            weap = {c['path'] for c in rep['changes'] if c['fragment'] == opt.F_WEAP}
            self.assertEqual(len(weap), 10)
        rep, blocks = run({'difficulty': 'normal'})
        self.assertEqual(blocks, [])

    def test_camera_distance(self):
        expect = {'far': '9abdf9693c515959', 'farther': 'ee82afb07e71a162'}   # research camera builds
        for level, back in opt.CAMERA_BACK.items():
            rep, blocks = run({'camera_distance': level})
            if not back:
                self.assertEqual(blocks, [])
                continue
            self.assertEqual(len(rep['changes']), 2)
            cp = value(rep, 'Animation/CameraCtrl/CameraPos', 'pos')
            dist = ((cp['new'][1] - cp['old'][1]) ** 2 + (cp['new'][2] - cp['old'][2]) ** 2) ** 0.5
            self.assertAlmostEqual(dist, abs(back), places=4)
            self.assertEqual(cp['new'][0], cp['old'][0])
            if level in expect:
                self.assertTrue(hashlib.sha256(blocks[0][1]).hexdigest().startswith(expect[level]), level)

    def test_vehicle_camera(self):
        rep, blocks = run({'vehicle_camera': 'far'})
        self.assertEqual(len(rep['changes']), 29)
        self.assertEqual({c['prop'] for c in rep['changes']}, {'pos'})

    def test_slowmo(self):
        for level, k in opt.SLOWMO_COST.items():
            rep, blocks = run({'slowmo_length': level})
            if k == 1.0:
                self.assertEqual(blocks, [])
                continue
            self.assertEqual(value(rep, 'MasterCtrlAux/PlayerStatusCtrl', 'StaminaCostShootdodge')['new'], 125.0 * k)
            if level == 'long':   # research package L2 data part (loco_override.zip, bb29f29d9534e0e2...)
                self.assertTrue(hashlib.sha256(blocks[0][1]).hexdigest().startswith('bb29f29d9534e0e2'))
        for level, k in opt.SLOWMO_REGEN.items():
            rep, blocks = run({'slowmo_regen': level})
            if k == 1.0:
                self.assertEqual(blocks, [])
                continue
            self.assertEqual(value(rep, 'MasterCtrlAux/PlayerStatusCtrl', 'StaminaGainStanding')['new'], 150.0 * k)
            self.assertEqual(value(rep, 'MasterCtrlAux/PlayerStatusCtrl', 'StaminaGainRunning')['new'], 80.0 * k)

    def test_combined_equals_union_of_single_options(self):
        sel = {'driving': 'better_feel', 'difficulty': 'hard', 'camera_distance': 'far', 'vehicle_camera': 'far',
               'slowmo_length': 'long', 'slowmo_regen': 'fast'}
        union = {}
        for k, v in sel.items():
            union.update(changes(run({k: v})[0]))
        rep, blocks = run(sel)
        got = changes(rep)
        self.assertEqual(set(got), set(union))
        for k in got:
            self.assertEqual(got[k]['new_bytes'], union[k]['new_bytes'], k)
        self.assertEqual(len(blocks), 1)

    def test_conflicting_options_are_refused(self):
        orig = list(opt.OPTIONS)

        def dup(c, level):
            ess = c.doc(MAIN, opt.F_ESS)
            ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
            return [] if level == 'off' else [opt.Op(MAIN, opt.F_ESS, ps, 'StaminaCostShootdodge', 1.0, 125.0, 'x')]
        try:
            opt.OPTIONS.append(dict(key='zz_test', levels=['off', 'on'], gen=dup, help=''))
            opt.BY_KEY['zz_test'] = opt.OPTIONS[-1]
            with self.assertRaises(builder.BuildError) as cm:
                run({'slowmo_length': 'long', 'zz_test': 'on'})
            self.assertIn('both change', str(cm.exception))
        finally:
            opt.OPTIONS[:] = orig
            opt.BY_KEY.pop('zz_test', None)

    def test_unknown_option_or_level(self):
        with self.assertRaises(builder.BuildError):
            builder.parse_selection({'difficulty': 'nightmare'})
        with self.assertRaises(builder.BuildError):
            builder.parse_selection({'fov': '90'})

    def test_options_ini_example(self):
        sel = builder.read_options_ini(os.path.join(os.path.dirname(builder.__file__), '..', 'options.ini'))
        self.assertEqual(sel, opt.defaults())

    def test_zip_and_engine_lookup(self):
        p = out('t_zip')
        rep = builder.build(GOG, {'slowmo_length': 'long'}, p, log=lambda *a: None, ctx=ctx())
        self.assertEqual(rep['result'], 'PASS')
        e = rep['zip']['entries'][0]
        self.assertTrue(e['engine_lookup_ok'])
        self.assertEqual(e['entry'], MAIN)
        import zipfile
        with zipfile.ZipFile(p) as z:
            self.assertEqual([i.compress_type for i in z.infolist()], [zipfile.ZIP_STORED])

    def test_refuses_to_write_into_game_folders(self):
        for bad in (os.path.join(GOG, 'x.zip'), os.path.join(HD, 'todgameplay_data.zip')):
            with self.assertRaises(builder.BuildError):
                builder.guard_out(bad, GOG)

    @unittest.skipUnless(os.path.exists(os.path.join(SAVES, 'Slot00')), 'no save copies')
    def test_save_check_finds_only_camera_copies(self):
        from gameplay import savecheck
        rep, _ = run({'driving': 'better_feel', 'difficulty': 'brutal', 'slowmo_length': 'long',
                      'slowmo_regen': 'fast'})
        res = savecheck.check(SAVES, GOG, rep['changes'])
        self.assertEqual(res['hits'], [], res['hits'][:3])
        rep, _ = run({'camera_distance': 'far'})
        res = savecheck.check(SAVES, GOG, rep['changes'])
        whats = {h['what'].split(' ')[0] for h in res['hits']}
        self.assertIn('Animation/CameraCtrl.end_pos', whats)

    @unittest.skipUnless(SLOW, 'set TODGP_SLOW=1')
    def test_cutscenes_unlock_all(self):
        rep, blocks = run({'cutscenes': 'unlock_all'})
        self.assertEqual(len(blocks), len(opt.UNSKIP_BLOCKS))
        self.assertEqual({c['prop'] for c in rep['changes']}, {'Unskipable'})
        self.assertTrue(all(c['new'] is False for c in rep['changes']))
        self.assertEqual(len(rep['changes']), 70)          # 26 sequences + their slow-motion / real-time actions
        import todblock as tb
        import todfrag as tf
        import fragtool as ft
        T = ft.types()
        P = T.index('Unskipable')
        for entry, data in blocks:                          # nothing in these blocks stays locked
            b = tb.Block(data, os.path.splitext(entry)[1])
            for k, _ in ft.fragments(b)[0]:
                fr = tf.Fragment(b.payloads[k], T)
                self.assertFalse(any(r.op == tf.SET and r.prop == P and fr.value(r) is True for r in fr.records))


@unittest.skipUnless(HAVE_GOG and HAVE_HD and SLOW, 'TOD HD base: set TODGP_SLOW=1')
class OnTodHD(unittest.TestCase):
    def test_main_block_comes_from_hd_override(self):
        fs = GameFS(HD, skip=[builder.ZIP_NAME])
        self.assertEqual(fs.locate(MAIN)[0].lower(), 'hd_override1.zip')

    def test_combined_on_hd(self):
        sel = {'driving': 'better_feel', 'difficulty': 'hard', 'camera_distance': 'far', 'slowmo_length': 'long'}
        rep, blocks = run(sel, game=HD)
        self.assertEqual(rep['blocks'][0]['base']['archive'].lower(), 'hd_override1.zip')
        # same edits as on the GOG block: the gameplay fragments of HD's overdose.main are GOG's
        gog = changes(run(sel)[0])
        hd = changes(rep)
        self.assertEqual(set(gog), set(hd))
        for k in gog:
            self.assertEqual(gog[k]['new_bytes'], hd[k]['new_bytes'])


if __name__ == '__main__':
    unittest.main()
