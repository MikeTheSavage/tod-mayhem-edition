r"""Unit tests of the Gameplay+ installer (stdlib unittest; no game, no registry, temp folders only).

  python -I installer\tests\test_installer.py        (or: python -m unittest discover -s installer\tests)
"""
import contextlib
import hashlib
import io
import re
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.dont_write_bytecode = True

from gpi import backup as B, catalog, engine, gamestate as GS, plan as P, procs, repo  # noqa: E402

# Never the player's own Documents folder: every test that reaches the display side file sees this fake one (and
# passes its own fake folder explicitly on top).
FAKE_DOCS_ROOT = tempfile.mkdtemp(prefix='gpi_fake_documents_')
os.environ[GS.DOCS_ENV] = os.path.join(FAKE_DOCS_ROOT, 'Documents')

SHIPPED = GS.read_text(repo.SHIPPED_INI)
USER_INI = (
    "; my own notes - keep me\r\n"
    "[general]\r\nlog = 1\r\n\r\n"
    "[bullettime]\r\nenabled = 1\r\nkey = G                   ; keyboard G (unused by the game)\r\nkey_alt = MOUSE3\r\n\r\n"
    "[suave]\r\nenabled = 1\r\nslot = 10   ; my test slot\r\ncheat_key = F7\r\n\r\n"
    "[smart_ai]\r\nenabled = 1\r\nmorale = 1\r\n\r\n"
    "[fixes]\r\nvector_realloc = 1\r\nweapon_owner = 1\r\n"
)


def no_procs():
    return []


def sha(p):
    with open(p, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def make_game(root, ini=USER_INI, big=None):
    g = os.path.join(root, 'Total Overdose')
    os.makedirs(os.path.join(g, 'sub'))
    with open(os.path.join(g, 'TOD.exe'), 'wb') as f:
        f.write(b'MZ fake exe')
    with open(os.path.join(g, 'configpc.txt'), 'wb') as f:
        f.write(b'fullscreen = false\r\nscreensize = 1920,1080\r\n')
    with open(os.path.join(g, 'sub', 'notes.txt'), 'wb') as f:
        f.write(b'a file in a sub folder')
    if ini is not None:
        with open(os.path.join(g, P.INI), 'wb') as f:
            f.write(ini.encode('latin-1'))
    if big:
        os.link(big, os.path.join(g, 'blocks.naz'))
    else:
        with open(os.path.join(g, 'blocks.naz'), 'wb') as f:
            f.write(os.urandom(4096))
    return g


def fake_build(game_dir, data, out_zip, report_dir, log):
    os.makedirs(os.path.dirname(out_zip), exist_ok=True)
    with open(out_zip, 'wb') as f:
        f.write(('PK fake zip ' + json.dumps(data, sort_keys=True)).encode())
    os.makedirs(report_dir, exist_ok=True)
    with open(os.path.join(report_dir, 'report.json'), 'w') as f:
        json.dump(dict(selection=data, blocks=[]), f)
    log('fake build')


def fake_prefetch(game_dir, zip_path, out, log):
    with open(out, 'w') as f:
        f.write('# fake prefetch for %s\n' % os.path.basename(zip_path))


def sel(**groups):
    s = catalog.default_selection()
    s['groups'].update(groups)
    return s


def all_off():
    return dict(groups={g['id']: False for g in catalog.GROUPS}, subs={})


class PlanMapping(unittest.TestCase):
    def test_defaults(self):
        p = P.make_plan(catalog.default_selection(), SHIPPED)
        d = p['data']
        self.assertEqual(d['driving'], 'better_feel')
        self.assertEqual(d['seamless'], 'on_nomarkers')
        self.assertEqual(d['suave_anims'], 'on')
        self.assertEqual(d['suave'], 'on_slot10')
        self.assertEqual(d['controller'], 'menu')
        self.assertEqual(d['difficulty'], 'normal')
        self.assertEqual(d['difficulty_menu'], 'off')
        self.assertEqual(d['camera_distance'], 'default')
        self.assertEqual(d['vehicle_camera'], 'default')
        self.assertTrue(p['data_needed'] and p['dll_needed'] and p['prefetch'])
        self.assertFalse(p['physics_dll'])
        ini = {(a, b): (c, m) for a, b, c, m in p['ini']}
        self.assertEqual(ini[('pad', 'enabled')], ('1', 'set'))
        self.assertEqual(ini[('pad', 'prompts')], ('auto', 'set'))
        self.assertEqual(ini[('suave', 'slot')], ('10', 'set'))
        self.assertEqual(ini[('bullettime', 'key')], ('G', 'set'))
        self.assertEqual(ini[('fixes', 'vector_realloc')], ('1', 'set'))
        self.assertEqual(ini[('physics', 'enabled')], ('0', 'if_present'))
        self.assertEqual(ini[('difficulty', 'enabled')], ('0', 'if_present'))
        self.assertEqual(ini[('display', 'borderless')], ('0', 'set'))
        self.assertEqual(p['display'], dict(fullscreen=False, w=1920, h=1080, apply=False))
        self.assertEqual(ini[('display', 'mode_menu')], ('1', 'set'))
        self.assertFalse(p['difficulty_marker'] or p['display_apply'] or p['minimap'] or p['mayhem_dds'])

    def test_merge5_wiring(self):
        # hang-watch: the two linker maps ship with their DLLs and are owned; hud-textures: the HD HUD icons card is a
        # data option, default on; heat-chili: score_label is always 0
        self.assertIn('TODGameplay.map', P.DLL_FILES)
        self.assertIn(P.PHYSICS_MAP, P.OWNED_FILES)
        self.assertIn('TODGameplay.map', P.OWNED_FILES)
        p = P.make_plan(catalog.default_selection(), SHIPPED)
        self.assertEqual(p['data']['hud_textures'], 'on')
        self.assertEqual(P.make_plan(sel(hud_textures=False), SHIPPED)['data']['hud_textures'], 'off')
        self.assertEqual(ini_map(P.make_plan(sel(mayhem_heat=True), SHIPPED))[('mayhem_heat', 'score_label')],
                         ('0', 'set'))
        self.assertEqual(ini_map(P.make_plan(sel(mayhem_heat=False), SHIPPED))[('mayhem_heat', 'score_label')],
                         ('0', 'if_present'))

    def test_mayhem_all_on(self):
        s = sel(physics=True, difficulty=True, more_enemies=True)
        s['subs']['max_live_enemies'] = 7
        p = P.make_plan(s, SHIPPED)
        ini = {(a, b): (c, m) for a, b, c, m in p['ini']}
        self.assertEqual(p['data']['difficulty_menu'], 'on')
        self.assertEqual(p['data']['difficulty'], 'normal')        # never a static level
        self.assertEqual(ini[('physics', 'enabled')], ('1', 'set'))
        self.assertEqual(ini[('difficulty', 'enabled')], ('1', 'set'))
        self.assertEqual(ini[('difficulty', 'max_live_enemies')], ('7', 'set'))
        self.assertNotIn(('physics', 'shell_impulse'), ini)        # left as the player has it
        self.assertNotIn(('physics', 'blast_lift'), ini)
        self.assertTrue(p['physics_dll'] and p['difficulty_marker'] and p['mayhem_on'])

    def test_bigger_fights_one_toggle_with_its_warning(self):
        g = catalog.BY_ID['bigger_fights']
        self.assertEqual(g['section'], 'mayhem')
        self.assertTrue(g['experimental'])
        self.assertFalse(g['default'])
        self.assertEqual(g['requires'], ['difficulty'])
        self.assertIn('OLD SAVES WILL NOT LOAD', g['tagline'])
        self.assertIn('new game', g['tagline'])
        p = P.make_plan(sel(difficulty=True, bigger_fights=True), SHIPPED)
        ini = {(a, b): (c, m) for a, b, c, m in p['ini']}
        self.assertEqual(p['data']['bigger_fights'], 'on')
        self.assertEqual(ini[('bigger_fights', 'enabled')], ('1', 'set'))

    def test_bigger_fights_off_leaves_nothing(self):
        p = P.make_plan(sel(difficulty=True, bigger_fights=False), SHIPPED)
        ini = {(a, b): (c, m) for a, b, c, m in p['ini']}
        self.assertEqual(ini.get(('bigger_fights', 'enabled')), ('0', 'if_present'))
        self.assertNotEqual(p['data'].get('bigger_fights', 'off'), 'on')

    def test_bigger_fights_needs_difficulty(self):
        p = P.make_plan(sel(bigger_fights=True, difficulty=False), SHIPPED)
        self.assertFalse(p['selection']['groups']['bigger_fights'])
        self.assertNotEqual(p['data'].get('bigger_fights', 'off'), 'on')

    def test_dependency_more_enemies_needs_difficulty(self):
        p = P.make_plan(sel(more_enemies=True, difficulty=False), SHIPPED)
        self.assertFalse(p['selection']['groups']['more_enemies'])
        ini = {(a, b): (c, m) for a, b, c, m in p['ini']}
        self.assertEqual(ini[('difficulty', 'max_live_enemies')], ('0', 'if_present'))

    def test_all_off(self):
        p = P.make_plan(all_off(), SHIPPED)
        self.assertFalse(p['data_needed'])
        self.assertFalse(p['dll_needed'])
        self.assertFalse(p['prefetch'])
        self.assertIsNone(p['display'])
        modes = {(a, b): m for a, b, c, m in p['ini']}
        self.assertEqual(modes[('fixes', 'vector_realloc')], 'set')        # default ON in the DLL: written as 0
        self.assertEqual(modes[('seamless', 'enabled')], 'if_present')
        self.assertTrue(all(c == '0' for a, b, c, m in p['ini']))

    def test_subs(self):
        s = sel(display=True, suave=True, seamless=True, controller=True, driving=True, smart_ai=True, qol=True)
        s['subs'].update(display_mode='borderless', suave_slot='11', seamless_hide_markers=False, pad_prompts='playstation',
                         pad_rumble_pct=0, chase_camera=True, max_live_enemies=42, smart_squad=False,
                         bt_key='mouse4', restart_key='F9', ess_crash=False)
        p = P.make_plan(s, SHIPPED, native=(3024, 1964))
        ini = {(a, b): c for a, b, c, m in p['ini']}
        self.assertEqual(p['display'], dict(fullscreen=True, w=3024, h=1964, apply=False))
        self.assertEqual(ini[('display', 'borderless')], '1')
        self.assertEqual(p['data']['suave'], 'on')
        self.assertEqual(ini[('suave', 'slot')], '11')
        self.assertEqual(p['data']['seamless'], 'on')
        self.assertEqual(ini[('pad', 'prompts')], 'playstation')
        self.assertEqual(ini[('pad', 'rumble')], '0')
        self.assertNotIn(('pad', 'rumble_strength'), ini)
        self.assertEqual(p['data']['vehicle_camera'], 'far')                       # Better Driving > Chase camera
        self.assertEqual(ini[('smart_ai', 'squad')], '0')
        self.assertEqual(ini[('bullettime', 'key')], 'MOUSE4')
        self.assertEqual(ini[('restart', 'key')], 'F9')
        self.assertEqual(ini[('fixes', 'vector_realloc')], '0')                    # crash fixes unticked
        self.assertEqual(ini[('timing', 'clock_fix')], '1')                        # stability fixes still on
        self.assertEqual(catalog.normalize(s)['subs']['max_live_enemies'], 9)       # clamped to 6..9

    def test_rumble_percent(self):
        s = sel(controller=True)
        s['subs']['pad_rumble_pct'] = 150
        ini = {(a, b): c for a, b, c, m in P.make_plan(s, SHIPPED)['ini']}
        self.assertEqual((ini[('pad', 'rumble')], ini[('pad', 'rumble_strength')]), ('1', '1.5'))
        legacy = catalog.normalize(dict(groups={}, subs=dict(pad_rumble=True, pad_rumble_strength=0.7)))
        self.assertEqual(legacy['subs']['pad_rumble_pct'], 70)
        self.assertEqual(catalog.normalize(dict(subs=dict(pad_rumble=False)))['subs']['pad_rumble_pct'], 0)

    def test_key_fields(self):
        for bad in ('ESC', 'MOUSE1', 'MOUSE2', 'PAD_A', 'NOPE', ''):
            self.assertEqual(catalog.normalize(dict(subs=dict(bt_key=bad)))['subs']['bt_key'], 'G', bad)
        self.assertEqual(catalog.normalize(dict(subs=dict(bt_key='numpad5')))['subs']['bt_key'], 'NUMPAD5')

    def test_key_names_known_to_the_dll(self):
        """every key name the installer may write is in TODGameplay.dll's own table (dll/src/gp_logic.cpp kKeys)"""
        src = open(os.path.join(repo.ROOT, 'dll', 'src', 'gp_logic.cpp'), encoding='utf-8', errors='replace').read()
        table = src[src.index('kKeys[]'):]
        table = table[:table.index('};')]
        names = set(re.findall(r'\{"([A-Z0-9_]+)"', table))
        self.assertTrue(len(names) > 100)
        self.assertEqual(sorted(set(catalog.KEY_NAMES) - names), [])

    def test_experimental_cards(self):
        exp = sorted(g['id'] for g in catalog.GROUPS if g.get('experimental'))
        # Seamless and Suave as before; the four Mayhem groups built on the PROTOTYPE [physics] world too
        self.assertEqual(exp, ['bigger_fights', 'breakable_cover', 'car_physics', 'chain_reactions', 'ragdoll', 'seamless', 'suave'])

    def test_data_levels_are_real(self):
        """every level the plan can produce exists in data/gameplay/options.py"""
        opts = repo.options()
        for s in (catalog.default_selection(), sel(physics=True, difficulty=True)):
            for k, v in P.make_plan(s, SHIPPED)['data'].items():
                self.assertIn(v, opts.BY_KEY[k]['levels'], k)

    def test_ini_keys_exist_in_shipped(self):
        """every key the installer sets is a key of the shipped TODGameplay.ini"""
        s = sel(physics=True, difficulty=True, more_enemies=True)
        s['subs'].update(steer_return=True, brake_parity=True, chase_camera=True)
        for a, b, c, m in P.make_plan(s, SHIPPED)['ini']:
            self.assertTrue(m == 'retired' or P.ini_has(SHIPPED, a, b), '[%s] %s' % (a, b))

    def test_mayhem_title_guarded(self):
        p = P.make_plan(sel(physics=True), SHIPPED.replace('[mayhem_title]', '[x_mayhem_title]'))
        self.assertNotIn('mayhem_title', [a for a, b, c, m in p['ini']])
        t = SHIPPED + '\n[mayhem_title]\nenabled = 0\nintensity = 1\n'
        on = {(a, b): (c, m) for a, b, c, m in P.make_plan(sel(physics=True), t)['ini']}
        off = {(a, b): (c, m) for a, b, c, m in P.make_plan(sel(), t)['ini']}
        self.assertEqual(on[('mayhem_title', 'enabled')], ('1', 'set'))
        self.assertEqual(off[('mayhem_title', 'enabled')], ('0', 'if_present'))


class IniMerge(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = make_game(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_merge_keeps_user_lines(self):
        s = sel(smart_ai=False, physics=True)
        s['subs']['bt_key'] = 'B'
        p = P.make_plan(s, SHIPPED)
        engine.merge_ini(self.game, p, os.path.join(self.tmp, 'st'), lambda m: None)
        out = open(os.path.join(self.game, P.INI), 'rb').read().decode('latin-1')
        self.assertTrue(out.startswith('; my own notes - keep me\r\n'))
        self.assertIn('key = B                   ; keyboard G (unused by the game)\r\n', out)   # value only
        self.assertIn('key_alt = MOUSE3\r\n', out)
        self.assertIn('slot = 10   ; my test slot\r\n', out)
        self.assertIn('cheat_key = F7\r\n', out)
        self.assertIn('[smart_ai]\r\nenabled = 0\r\nmorale = 1\r\n', out)              # off, not deleted
        self.assertIn('[physics]', out)                                               # copied from the template
        self.assertEqual(GS.ini_value(out, 'physics', 'enabled'), '1')
        self.assertEqual(GS.ini_value(out, 'physics', 'shell_impulse'), '450')
        self.assertEqual(GS.ini_value(out, 'physics', 'blast_lift'), '4')
        self.assertEqual(GS.ini_value(out, 'pad', 'enabled'), '1')
        self.assertNotIn('[difficulty]', out)                                          # off and absent: not added
        self.assertNotIn('\n\n\n', out.replace('\r', ''))
        for ln in USER_INI.split('\r\n'):
            if ln and '=' not in ln:
                self.assertIn(ln, out)

    def test_psycho_tune_upgrades_an_older_ini(self):
        # an ini from an install before psycho-tune (2026-11-07): PSYCHO at x1.85, no wake share, no PSYCHO reinforce share
        old = ('[difficulty]\r\nenabled = 1\r\nreinforce_share = 0.5     ; extra enemies per enemy death\r\n'
               'max_live_enemies = 9\r\n\r\n[bigger_fights]\r\nenabled = 1\r\ncap_scale_brutal = 1.5    ; live enemies\r\n'
               'cap_scale_psycho = 1.85\r\nwake_radius = 90\r\n\r\n[psycho]\r\nenemy_loco_moves = 1\r\n')
        open(os.path.join(self.game, P.INI), 'wb').write(old.encode('latin-1'))
        p = P.make_plan(sel(difficulty=True, bigger_fights=True), SHIPPED)
        engine.merge_ini(self.game, p, os.path.join(self.tmp, 'st'), lambda m: None)
        out = GS.read_text(os.path.join(self.game, P.INI))
        self.assertEqual(GS.ini_value(out, 'bigger_fights', 'cap_scale_psycho'), '1.3')
        self.assertEqual(GS.ini_value(out, 'bigger_fights', 'psycho_wake_share'), '0.6')
        self.assertEqual(GS.ini_value(out, 'difficulty', 'reinforce_share_psycho'), '0.375')
        self.assertEqual(GS.ini_value(out, 'bigger_fights', 'cap_scale_brutal'), '1.5')    # BRUTAL as it was
        self.assertEqual(GS.ini_value(out, 'difficulty', 'reinforce_share'), '0.5')
        # the shipped ini holds the same values (the DLL's defaults, gp_bigfights_logic.h / gp_difficulty_logic.h)
        for sec, key, val in P.PSYCHO_TUNE['bigger_fights'] + P.PSYCHO_TUNE['difficulty']:
            self.assertEqual(GS.ini_value(SHIPPED, sec, key), val)
        # off: nothing of it is written
        q = {(a, b) for a, b, c, m in P.make_plan(sel(difficulty=True, bigger_fights=False), SHIPPED)['ini']}
        self.assertNotIn(('bigger_fights', 'cap_scale_psycho'), q)

    def test_merge_twice_is_stable(self):
        p = P.make_plan(sel(), SHIPPED)
        engine.merge_ini(self.game, p, os.path.join(self.tmp, 'st'), lambda m: None)
        a = open(os.path.join(self.game, P.INI), 'rb').read()
        engine.merge_ini(self.game, p, os.path.join(self.tmp, 'st'), lambda m: None)
        self.assertEqual(a, open(os.path.join(self.game, P.INI), 'rb').read())

    def test_no_ini_and_no_dll(self):
        os.remove(os.path.join(self.game, P.INI))
        engine.merge_ini(self.game, P.make_plan(all_off(), SHIPPED), os.path.join(self.tmp, 'st'), lambda m: None)
        self.assertFalse(os.path.exists(os.path.join(self.game, P.INI)))

    def test_no_ini_gets_shipped_one(self):
        os.remove(os.path.join(self.game, P.INI))
        engine.merge_ini(self.game, P.make_plan(sel(), SHIPPED), os.path.join(self.tmp, 'st'), lambda m: None)
        out = GS.read_text(os.path.join(self.game, P.INI))
        self.assertEqual(GS.ini_value(out, 'seamless', 'enabled'), '1')
        self.assertEqual(GS.ini_value(out, 'physics', 'enabled'), '0')


class BackupRestore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.big = os.path.join(self.tmp, 'original_blocks.naz')       # stands in for a GOG archive
        with open(self.big, 'wb') as f:
            f.write(os.urandom(1 << 20))
        self.big_sha = sha(self.big)
        self.game = make_game(self.tmp, big=self.big)
        self.root = os.path.join(self.tmp, 'Gameplay+ Backups')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def mutate(self):
        B.write_bytes(os.path.join(self.game, 'blocks.naz'), b'changed archive')     # replace, never write into
        B.write_bytes(os.path.join(self.game, 'dsound.dll'), b'added by the installer')
        B.write_bytes(os.path.join(self.game, P.SELECTION_FILE), b'{}')
        os.remove(os.path.join(self.game, 'sub', 'notes.txt'))
        B.write_bytes(os.path.join(self.game, P.INI), b'[general]\r\nlog = 0\r\n')

    def test_full_round_trip(self):
        before = B.folder_digest(self.game)
        d, man = B.make_backup(self.game, self.root, 'full', lambda m: None)
        self.assertEqual(len(man['files']), len(before))
        self.assertEqual(B.verify_backup(d), [])
        self.mutate()
        self.assertEqual(sha(self.big), self.big_sha)                 # the hard-linked original was not written
        res = B.restore(d, self.game, lambda m: None)
        self.assertEqual(B.folder_digest(self.game), before)          # byte-identical
        self.assertIn('dsound.dll', res['removed'])
        self.assertIn(P.SELECTION_FILE, res['removed'])
        self.assertEqual(sha(self.big), self.big_sha)

    def test_changed_only_round_trip(self):
        before = B.folder_digest(self.game)
        d, man = B.make_backup(self.game, self.root, 'changed', lambda m: None)
        self.assertEqual(set(man['files']), {'configpc.txt', P.INI})
        self.assertIn('dsound.dll', man['absent'])
        B.write_bytes(os.path.join(self.game, 'dsound.dll'), b'added')
        B.write_bytes(os.path.join(self.game, P.INI), b'changed')
        B.restore(d, self.game, lambda m: None)
        self.assertEqual(B.folder_digest(self.game), before)

    def test_corrupt_backup_refused(self):
        d, man = B.make_backup(self.game, self.root, 'full', lambda m: None)
        with open(os.path.join(d, 'files', 'configpc.txt'), 'ab') as f:
            f.write(b'x')
        self.mutate()
        after = B.folder_digest(self.game)
        with self.assertRaises(B.BackupError):
            B.restore(d, self.game, lambda m: None)
        self.assertEqual(B.folder_digest(self.game), after)            # nothing touched

    def test_list_backups(self):
        B.make_backup(self.game, self.root, 'changed', lambda m: None)
        B.make_backup(self.game, self.root, 'full', lambda m: None)
        lst = B.list_backups(self.root)
        self.assertEqual(len(lst), 2)
        self.assertEqual({b['kind'] for b in lst}, {'full', 'changed'})


class RunningRefusal(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = make_game(self.tmp)
        self.root = os.path.join(self.tmp, 'bk')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def lister(self, path):
        return lambda: [(4242, path, os.path.basename(path).lower() if path else 'tod.exe'), (7, r'C:\x\notepad.exe', 'notepad.exe')]

    def test_game_processes(self):
        here = os.path.join(self.game, 'TOD.exe')
        self.assertEqual(procs.game_processes(self.game, self.lister(here)), [(4242, here)])
        self.assertEqual(procs.game_processes(self.game, self.lister(r'D:\elsewhere\TOD.exe')), [])
        self.assertEqual(procs.game_processes(self.game, self.lister(None)), [(4242, None)])   # unknown path: refuse
        self.assertEqual(len(procs.game_processes(self.game, self.lister(os.path.join(self.game, 'TotalOverdose.exe')))), 1)

    def test_install_refused_while_running(self):
        before = B.folder_digest(self.game)
        lister = self.lister(os.path.join(self.game, 'TOD.exe'))
        c = engine.check(self.game, sel(), self.root, 'full', False, lister)
        self.assertTrue(any('running' in p for p in c['problems']))
        job = engine.Job('install', engine.INSTALL_STEPS)
        engine.run_install(job, self.game, sel(), self.root, 'full', False, lister, fake_build, fake_prefetch)
        self.assertEqual(job.state, 'failed')
        self.assertEqual(job.steps[0]['status'], 'failed')
        self.assertFalse(os.path.exists(self.root))                  # not even a backup
        self.assertEqual(B.folder_digest(self.game), before)

    def test_restore_refused_while_running(self):
        d, _ = B.make_backup(self.game, self.root, 'full', lambda m: None)
        B.write_bytes(os.path.join(self.game, 'configpc.txt'), b'changed')
        before = B.folder_digest(self.game)
        job = engine.Job('restore', engine.RESTORE_STEPS)
        engine.run_restore(job, self.game, d, False, self.lister(os.path.join(self.game, 'TOD.exe')))
        self.assertEqual(job.state, 'failed')
        self.assertEqual(B.folder_digest(self.game), before)

    def test_other_folder_does_not_block(self):
        c = engine.check(self.game, sel(), self.root, 'full', False, self.lister(r'C:\Other\TOD.exe'))
        self.assertFalse(any('running' in p for p in c['problems']))

    def test_foreign_dsound_refused(self):
        B.write_bytes(os.path.join(self.game, 'dsound.dll'), b'MZ some other dsound proxy')
        c = engine.check(self.game, sel(), self.root, 'full', False, no_procs)
        self.assertTrue(any('dsound.dll' in p for p in c['problems']))


class InstallJob(unittest.TestCase):
    """The whole job on a fake game (fake data build; the real configpc / ini / DLL / backup code)."""
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.big = os.path.join(self.tmp, 'original_blocks.naz')
        with open(self.big, 'wb') as f:
            f.write(os.urandom(1 << 20))
        self.big_sha = sha(self.big)
        self.game = make_game(self.tmp, big=self.big)
        self.root = os.path.join(self.tmp, 'Gameplay+ Backups')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_job(self, s, kind='full'):
        job = engine.Job('install', engine.INSTALL_STEPS)
        engine.run_install(job, self.game, s, self.root, kind, False, no_procs, fake_build, fake_prefetch)
        self.assertEqual(job.state, 'done', job.error)
        return job

    def test_install_switch_off_restore(self):
        before = B.folder_digest(self.game)
        s = sel(physics=True, difficulty=True, more_enemies=True)
        job = self.run_job(s)
        g = lambda n: os.path.join(self.game, n)          # noqa: E731
        for f in ('TODGameplay.dll', 'dsound.dll', 'TODPhysics.dll'):
            self.assertEqual(sha(g(f)), sha(os.path.join(repo.DIST, f)))
        self.assertTrue(os.path.exists(g(P.ZIP)) and os.path.exists(g(P.PREFETCH)))
        cfg = GS.read_text(g('configpc.txt'))
        self.assertIn('filesystem = todgameplay_data.zip, blocks.naz', cfg)
        self.assertEqual(open(g(P.DIFF_MARKER), 'rb').read(), b'menu\r\n')
        ini = GS.read_text(g(P.INI))
        self.assertEqual(GS.ini_value(ini, 'difficulty', 'max_live_enemies'), '9')
        info = json.load(open(g(P.SELECTION_FILE)))
        self.assertTrue(info['selection']['groups']['physics'])
        self.assertTrue(os.path.isfile(os.path.join(job.result['backup'], 'install.log')))
        self.assertEqual(GS.initial_selection(self.game)[1], 'saved')
        self.assertEqual(sha(self.big), self.big_sha)
        first_backup = job.result['backup']
        # second run: everything off, changed-files backup
        self.run_job(all_off(), 'changed')
        for f in ('TODGameplay.dll', 'dsound.dll', 'TODPhysics.dll', P.ZIP, P.PREFETCH, P.DIFF_MARKER):
            self.assertFalse(os.path.exists(g(f)), f)
        self.assertNotIn('todgameplay_data.zip', GS.read_text(g('configpc.txt')))
        ini = GS.read_text(g(P.INI))
        for sec in ('seamless', 'suave', 'smart_ai', 'pad', 'physics', 'difficulty', 'display', 'bullettime'):
            self.assertEqual(GS.ini_value(ini, sec, 'enabled'), '0', sec)              # kept, switched off
        self.assertEqual(GS.ini_value(ini, 'suave', 'cheat_key'), 'F7')
        # restore the first (full) backup: byte-identical to the start
        rj = engine.Job('restore', engine.RESTORE_STEPS)
        engine.run_restore(rj, self.game, first_backup, False, no_procs)
        self.assertEqual(rj.state, 'done', rj.error)
        self.assertEqual(B.folder_digest(self.game), before)
        self.assertEqual(sha(self.big), self.big_sha)

    def test_space_refusal(self):
        c = engine.check(self.game, sel(), self.root, 'full', False, no_procs)
        self.assertTrue(c['space']['drives'])
        orig = GS.free_bytes
        try:
            GS.free_bytes = lambda p: 1000
            c = engine.check(self.game, sel(), self.root, 'full', False, no_procs)
            self.assertTrue(any('Not enough space' in p for p in c['problems']))
        finally:
            GS.free_bytes = orig

    def test_backup_inside_game_refused(self):
        c = engine.check(self.game, sel(), os.path.join(self.game, 'bk'), 'full', False, no_procs)
        self.assertTrue(any('inside the game folder' in p for p in c['problems']))


def fake_hdfx(game_dir, staging, sets, user_ini, log):
    """The real staging minus the archive scan (a fake game has no textures): the real DLL, ini merge and payload,
    plus stand-in generated files."""
    hs = repo.hdfx_stage()
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)
    shutil.copyfile(hs.RELEASE_DLL, os.path.join(staging, 'd3d9.dll'))
    shutil.copyfile(os.path.join(hs.PAYLOAD, 'tod_hd_light.txt'), os.path.join(staging, 'tod_hd_light.txt'))
    shutil.copytree(os.path.join(hs.PAYLOAD, 'tod_hd_fx'), os.path.join(staging, 'tod_hd_fx'))
    shipped = open(os.path.join(hs.PAYLOAD, 'tod_hd_fx.ini'), encoding='utf-8').read()
    text = hs.merge_user_ini(shipped, user_ini, sets) if user_ini else hs.apply_sets(shipped, sets)
    with open(os.path.join(staging, 'tod_hd_fx.ini'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    for rel in ('light/manifest.txt', 'light/sh.txt', 'particles/manifest.txt', 'sky/manifest.txt'):
        with open(os.path.join(staging, 'tod_hd_fx', *rel.split('/')), 'w') as f:
            f.write('# fake generated %s\n' % rel)
    return dict(classes=1, particles=1, sky=5, sh=True, targets=7), hs.staged_files(staging)


def ini_val(text, key):
    for ln in text.splitlines():
        s = ln.split(';', 1)[0].strip()
        if '=' in s and s.split('=', 1)[0].strip() == key:
            return s.split('=', 1)[1].strip()
    return None


class HdfxPlan(unittest.TestCase):
    def test_off_by_default_no_plan(self):
        s = catalog.default_selection()
        s['groups']['hdfx'] = False
        self.assertIsNone(P.make_plan(s, SHIPPED)['hdfx'])

    def test_defaults(self):
        p = P.make_plan(sel(hdfx=True), SHIPPED)
        sets = dict(x.split('=') for x in p['hdfx']['sets'])
        self.assertEqual(sets, dict(aniso_all='16', ssao='1', post='1', light_shafts='1', mist='1', volumetric_beams='1',
                                    volumetric_light='1', particle_mips='1', soft_particles='1', part_lit='1',
                                    heat_haze='1', lighting_mode='1', water_mode='1', dyn_shadows='1',
                                    static_shadows='0', fps_cap='60'))
        ini = {(a, b): (c, m) for a, b, c, m in p['ini']}
        self.assertEqual(ini[('timing', 'fps_limit')], ('0', 'set'))      # one frame limiter
        self.assertTrue(p['dll_needed'])                                    # the loader sets the log folder

    def test_shadows_need_lighting(self):
        s = sel(hdfx=True)
        s['subs'].update(hd_lighting=False, hd_static_shadows=True)
        sets = dict(x.split('=') for x in P.make_plan(s, SHIPPED)['hdfx']['sets'])
        self.assertEqual((sets['lighting_mode'], sets['dyn_shadows'], sets['static_shadows'], sets['water_mode']),
                         ('0', '0', '0', '0'))
        s['subs'].update(hd_lighting=True, hd_fps_cap=False, hd_filtering=False)
        p = P.make_plan(s, SHIPPED)
        sets = dict(x.split('=') for x in p['hdfx']['sets'])
        self.assertEqual((sets['dyn_shadows'], sets['static_shadows'], sets['fps_cap'], sets['aniso_all']),
                         ('1', '1', '0', '1'))
        self.assertNotIn(('timing', 'fps_limit'), {(a, b) for a, b, c, m in p['ini']})

    def test_every_set_key_is_in_the_shipped_hd_ini(self):
        hs = repo.hdfx_stage()
        text = open(os.path.join(hs.PAYLOAD, 'tod_hd_fx.ini'), encoding='utf-8').read()
        for keys in P.HDFX_SUBS.values():
            for k, _on, _off in keys:
                self.assertIsNotNone(ini_val(text, k), k)
        for k in ('borderless', 'dpi_aware', 'resolution_lock', 'sky_dome', 'shine', 'env', 'veg', 'parallax',
                  'tex_override', 'part_flipbook', 'explosion_fx', 'terrain_rock', 'sun_shadows', 'local_reflections',
                  'clearcoat', 'yard_decals', 'level_looks', 'ambient_warm', 'vehicle_hd', 'replacement_models'):
            self.assertEqual(ini_val(text, k), '0', k)

    def test_every_payload_file_is_owned(self):
        """the off switch removes exactly HDFX_FILES: every shipped payload file and every generated one is in it"""
        hs = repo.hdfx_stage()
        ship = set(hs.staged_files(hs.PAYLOAD))
        ship = {x.replace('\\', '/') for x in ship if x != 'tod_hd_fx.ini'} | {'tod_hd_fx.ini', 'd3d9.dll'}
        gen = {'tod_hd_fx/light/manifest.txt', 'tod_hd_fx/light/sh.txt', 'tod_hd_fx/particles/manifest.txt',
               'tod_hd_fx/sky/manifest.txt'}
        self.assertEqual(ship | gen, set(P.HDFX_FILES))

    def test_release_dll_is_the_todhd_release(self):
        hs = repo.hdfx_stage()
        self.assertEqual(hs.sha_file(hs.RELEASE_DLL), hs.RELEASE_SHA256)


class HdfxInstall(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = make_game(self.tmp)
        self.root = os.path.join(self.tmp, 'Gameplay+ Backups')
        self.other = b"MZ another program's d3d9.dll"
        with open(os.path.join(self.game, 'd3d9.dll'), 'wb') as f:
            f.write(self.other)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_job(self, s, kind='full'):
        job = engine.Job('install', engine.INSTALL_STEPS)
        engine.run_install(job, self.game, s, self.root, kind, False, no_procs, fake_build, fake_prefetch, fake_hdfx)
        self.assertEqual(job.state, 'done', job.error)
        return job

    def test_install_keep_tuning_off_restore(self):
        g = lambda *n: os.path.join(self.game, *n)          # noqa: E731
        before = B.folder_digest(self.game)
        c = engine.check(self.game, sel(hdfx=True), self.root, 'full', False, no_procs)
        self.assertTrue(any('another d3d9.dll' in w for w in c['warnings']), c['warnings'])
        self.assertFalse(c['problems'], c['problems'])
        job = self.run_job(sel(hdfx=True))
        first = job.result['backup']
        hs = repo.hdfx_stage()
        self.assertEqual(sha(g('d3d9.dll')), hs.RELEASE_SHA256)
        self.assertEqual(open(g(P.HDFX_ASIDE), 'rb').read(), self.other)
        self.assertTrue(hs.is_ours_ini(g('tod_hd_fx.ini')))
        for rel in P.HDFX_FILES:
            self.assertTrue(os.path.isfile(g(*rel.split('/'))), rel)
        self.assertEqual(engine.hdfx_state(self.game), 'ours')
        # the player tunes a value; a reinstall with fewer effects keeps it and switches the sub-option off
        ini = open(g('tod_hd_fx.ini'), encoding='utf-8').read().replace('sharpen = 0.30', 'sharpen = 0.55')
        open(g('tod_hd_fx.ini'), 'w', encoding='utf-8').write(ini)
        s = sel(hdfx=True)
        s['subs']['hd_ao'] = False
        self.run_job(s, 'changed')
        ini = open(g('tod_hd_fx.ini'), encoding='utf-8').read()
        self.assertEqual((ini_val(ini, 'sharpen'), ini_val(ini, 'ssao')), ('0.55', '0'))
        # off: our files go, the other d3d9.dll comes back, no empty tod_hd_fx folder is left
        self.run_job(all_off(), 'changed')
        self.assertEqual(open(g('d3d9.dll'), 'rb').read(), self.other)
        self.assertFalse(os.path.exists(g('tod_hd_fx')) or os.path.exists(g('tod_hd_fx.ini')) or
                         os.path.exists(g(P.HDFX_ASIDE)) or os.path.exists(g('tod_hd_light.txt')))
        # and the first backup restores the folder byte for byte
        self.run_job(sel(hdfx=True), 'changed')
        rj = engine.Job('restore', engine.RESTORE_STEPS)
        engine.run_restore(rj, self.game, first, False, no_procs)
        self.assertEqual(rj.state, 'done', rj.error)
        self.assertEqual(B.folder_digest(self.game), before)
        self.assertFalse(os.path.exists(g('tod_hd_fx')))

    def test_todhd_install_refused(self):
        hs = repo.hdfx_stage()
        shutil.copyfile(hs.RELEASE_DLL, os.path.join(self.game, 'd3d9.dll'))
        with open(os.path.join(self.game, 'tod_hd_fx.ini'), 'w') as f:
            f.write('; TOD HD fx -- settings for d3d9.dll\nenabled = 1\n')
        c = engine.check(self.game, sel(hdfx=True), self.root, 'full', False, no_procs)
        self.assertTrue(any('TOD HD install' in p for p in c['problems']), c['problems'])
        job = engine.Job('install', engine.INSTALL_STEPS)              # off never removes a TOD HD install
        engine.run_install(job, self.game, all_off(), self.root, 'changed', False, no_procs, fake_build, fake_prefetch,
                           fake_hdfx)
        self.assertEqual(job.state, 'done', job.error)
        self.assertTrue(os.path.isfile(os.path.join(self.game, 'tod_hd_fx.ini')))
        self.assertEqual(sha(os.path.join(self.game, 'd3d9.dll')), hs.RELEASE_SHA256)


# ---------------------------------------------------------------------------------------------- merge round 2026-10-29
NEW_MAYHEM = ['ragdoll', 'car_physics', 'chain_reactions', 'breakable_cover', 'mayhem_heat', 'gang_wars']
PHYSICS_BASED = ['ragdoll', 'car_physics', 'chain_reactions', 'breakable_cover']


def everything_on():
    s = catalog.default_selection()
    for g in catalog.GROUPS:
        s['groups'][g['id']] = True
        for sub in g['subs']:
            if sub['type'] == 'check':
                s['subs'][sub['id']] = True
    return s


def ini_map(plan):
    return {(a, b): (c, m) for a, b, c, m in plan['ini']}


class MergeRoundPlan(unittest.TestCase):
    def test_catalogue_shape(self):
        mh = [g['id'] for g in catalog.GROUPS if g['section'] == 'mayhem']
        self.assertEqual(mh, ['physics'] + PHYSICS_BASED + ['mayhem_heat', 'difficulty', 'more_enemies', 'bigger_fights', 'gang_wars'])
        self.assertIn(len(mh) % 3, (0, 1))                         # the Mayhem grid has 3 columns; Bigger Fights is the tenth card, alone on its row (no web edit for it)
        self.assertEqual(catalog.COMING_SOON, [])
        self.assertIn('hud', [g['id'] for g in catalog.GROUPS if g['section'] == 'main'])
        for gid in PHYSICS_BASED:
            self.assertEqual(catalog.BY_ID[gid]['requires'], ['physics'], gid)
        ids = {s['id'] for g in catalog.GROUPS for s in g['subs']}
        self.assertEqual(len(ids), sum(len(g['subs']) for g in catalog.GROUPS))     # sub ids are unique
        for g in catalog.GROUPS:                                   # a sub's `requires` is a sub of the same card
            for s in g['subs']:
                if s.get('requires'):
                    self.assertIn(s['requires'], [x['id'] for x in g['subs']], s['id'])

    def test_physics_based_groups_need_physics(self):
        for gid in PHYSICS_BASED:
            p = P.make_plan(sel(**{gid: True, 'physics': False}), SHIPPED)
            self.assertFalse(p['selection']['groups'][gid], gid)
            self.assertFalse(p['physics_dll'])
            p = P.make_plan(sel(**{gid: True, 'physics': True}), SHIPPED)
            self.assertTrue(p['selection']['groups'][gid] and p['physics_dll'], gid)
            self.assertEqual(ini_map(p)[('physics', 'enabled')], ('1', 'set'))

    def test_new_mayhem_groups_on(self):
        s = sel(physics=True, ragdoll=True, car_physics=True, chain_reactions=True, breakable_cover=True,
                mayhem_heat=True, gang_wars=True)
        s['subs'].update(ragdoll_impulse=2.0, car_impulse=1.5, chain_cars=False, heat_escalation_scale=1.25,
                         heat_decay=15, heat_tint=False, heat_icons=True, heat_score=False, gw_provoke=False, gw_spark=True)
        p = P.make_plan(s, SHIPPED)
        ini = ini_map(p)
        want = {('ragdoll', 'enabled'): '1', ('ragdoll', 'impulse_scale'): '2.0',
                ('car_physics', 'enabled'): '1', ('car_physics', 'impulse_scale'): '1.5',
                ('mayhem', 'chain_reactions'): '1', ('mayhem', 'chain_cars'): '0', ('mayhem', 'breakable_cover'): '1',
                ('mayhem_heat', 'enabled'): '1', ('mayhem_heat', 'escalation'): '1',
                ('mayhem_heat', 'escalation_scale'): '1.25', ('mayhem_heat', 'decay_seconds'): '15',
                ('mayhem_heat', 'tint'): '0.0', ('mayhem_heat', 'icons'): '1', ('mayhem_heat', 'score_multiplier'): '0',
                ('gang_wars', 'enabled'): '1', ('gang_wars', 'provoke'): '0', ('gang_wars', 'spark'): '1',
                ('mayhem_title', 'enabled'): '1', ('physics', 'enabled'): '1'}
        for k, v in want.items():
            self.assertEqual(ini[k], (v, 'set'), k)
        self.assertEqual(p['data']['difficulty_menu'], 'off')        # none of them is a data option
        self.assertTrue(p['dll_needed'] and p['physics_dll'] and p['mayhem_dds'])
        self.assertEqual(p['data'], P.make_plan(sel(), SHIPPED)['data'])

    def test_new_groups_off(self):
        ini = ini_map(P.make_plan(all_off(), SHIPPED))
        for k in (('ragdoll', 'enabled'), ('car_physics', 'enabled'), ('mayhem', 'chain_reactions'),
                  ('mayhem', 'breakable_cover'), ('mayhem_heat', 'enabled'), ('gang_wars', 'enabled'),
                  ('minimap', 'enabled'), ('psycho', 'enemy_loco_moves'),
                  ('mayhem_title', 'enabled')):
            self.assertEqual(ini[k], ('0', 'if_present'), k)

    def test_psycho_under_difficulty(self):
        ini = ini_map(P.make_plan(sel(difficulty=True), SHIPPED))
        self.assertEqual(ini[('psycho', 'enemy_loco_moves')], ('1', 'set'))         # default on with Difficulty
        self.assertEqual(ini[('psycho', 'elite_drops')], ('1', 'set'))              # part of the same toggle, on by default
        off = sel(difficulty=True); off['subs']['psycho_drops'] = False
        self.assertEqual(ini_map(P.make_plan(off, SHIPPED))[('psycho', 'elite_drops')], ('0', 'set'))
        s = sel(difficulty=True)
        s['subs']['psycho_loco'] = False
        self.assertEqual(ini_map(P.make_plan(s, SHIPPED))[('psycho', 'enemy_loco_moves')], ('0', 'if_present'))
        self.assertEqual(ini_map(P.make_plan(sel(difficulty=False), SHIPPED))[('psycho', 'enemy_loco_moves')],
                         ('0', 'if_present'))

    def test_drops_under_difficulty(self):
        ini = ini_map(P.make_plan(sel(difficulty=True), SHIPPED))
        self.assertEqual(ini[('drops', 'enabled')], ('1', 'set'))                    # default on with Difficulty
        self.assertEqual(ini[('drops', 'ammo_scale')], ('1.75', 'set'))
        s = sel(difficulty=True); s['subs']['loot_ammo'] = 2.5
        self.assertEqual(ini_map(P.make_plan(s, SHIPPED))[('drops', 'ammo_scale')], ('2.5', 'set'))
        s = sel(difficulty=True); s['subs']['loot_drops'] = False
        self.assertEqual(ini_map(P.make_plan(s, SHIPPED))[('drops', 'enabled')], ('0', 'if_present'))
        self.assertEqual(ini_map(P.make_plan(sel(difficulty=False), SHIPPED))[('drops', 'enabled')], ('0', 'if_present'))

    def test_heat_score_default_on(self):
        ini = ini_map(P.make_plan(sel(mayhem_heat=True), SHIPPED))
        self.assertEqual(ini[('mayhem_heat', 'score_multiplier')], ('1', 'set'))

    def test_heat_escalation_scale_needs_escalation(self):
        s = sel(mayhem_heat=True)
        s['subs'].update(heat_escalation=False, heat_escalation_scale=2.0)
        ini = ini_map(P.make_plan(s, SHIPPED))
        self.assertEqual(ini[('mayhem_heat', 'escalation')], ('0', 'set'))
        self.assertNotIn(('mayhem_heat', 'escalation_scale'), ini)

    def test_hud(self):
        p = P.make_plan(sel(hud=True), SHIPPED)
        ini = ini_map(p)
        for k, v in {('minimap', 'enabled'): '1', ('minimap', 'map'): '1', ('minimap', 'rotate'): '1',
                     ('minimap', 'objective_marker'): '1', ('minimap', 'world_marker'): '1'}.items():
            self.assertEqual(ini[k], (v, 'set'), k)
        self.assertEqual(ini[('weapon_wheel', 'enabled')], ('0', 'retired'))       # the wheel is gone: never on
        self.assertNotIn(('weapon_wheel', 'hold_key'), ini)
        self.assertTrue(p['minimap'])
        s = sel(hud=True)
        s['subs'].update(hud_minimap=False, hud_rotate=True)
        p = P.make_plan(s, SHIPPED)
        ini = ini_map(p)
        self.assertFalse(p['minimap'])                               # no bake (and the folder goes)
        self.assertEqual(ini[('minimap', 'map')], ('0', 'set'))
        self.assertNotIn(('minimap', 'rotate'), ini)
        s['subs'].update(hud_markers=False)
        ini = ini_map(P.make_plan(s, SHIPPED))
        self.assertEqual(ini[('minimap', 'enabled')], ('0', 'if_present'))
        self.assertEqual(ini[('weapon_wheel', 'enabled')], ('0', 'retired'))

    def test_retire_key_keeps_everything_else(self):
        t = '[a]\r\nenabled = 1\r\n[weapon_wheel]\r\nenabled = 1   ; on\r\nslowmo = 0.3\r\n[b]\r\nenabled = 1\r\n'
        self.assertEqual(P.retire_key(t, 'weapon_wheel', 'enabled', 0), t.replace('enabled = 1   ; on', 'enabled = 0   ; on'))
        self.assertEqual(P.retire_key(t, 'nothing', 'enabled', 0), t)

    def test_card_is_minimap_with_three_subs(self):
        g = [x for x in catalog.GROUPS if x['id'] == 'hud'][0]
        self.assertEqual(g['title'], 'Minimap')
        self.assertEqual([x['id'] for x in g['subs']], ['hud_minimap', 'hud_rotate', 'hud_markers'])

    def test_old_wheel_selection_and_ini(self):
        """a selection saved with the wheel's subs loads; an ini with [weapon_wheel] enabled = 1 ends at 0, with the card
        on or off; a game that has only the wheel on is not read as 'Minimap on'"""
        old = sel(hud=True)
        old['subs'].update(hud_wheel=True, wheel_key='C', wheel_slowmo=False)
        norm = catalog.normalize(old)
        for k in ('hud_wheel', 'wheel_key', 'wheel_slowmo'):
            self.assertNotIn(k, norm['subs'])
        wheel = SHIPPED + '\n[weapon_wheel]\nenabled = 1\nhold_key = C\nslowmo = 0.3\n'
        for card in (True, False):
            tmp = tempfile.mkdtemp()
            try:
                game = make_game(tmp, ini=wheel)
                self.assertTrue(GS.installed_state(game)['detected']['groups']['hud'] is False)
                engine.merge_ini(game, P.make_plan(sel(hud=card), SHIPPED), os.path.join(tmp, 'st'), lambda m: None)
                ini = GS.read_text(os.path.join(game, P.INI))
                self.assertEqual(GS.ini_value(ini, 'weapon_wheel', 'enabled'), '0')
                self.assertEqual(GS.ini_value(ini, 'minimap', 'enabled'), '1' if card else '0')
            finally:
                shutil.rmtree(tmp, ignore_errors=True)

    def test_display_mode_menu_and_apply(self):
        s = sel(display=True)
        s['subs']['display_apply'] = True
        p = P.make_plan(s, SHIPPED)
        self.assertTrue(p['display_apply'])
        self.assertEqual(ini_map(p)[('display', 'mode_menu')], ('1', 'set'))
        self.assertFalse(P.saved_selection(p['selection'])['subs']['display_apply'])     # applies once
        s['groups']['display'] = False
        self.assertFalse(P.make_plan(s, SHIPPED)['display_apply'])                      # never without the group

    def test_every_set_key_exists_everything_on(self):
        """every key the installer can set (every group, every check on, and the off variants) is in the shipped ini;
        no debug_* key is ever written"""
        variants = [everything_on(), all_off(), catalog.default_selection()]
        v = everything_on()
        for sub in [x for g in catalog.GROUPS for x in g['subs'] if x['type'] == 'check']:
            v['subs'][sub['id']] = False
        variants.append(v)
        for s in variants:
            for a, b, c, m in P.make_plan(s, SHIPPED)['ini']:
                self.assertTrue(m == 'retired' or P.ini_has(SHIPPED, a, b), '[%s] %s' % (a, b))
                self.assertFalse(b.lower().startswith('debug'), '[%s] %s' % (a, b))

    def test_data_levels_everything_on(self):
        opts = repo.options()
        for k, v in P.make_plan(everything_on(), SHIPPED)['data'].items():
            self.assertIn(v, opts.BY_KEY[k]['levels'], k)

    def test_shipped_ini_has_no_merge_leftovers(self):
        for ln in SHIPPED.splitlines():
            self.assertFalse(ln.startswith(('|||||||', '<<<<<<<', '>>>>>>>', '=======')), ln)
        self.assertIsNone(GS.ini_value(SHIPPED, 'psycho', 'embers'))       # MAYHEM v1 keys no longer under [psycho]

    def test_detected_state_round_trip(self):
        """an ini written for a selection is read back as that selection (no install.json)"""
        tmp = tempfile.mkdtemp()
        try:
            game = make_game(tmp, ini=SHIPPED)
            s = sel(physics=True, ragdoll=True, chain_reactions=True, mayhem_heat=True, gang_wars=True, hud=True,
                    difficulty=True)
            s['subs'].update(psycho_loco=False, gw_spark=True, hud_rotate=False, heat_tint=False, heat_score=False)
            engine.merge_ini(game, P.make_plan(s, SHIPPED), os.path.join(tmp, 'st'), lambda m: None)
            B.write_bytes(os.path.join(game, P.PHYSICS_DLL), b'MZ')
            det = GS.installed_state(game)['detected']
            for gid in ('physics', 'ragdoll', 'chain_reactions', 'mayhem_heat', 'gang_wars', 'hud'):
                self.assertTrue(det['groups'][gid], gid)
            for gid in ('car_physics', 'breakable_cover'):
                self.assertFalse(det['groups'][gid], gid)
            self.assertEqual((det['subs']['psycho_loco'], det['subs']['gw_spark'], det['subs']['hud_rotate'],
                              det['subs']['heat_tint'], det['subs']['heat_score']), (False, True, False, False, False))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


def make_fake_bake(calls, fail=False):
    """Stands in for hud/tools/bake_minimap.py: writes what the tool writes (bake.json, minimap.txt, per map a .png
    and an .occ) and reports progress like the real step."""
    def bake(game_dir, out_dir, log, progress):
        calls.append(out_dir)
        if fail:
            raise engine.StepError('fake bake failure')
        os.makedirs(out_dir, exist_ok=True)
        maps = {}
        for i, k in enumerate(('map02_downtown', 'map03_harbour')):
            B.write_bytes(os.path.join(out_dir, k + '.png'), b'\x89PNG fake ' + k.encode())
            B.write_bytes(os.path.join(out_dir, k + '.occ'), b'occ 16.0 1 1 0 0\n1\n')
            maps[k] = dict(file=k + '.png', source='x', x0=0, z0=0, x1=1, z1=1, w=1, h=1, res=1)
            progress(i + 1, 2)
            log('%s: 1x1 px at 1.00 m/px' % k)
        B.write_bytes(os.path.join(out_dir, 'bake.json'), json.dumps(dict(version=2, maps=maps)).encode())
        B.write_bytes(os.path.join(out_dir, 'minimap.txt'), b'# index\nmap02_downtown 0 0 1 1 1 1 map02_downtown.png\n'
                      b'map03_harbour 0 0 1 1 1 1 map03_harbour.png\n')
        return dict(maps=2, seconds=0.1)
    return bake


class MergeRoundInstall(unittest.TestCase):
    """Minimap bake step, MAYHEM title art, the display side file under a FAKE Documents folder, clean switch-off."""
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = make_game(self.tmp)
        self.root = os.path.join(self.tmp, 'Gameplay+ Backups')
        self.docs = os.path.join(self.tmp, 'Fake Documents')
        self.saves = os.path.join(self.docs, 'Total Overdose', 'savegames')
        os.makedirs(self.saves)
        self.side = os.path.join(self.saves, P.DISPLAY_SIDE)
        for n, data in (('Slot00', b'save 0'), ('Slot11', b'settings'), ('TODGameplay_difficulty.txt', b'level = psycho\r\n'),
                        (P.DISPLAY_SIDE, b'mode = borderless\r\nwidth = 3024\r\nheight = 1964\r\nmonitor = 0,0\r\n')):
            with open(os.path.join(self.saves, n), 'wb') as f:
                f.write(data)
        self.bakes = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_job(self, s, kind='full', bake=None, documents=None):
        job = engine.Job('install', engine.INSTALL_STEPS)
        engine.run_install(job, self.game, s, self.root, kind, False, no_procs, fake_build, fake_prefetch, fake_hdfx,
                           bake or make_fake_bake(self.bakes), documents=self.docs if documents is None else documents)
        self.assertEqual(job.state, 'done', job.error)
        return job

    def g(self, *n):
        return os.path.join(self.game, *n)

    def test_tests_never_see_the_real_documents(self):
        self.assertEqual(GS.documents_dir(), os.path.join(FAKE_DOCS_ROOT, 'Documents'))
        self.assertEqual(GS.documents_dir(self.docs), os.path.abspath(self.docs))
        self.assertTrue(engine.side_file(None).startswith(FAKE_DOCS_ROOT))
        self.assertIsNone(engine.side_file(''))
        from gpi import server
        self.assertEqual(server.App(sandbox=self.tmp, documents=r'C:\elsewhere').documents, '')   # outside the sandbox
        old = os.environ.pop(GS.DOCS_ENV)
        try:
            self.assertEqual(server.App(sandbox=self.tmp).documents, '')         # sandbox without --documents: none
        finally:
            os.environ[GS.DOCS_ENV] = old

    def test_minimap_bake_and_clean_off(self):
        before = B.folder_digest(self.game)
        job = self.run_job(sel(hud=True))
        first = job.result['backup']
        st = {s['id']: s for s in job.steps}
        self.assertEqual(st['minimap']['status'], 'done', st['minimap'])
        self.assertEqual(self.bakes, [self.g(P.MINIMAP_DIR)])
        self.assertEqual(job.snapshot()['flags'], dict(hdfx=False, minimap=True))
        made = B.minimap_files(self.game)
        self.assertEqual(len(made), 6)                               # 2 maps x (png + occ) + bake.json + minimap.txt
        self.assertTrue(set(made) <= set(job.result['files']))
        ini = GS.read_text(self.g(P.INI))
        self.assertEqual((GS.ini_value(ini, 'minimap', 'enabled'), GS.ini_value(ini, 'minimap', 'map')), ('1', '1'))
        # a file the player put into the folder is not the installer's
        B.write_bytes(self.g(P.MINIMAP_DIR, 'my_notes.txt'), b'mine')
        # the map off, markers kept: the bake's files go, the player's file stays, the section stays on
        s = sel(hud=True)
        s['subs']['hud_minimap'] = False
        job = self.run_job(s, 'changed')
        self.assertEqual({x['id']: x['status'] for x in job.steps}['minimap'], 'done')
        self.assertEqual(sorted(os.listdir(self.g(P.MINIMAP_DIR))), ['my_notes.txt'])
        self.assertEqual(len(self.bakes), 1)
        os.remove(self.g(P.MINIMAP_DIR, 'my_notes.txt'))
        # on again, then the whole group off: no folder left; the sections are switched off, not deleted
        self.run_job(sel(hud=True), 'changed')
        self.run_job(all_off(), 'changed')
        self.assertFalse(os.path.exists(self.g(P.MINIMAP_DIR)))
        ini = GS.read_text(self.g(P.INI))
        self.assertEqual(GS.ini_value(ini, 'minimap', 'enabled'), '0')
        # the first backup restores the pre-install folder byte for byte
        self.run_job(sel(hud=True), 'changed')
        rj = engine.Job('restore', engine.RESTORE_STEPS)
        engine.run_restore(rj, self.game, first, False, no_procs, documents=self.docs)
        self.assertEqual(rj.state, 'done', rj.error)
        self.assertEqual(B.folder_digest(self.game), before)

    def test_changed_files_backup_holds_the_minimap(self):
        self.run_job(sel(hud=True))
        baked = {r: sha(self.g(*r.split('/'))) for r in B.minimap_files(self.game)}
        job = self.run_job(all_off(), 'changed')                      # removes the folder
        self.assertFalse(os.path.exists(self.g(P.MINIMAP_DIR)))
        man = B.load_manifest(job.result['backup'])
        self.assertTrue(set(baked) <= set(man['files']))
        rj = engine.Job('restore', engine.RESTORE_STEPS)
        engine.run_restore(rj, self.game, job.result['backup'], False, no_procs, documents=self.docs)
        self.assertEqual(rj.state, 'done', rj.error)
        self.assertEqual({r: sha(self.g(*r.split('/'))) for r in B.minimap_files(self.game)}, baked)

    def test_bake_failure_does_not_stop_the_install(self):
        job = self.run_job(sel(hud=True), bake=make_fake_bake(self.bakes, fail=True))
        self.assertEqual({x['id']: x['status'] for x in job.steps}['minimap'], 'warn')
        self.assertTrue(os.path.isfile(self.g('TODGameplay.dll')))

    def test_mayhem_title_art(self):
        self.run_job(sel(gang_wars=True))
        for f in P.MAYHEM_DDS:
            self.assertEqual(sha(self.g(f)), sha(os.path.join(repo.DIST, f)), f)
        self.assertEqual(GS.ini_value(GS.read_text(self.g(P.INI)), 'mayhem_title', 'enabled'), '1')
        self.run_job(sel(), 'changed')                                 # no Mayhem group: the art goes
        for f in P.MAYHEM_DDS:
            self.assertFalse(os.path.exists(self.g(f)), f)
        self.assertEqual(GS.ini_value(GS.read_text(self.g(P.INI)), 'mayhem_title', 'enabled'), '0')

    def test_physics_groups_install(self):
        s = sel(physics=True, ragdoll=True, car_physics=True, chain_reactions=True, breakable_cover=True)
        self.run_job(s)
        ini = GS.read_text(self.g(P.INI))
        self.assertEqual([GS.ini_value(ini, a, b) for a, b in (('physics', 'enabled'), ('ragdoll', 'enabled'),
                                                               ('car_physics', 'enabled'), ('mayhem', 'chain_reactions'),
                                                               ('mayhem', 'breakable_cover'))], ['1'] * 5)
        self.assertEqual(sha(self.g(P.PHYSICS_DLL)), sha(os.path.join(repo.DIST, P.PHYSICS_DLL)))
        self.assertEqual(sha(self.g(P.PHYSICS_MAP)), sha(os.path.join(repo.DIST, P.PHYSICS_MAP)))   # hang reports
        self.assertEqual(sha(self.g('TODGameplay.map')), sha(os.path.join(repo.DIST, 'TODGameplay.map')))
        self.run_job(sel(physics=False, ragdoll=True), 'changed')       # Physics off takes the four with it
        ini = GS.read_text(self.g(P.INI))
        self.assertEqual([GS.ini_value(ini, a, b) for a, b in (('physics', 'enabled'), ('ragdoll', 'enabled'),
                                                               ('car_physics', 'enabled'), ('mayhem', 'chain_reactions'),
                                                               ('mayhem', 'breakable_cover'))], ['0'] * 5)
        self.assertFalse(os.path.exists(self.g(P.PHYSICS_DLL)))
        self.assertFalse(os.path.exists(self.g(P.PHYSICS_MAP)))

    def saves_digest(self, but=()):
        return {k: v for k, v in B.folder_digest(self.docs).items() if k not in but}

    def test_side_file_only_when_picked(self):
        before = self.saves_digest()
        self.run_job(sel(display=True))                                # no pick: the Documents folder is untouched
        self.assertEqual(self.saves_digest(), before)
        s = sel(display=False)
        s['subs']['display_apply'] = True                              # a pick without the group does nothing
        self.run_job(s, 'changed')
        self.assertEqual(self.saves_digest(), before)

    def test_side_file_backed_up_then_deleted_only_that_file(self):
        rel_side = 'Total Overdose/savegames/' + P.DISPLAY_SIDE
        before = self.saves_digest()
        side_bytes = open(self.side, 'rb').read()
        s = sel(display=True)
        s['subs'].update(display_mode='borderless', display_apply=True)
        job = self.run_job(s)
        bk = job.result['backup']
        self.assertFalse(os.path.exists(self.side))                     # deleted ...
        self.assertEqual(open(os.path.join(bk, 'side', P.DISPLAY_SIDE), 'rb').read(), side_bytes)   # ... after a backup
        self.assertEqual(B.load_manifest(bk)['side_files'][0]['path'], os.path.abspath(self.side))
        self.assertEqual(self.saves_digest(), {k: v for k, v in before.items() if k != rel_side})  # nothing else
        saved = json.load(open(self.g(P.SELECTION_FILE)))
        self.assertFalse(saved['selection']['subs']['display_apply'])  # the next run does not delete again
        # the game keeps a new mode later; a reinstall with the saved selection leaves it alone
        with open(self.side, 'wb') as f:
            f.write(b'mode = windowed\r\nwidth = 2560\r\nheight = 1440\r\nmonitor = 0,0\r\n')
        kept = self.saves_digest()
        self.run_job(saved['selection'], 'changed')
        self.assertEqual(self.saves_digest(), kept)
        # restore: a side file the game wrote since is never overwritten ...
        rj = engine.Job('restore', engine.RESTORE_STEPS)
        engine.run_restore(rj, self.game, bk, False, no_procs, documents=self.docs)
        self.assertEqual(rj.state, 'done', rj.error)
        self.assertEqual(self.saves_digest(), kept)
        # ... but one the install deleted comes back
        os.remove(self.side)
        engine.run_restore(engine.Job('restore', engine.RESTORE_STEPS), self.game, bk, False, no_procs,
                           documents=self.docs)
        self.assertEqual(self.saves_digest(), before)

    def test_side_file_without_documents_never_touched(self):
        before = self.saves_digest()
        s = sel(display=True)
        s['subs']['display_apply'] = True
        job = self.run_job(s, documents='')
        self.assertEqual(self.saves_digest(), before)
        self.assertNotIn('side_files', B.load_manifest(job.result['backup']))


class MinimapBakeProcess(unittest.TestCase):
    """default_minimap runs a bake_minimap.py as a child process and turns its lines into progress (a stand-in script
    in a temp folder, never the real bake)."""
    def test_progress_from_the_bake_lines(self):
        tmp = tempfile.mkdtemp()
        old = repo.HUD_TOOLS
        try:
            tools = os.path.join(tmp, 'tools')
            os.makedirs(tools)
            with open(os.path.join(tools, 'bake_minimap.py'), 'w') as f:
                f.write('import sys\n'
                        'a = sys.argv\n'
                        'assert a[1:3] == ["--game", "G"] and a[3] == "--out", a\n'
                        'print("map02: reading collision")\n'
                        'print("map02: 10x10 px at 1.00 m/px, x 0..1 z 0..1 (read 1 s)")\n'
                        'import time; time.sleep(1.5)\n'
                        'print("map03: up to date")\n'
                        'print("done: 2 map(s) listed in X (1 s)")\n')
            repo.HUD_TOOLS = tools
            import time
            lines, prog, when = [], [], []
            r = engine.default_minimap('G', os.path.join(tmp, 'out'), lines.append,
                                       lambda d, t: (prog.append((d, t)), when.append(time.time())))
            self.assertEqual(r['maps'], 2)
            self.assertEqual(prog, [(0, 9), (1, 9), (2, 9)])
            self.assertGreater(when[2] - when[1], 1.0)          # lines arrive as printed (unbuffered child), not at exit
            self.assertIn('minimap: map03: up to date', lines)
            with open(os.path.join(tools, 'bake_minimap.py'), 'w') as f:
                f.write('import sys\nprint("boom")\nsys.exit(3)\n')
            with self.assertRaises(engine.StepError):
                engine.default_minimap('G', os.path.join(tmp, 'out'), lines.append, lambda d, t: None)
        finally:
            repo.HUD_TOOLS = old
            shutil.rmtree(tmp, ignore_errors=True)

    def _fake_bake(self, game, out, fail=()):
        """the real bake_minimap.main with the game files and the native baker replaced; -> the maps it baked"""
        bm = repo.bake_minimap()
        baked = []

        class Fs:
            order = ['blocks.naz']

            def __init__(self, *a, **k):
                pass

            def locate(self, e):
                raise KeyError(e)

        class Geo:
            tris = [(0.0,) * 10] * 200
            marks = []

            def __init__(self, *a, **k):
                pass

        def run(exe, mmb, pack, occ):
            k = os.path.basename(pack)[:-4]
            if k in fail:
                raise RuntimeError('no')
            baked.append(k)
            for p in (pack, occ):
                with open(p, 'wb') as f:
                    f.write(b'x')
            return dict(x0=0.0, z0=0.0, x1=9.0, z1=9.0, w=36, h=36, tiles=1, bytes=1)
        saved = bm.MapFS, bm.mg.MapGeo, bm.mg.list_maps, bm.run_baker, bm.baker_path
        bm.MapFS, bm.mg.MapGeo, bm.mg.list_maps, bm.run_baker, bm.baker_path = Fs, Geo, (lambda fs: {'mapA': ['a'], 'mapB': ['b']}), run, (lambda: 'x.exe')
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bm.main(['--game', game, '--out', out]), 0)
        finally:
            bm.MapFS, bm.mg.MapGeo, bm.mg.list_maps, bm.run_baker, bm.baker_path = saved
        return baked

    def test_bake_version_change_bakes_again_and_a_failure_keeps_the_old_map(self):
        tmp = tempfile.mkdtemp()
        try:
            out = os.path.join(tmp, 'mm')
            os.makedirs(out)
            maps = {k: dict(file=k + '.png', source='old', x0=0, z0=0, x1=9, z1=9, w=9, h=9) for k in ('mapA', 'mapB')}
            with open(os.path.join(out, 'bake.json'), 'w') as f:
                json.dump(dict(version=2, maps=maps), f)
            for k in maps:
                with open(os.path.join(out, k + '.png'), 'wb') as f:
                    f.write(b'old')
            self.assertEqual(self._fake_bake(tmp, out, fail=('mapB',)), ['mapA'])
            names = sorted(os.listdir(out))
            self.assertIn('mapA.mmp', names)
            self.assertNotIn('mapA.png', names)                     # replaced: the old picture is gone
            self.assertIn('mapB.png', names)                         # failed: the old map stays
            txt = open(os.path.join(out, 'minimap.txt')).read()
            self.assertIn('mapA.mmp', txt)
            self.assertIn('mapB.png', txt)
            self.assertEqual(self._fake_bake(tmp, out), ['mapB'])    # the next install finishes the job
            self.assertEqual(self._fake_bake(tmp, out), [])          # and then nothing is left to do
            self.assertNotIn('mapB.png', os.listdir(out))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_bake_filesystem_lines(self):
        """a configpc.txt with no filesystem line, one that leaves the blocks out, a broken archive: the bake's reader
        copes with each (the strict GameFS of the data builder is unchanged)"""
        bm = repo.bake_minimap()
        tmp = tempfile.mkdtemp()
        try:
            with open(os.path.join(tmp, 'blocks.naz'), 'wb') as f:
                f.write(b'not an archive')
            for cfg in ('Fullscreen = 1\n', 'filesystem = sounds.naz\n', 'filesystem =\n'):
                with open(os.path.join(tmp, 'configpc.txt'), 'w') as f:
                    f.write(cfg)
                fs = bm.MapFS(tmp, skip=['todgameplay_data.zip'])
                self.assertIn('blocks.naz', [n.lower() for n in fs.order], cfg)
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertIsNone(fs.archive('blocks.naz'))
                with self.assertRaises(KeyError):
                    fs.locate('nothing')
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_real_bake_arguments(self):
        """the real tool takes --game and --out and defaults to <game>\\TODGameplay_minimap"""
        src = open(os.path.join(repo.HUD_TOOLS, 'bake_minimap.py'), encoding='utf-8').read()
        self.assertIn("ap.add_argument('--game', required=True)", src)
        self.assertIn("ap.add_argument('--out')", src)
        self.assertIn("os.path.join(game, 'TODGameplay_minimap')", src)


class SuaveSong(unittest.TestCase):
    """The Suave card's song file: the plan, the checks, and the install order (the song is cut before anything is
    written, a refused song stops the install untouched, no song = Suave without music)."""
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = make_game(self.tmp)
        self.root = os.path.join(self.tmp, 'Gameplay+ Backups')
        self.song = os.path.join(self.tmp, 'my song.mp3')
        with open(self.song, 'wb') as f:
            f.write(b'not really audio: the song import is faked here')
        self.cache = os.path.join(self.tmp, 'cache')
        # the audio reader (soundfile) is not needed here: the song import itself is faked
        import types
        from unittest import mock
        p = mock.patch.object(repo, 'song_import', lambda: types.SimpleNamespace(_sf=lambda: None))
        p.start()
        self.addCleanup(p.stop)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def suave_sel(self, song=None):
        s = all_off()
        s['groups']['suave'] = True
        if song is not None:
            s['subs']['suave_song'] = song
        return s

    def test_plan_without_and_with_a_song(self):
        shipped = GS.read_text(repo.SHIPPED_INI)
        p = P.make_plan(self.suave_sel(), shipped)
        self.assertEqual(p['data']['suave_music'], 'off')
        self.assertEqual(p['song'], '')
        ini = {(a, b): v for a, b, v, m in p['ini']}
        self.assertEqual(ini[('suave', 'music')], '0')
        self.assertEqual(ini[('suave', 'enabled')], '1')
        self.assertTrue(any('No music' in e['text'] for e in p['effects']['suave']))
        p = P.make_plan(self.suave_sel('  %s ' % self.song), shipped)
        self.assertEqual(p['song'], self.song)
        self.assertNotEqual(p['data'].get('suave_music'), 'off')           # the builder default: music on
        ini = {(a, b): v for a, b, v, m in p['ini']}
        self.assertEqual(ini[('suave', 'music')], '1')
        # a song chosen while the card is off: ignored (nothing imported, nothing set)
        s = all_off()
        s['subs']['suave_song'] = self.song
        p = P.make_plan(s, shipped)
        self.assertEqual(p['song'], '')
        self.assertNotEqual(p['data'].get('suave_music'), 'off')
        # the path survives the selection round trip, a non-string does not
        self.assertEqual(catalog.normalize(self.suave_sel(self.song))['subs']['suave_song'], self.song)
        self.assertEqual(catalog.normalize(self.suave_sel(5))['subs']['suave_song'], '')

    def test_missing_song_blocks_the_install(self):
        c = engine.check(self.game, self.suave_sel(os.path.join(self.tmp, 'gone.mp3')), self.root, 'full', False, no_procs)
        self.assertTrue(any('song file' in p and 'not found' in p for p in c['problems']), c['problems'])
        c = engine.check(self.game, self.suave_sel(), self.root, 'full', False, no_procs)
        self.assertFalse(any('song' in p for p in c['problems']), c['problems'])

    def run_job(self, s, song_fn, build_fn=fake_build):
        job = engine.Job('install', engine.INSTALL_STEPS)
        engine.run_install(job, self.game, s, self.root, 'full', False, no_procs, build_fn, fake_prefetch,
                           song_fn=song_fn, music_cache=self.cache)
        return job

    def test_song_is_cut_first_and_the_builder_reads_the_cache(self):
        calls, seen = [], {}

        def song_fn(path, out_dir, log):
            calls.append((path, out_dir, os.path.exists(self.root)))        # no backup folder yet
            os.makedirs(out_dir, exist_ok=True)
            m = os.path.join(out_dir, 'manifest.json')
            with open(m, 'w') as f:
                f.write('{}')
            return m

        def build_fn(game_dir, data, out_zip, report_dir, log, suave_manifest=None):
            seen['manifest'] = suave_manifest
            seen['data'] = dict(data)
            fake_build(game_dir, data, out_zip, report_dir, log)

        job = self.run_job(self.suave_sel(self.song), song_fn, build_fn)
        self.assertEqual(job.state, 'done', job.error)
        self.assertEqual(calls, [(self.song, self.cache, False)])
        self.assertEqual(seen['manifest'], os.path.join(self.cache, 'manifest.json'))
        self.assertNotEqual(seen['data'].get('suave_music'), 'off')
        self.assertEqual(GS.ini_value(GS.read_text(os.path.join(self.game, P.INI)), 'suave', 'music'), '1')
        by = {s['id']: s for s in job.steps}
        self.assertEqual(by['music']['status'], 'done')
        # nothing of the song or its segments lands in the game folder
        for root, _, files in os.walk(self.game):
            for n in files:
                self.assertNotIn('suave_mus', n.lower())
                self.assertFalse(n.lower().endswith(('.mp3', '.ogg', '.flac')), n)

    def test_refused_song_stops_before_anything_is_touched(self):
        before = B.folder_digest(self.game)

        def song_fn(path, out_dir, log):
            raise engine.StepError('Your song file was refused: it does not look like the song.')

        job = self.run_job(self.suave_sel(self.song), song_fn)
        self.assertEqual(job.state, 'failed')
        self.assertIn('refused', job.error)
        self.assertEqual(B.folder_digest(self.game), before)
        self.assertFalse(os.path.exists(self.root), 'no backup was made for a refused install')

    def test_no_song_installs_without_music(self):
        def song_fn(path, out_dir, log):                  # must not run
            raise AssertionError('no song given')

        seen = {}

        def build_fn(game_dir, data, out_zip, report_dir, log, **kw):
            seen.update(data=dict(data), kw=kw)
            fake_build(game_dir, data, out_zip, report_dir, log)

        job = self.run_job(self.suave_sel(), song_fn, build_fn)
        self.assertEqual(job.state, 'done', job.error)
        self.assertEqual(seen['data']['suave_music'], 'off')
        self.assertEqual(seen['kw'], {})
        self.assertEqual(GS.ini_value(GS.read_text(os.path.join(self.game, P.INI)), 'suave', 'music'), '0')
        self.assertEqual(GS.ini_value(GS.read_text(os.path.join(self.game, P.INI)), 'suave', 'enabled'), '1')
        self.assertEqual({s['id']: s['status'] for s in job.steps}['music'], 'skipped')

    def test_suave_off_skips_the_step(self):
        job = self.run_job(all_off(), lambda *a: self.fail('no song'))
        self.assertEqual(job.state, 'done', job.error)
        self.assertEqual({s['id']: s['status'] for s in job.steps}['music'], 'skipped')


def tearDownModule():
    shutil.rmtree(FAKE_DOCS_ROOT, ignore_errors=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
