"""Static verification of the menu helper (gameplay/menus.py) and the `difficulty_menu` option against the
GOG game's blocks (read only): the marker set op on the DIFFICULTY LEVEL entry, the Menu text #77 replacement per
language block (alone and on top of suave's uk edits), and the refusals."""
import os
import unittest

from common import GOG, HAVE_GOG

from gameplay import builder, menus as mn
from gameplay.gamefs import GameFS
import texlib as tl
import todblock as tb


def menu_pairs(raw):
    b = tb.Block(raw, '.main')
    info = bytes(b.info)
    st = tb.record_starts(info, b.n)
    t = [t for i, R, E, t in tl.find_text_records(info, st) if t.name == mn.MENU_TXT][0]
    return t.pairs()


@unittest.skipUnless(HAVE_GOG, 'needs the GOG game')
class DifficultyMenu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        cls.rep, cls.entries = builder.build_blocks(cls.ctx, {'difficulty_menu': 'on'}, log=lambda *a: None)

    def test_marker(self):
        ch = sorted((c['path'], c['prop'], c['new']) for c in self.rep['changes'] if c['option'] == 'difficulty_menu')
        self.assertEqual(ch, [(mn.SETTINGS + '/difficulty', 'OptionValueType4', mn.BRUTAL_ORDINAL),
                              (mn.SETTINGS + '/difficulty', 'OptionValueType5', mn.PSYCHO_ORDINAL)])
        # the entry keeps three offered values without the DLL: ValueString4 untouched
        vals = mn.entry_values(self.ctx, mn.SETTINGS + '/difficulty')
        self.assertEqual([v for v, _ in vals[:3]], [160, 161, 162])
        self.assertIn(vals[3][0], (None, 0))
        self.assertIn(vals[4][0], (None, 0))

    def test_uk_text(self):
        uk = dict(self.entries)[mn.lang_block('uk')]
        p_new, p_old = menu_pairs(uk), menu_pairs(self.ctx.block(mn.lang_block('uk'))['raw'])
        self.assertEqual(len(p_new), len(p_old))
        self.assertEqual(tl.codes_to_str(p_new[77][1]), 'BRUTAL')
        self.assertEqual(tl.codes_to_str(p_new[77][0]), 'FILL_IN')
        self.assertEqual(tl.codes_to_str(p_new[101][1]), 'PSYCHO')
        self.assertEqual(sum(1 for a, b in zip(p_new, p_old) if a != b), 2)
        self.assertNotIn(mn.lang_block('de'), dict(self.entries))   # level on = uk only

    def test_all_languages(self):
        rep, ent = builder.build_blocks(self.ctx, {'difficulty_menu': 'on_all_languages'}, log=lambda *a: None)
        d = dict(ent)
        want = {'uk': 'BRUTAL', 'de': 'BRUTAL', 'fr': 'BRUTAL', 'es': 'BRUTAL', 'it': 'BRUTALE'}
        for lang, txt in want.items():
            self.assertEqual(tl.codes_to_str(menu_pairs(d[mn.lang_block(lang)])[77][1]), txt, lang)

    def test_with_suave(self):
        from gameplay import suave_data as sd
        if not os.path.exists(sd.MANIFEST):
            self.skipTest('no suave/audio/build')
        rep, ent = builder.build_blocks(self.ctx, {'difficulty_menu': 'on', 'suave': 'on_slot10'}, log=lambda *a: None)
        uk = dict(ent)[mn.lang_block('uk')]
        self.assertEqual(tl.codes_to_str(menu_pairs(uk)[77][1]), 'BRUTAL')
        b = tb.Block(uk, '.main')
        self.assertEqual(b.n, rep['suave']['records_after'])          # suave's 4 stream records kept
        self.assertEqual([e for e, _ in ent].count(mn.lang_block('uk')), 1)

    def test_refusals(self):
        with self.assertRaises(builder.BuildError):
            builder.build_blocks(self.ctx, {'difficulty_menu': 'on', 'difficulty': 'brutal'}, log=lambda *a: None)
        rep, ent = builder.build_blocks(self.ctx, {'difficulty_menu': 'off'}, log=lambda *a: None)
        self.assertEqual(ent, [])

    def test_text_edit_guard(self):
        raw = self.ctx.block(mn.lang_block('uk'))['raw']
        with self.assertRaises(mn.MenuError):
            mn.apply_texts(raw, 'uk', [mn.MenuText(77, 'NOT_THE_KEY', 'FILL_IN', {'*': 'X'})])
        with self.assertRaises(mn.MenuError):
            mn.apply_texts(raw, 'uk', [mn.MenuText(400, 'X', 'X', {'*': 'X'})])
        same, rep = mn.apply_texts(raw, 'uk', [mn.MenuText(77, 'FILL_IN', 'FILL_IN', {'*': 'FILL_IN'})])
        self.assertIs(same, raw)


@unittest.skipUnless(HAVE_GOG, 'needs the GOG game')
class ControllerAndDifficultyMenu(unittest.TestCase):
    """`controller = menu` and `difficulty_menu` together (merge check): both edit SETTINGS records of HUD.fragment,
    never the same record / property; the screen-resolution hint moves below the one-row-longer list."""

    @classmethod
    def setUpClass(cls):
        cls.ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        cls.rep, cls.entries = builder.build_blocks(cls.ctx, {'controller': 'menu', 'difficulty_menu': 'on'},
                                                    log=lambda *a: None)

    def test_both_sets_of_ops(self):
        ch = sorted((c['option'], c['path'], c['prop']) for c in self.rep['changes']
                    if c['option'] in ('controller', 'difficulty_menu'))
        self.assertEqual(ch, [('controller', 'InterMission_HUD/GfxFolder/ScreenResDetailTextTB', 'pos'),
                              ('controller', mn.SETTINGS + '/vibration', 'NotPcOption'),
                              ('difficulty_menu', mn.SETTINGS + '/difficulty', 'OptionValueType4'),
                              ('difficulty_menu', mn.SETTINGS + '/difficulty', 'OptionValueType5')])
        nodes = {}
        for c in self.rep['changes']:
            nodes.setdefault((c['block'], c['fragment'], c['node'], c['prop']), []).append(c['option'])
        self.assertTrue(all(len(v) == 1 for v in nodes.values()), nodes)   # no record / property edited twice

    def test_hint_moved(self):
        c = [c for c in self.rep['changes'] if c['option'] == 'controller' and c['prop'] == 'pos'][0]
        self.assertEqual(c['old'][1], 450.0)
        self.assertEqual(c['new'][1], 466.0)
        self.assertEqual((c['new'][0], c['new'][2]), (c['old'][0], c['old'][2]))
        self.assertEqual(tl.codes_to_str(menu_pairs(dict(self.entries)[mn.lang_block('uk')])[77][1]), 'BRUTAL')


if __name__ == '__main__':
    unittest.main()
