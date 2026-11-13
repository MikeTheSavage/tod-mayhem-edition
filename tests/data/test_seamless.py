"""Static verification of the `seamless` data option against the GOG game's blocks (read only).

What the option promises: only set ops (Teleport_to of the corridor loaders, one root pos per corridor), every create
record and node index unchanged (saves), the moved box exactly where the rule puts it, the box order along each
corridor 'walk' with the T1 gap, and it combines with the other options (driving, difficulty) as the union of the
single-option diffs. The full transition catalogue re-run and the ground check are seamless/tools/catalog_rerun.py
and seamless/tools/verify_ground.py (they need the research tools on G:).
"""
import os
import unittest

from common import GOG, HAVE_GOG

from gameplay import builder, seamless as sm
from gameplay.gamefs import GameFS


@unittest.skipUnless(HAVE_GOG, 'needs the GOG game')
class Seamless(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        cls.rep, cls.entries = builder.build_blocks(cls.ctx, {'seamless': 'on'}, log=lambda *a: None)

    def test_only_planned_sets(self):
        ch = self.rep['changes']
        self.assertEqual(sorted(c['prop'] for c in ch), ['Teleport_to'] * 5 + ['pos'] * 2)
        for c in ch:
            if c['prop'] == 'Teleport_to':
                self.assertEqual(c['new'], '')
                self.assertTrue(c['old'].startswith('Teleport_'))
        blocks = sorted(e.split('/')[-1] for e, _ in self.entries)
        self.assertEqual(blocks, sorted(['submap01_map02_lostoros_downtown.submap', 'submap01_map04_lostoros_train_harbour.submap',
                                         'submap02_map02_lostoros_downtown.submap', 'submap02_map03_lostoros_industrial.submap',
                                         'submap04_map03_lostoros_industrial.submap']))

    def test_create_records_and_indices(self):
        # the builder already refuses a changed create record; checked again here independently, every fragment
        T = self.ctx.T
        for entry, data in self.entries:
            old = self.ctx.block(entry)
            nb = builder.tb.Block(data, os.path.splitext(entry)[1])
            self.assertEqual(old['block'].n, nb.n)
            self.assertEqual(bytes(old['block'].info), bytes(nb.info))
            for k, nm in old['frs']:
                a, b = old['block'].payloads[k], nb.payloads[k]
                if a == b:
                    continue
                self.assertEqual(builder.creates_of(a, T), builder.creates_of(b, T), nm)

    def test_check_report(self):
        s = self.rep['seamless']
        self.assertEqual(s['result'], 'PASS')
        self.assertTrue(all(st == 'PASS' for st, _ in s['checks']))
        by = {c['key']: c for c in s['corridors']}
        self.assertEqual(set(by), {'bridge', 'tunnel'})
        for c in by.values():
            self.assertEqual(c['order_before']['state'], 'wrong')
            self.assertEqual(c['order_after']['state'], 'walk')
            self.assertAlmostEqual(c['order_after']['gap'], sm.GAP, places=2)
        # the bridge reproduces research package T1 (Downtown's box from z -329 to z -540, x and y kept) to 5 cm
        b = by['bridge']['moved']
        self.assertAlmostEqual(b['new'][2], -540.0, delta=0.05)
        self.assertAlmostEqual(b['new'][0], b['old'][0], delta=0.05)
        self.assertEqual(b['new'][1], b['old'][1])
        self.assertEqual(by['bridge']['teleports_cleared'], 2)
        self.assertEqual(by['tunnel']['teleports_cleared'], 3)
        # the tunnel box moves west along the tube (x and z decrease) and stays at its height
        t = by['tunnel']['moved']
        self.assertLess(t['new'][0], -450.0)
        self.assertLess(t['new'][2], t['old'][2])
        self.assertEqual(t['new'][1], t['old'][1])

    def test_harbour_block_equals_t1(self):
        # T1's Harbour 01 block (only Teleport_to emptied) has sha256 4a91e137a3fd2b28...; same op, same bytes
        h = [b for b in self.rep['blocks'] if b['entry'].endswith('submap01_map04_lostoros_train_harbour.submap')][0]
        self.assertTrue(h['out_sha256'].startswith('4a91e137a3fd2b28'))

    def test_classification(self):
        vb = {}
        for entry, data in self.entries:
            b = builder.tb.Block(data, os.path.splitext(entry)[1])
            frs, _ = builder.ft.fragments(b)
            vb[entry.split('/')[-1]] = sm.scan_doc_volumes(entry, b, frs, self.ctx.T, builder.fe, builder.ft)
        rows = sm.classify(vb)
        # the corridor volumes = the ones that teleported in the base blocks
        tele = set()
        for entry, _ in self.entries:
            info = self.ctx.block(entry)
            for v in sm.scan_doc_volumes(entry, info['block'], info['frs'], self.ctx.T, builder.fe, builder.ft):
                if any(a['teleport'] for a in v['actions']):
                    tele.add((entry.split('/')[-1], v['frag'], v['root']))
        self.assertEqual(len(tele), 5)
        corr = {(r['block'], r['frag'], r['root']): r['cls'] for r in rows if (r['block'], r['frag'], r['root']) in tele}
        self.assertEqual(len(corr), 5)
        # Industrial 02 has no return loader in Downtown 02 (Downtown returns to its overlapping twin Industrial 04, as in
        # vanilla), so the research rule calls it ONEWAY; the other four are WALK
        expect = {k: ('ONEWAY' if k[0] == 'submap02_map03_lostoros_industrial.submap' else 'WALK') for k in corr}
        self.assertEqual(corr, expect)
        self.assertFalse(any(r['cls'] == 'TELEPORT' for r in rows))

    def test_off_and_combined(self):
        rep, ent = builder.build_blocks(self.ctx, {'seamless': 'off'}, log=lambda *a: None)
        self.assertEqual(ent, [])
        sel = {'driving': 'better_feel', 'difficulty': 'brutal', 'seamless': 'on'}
        rep2, ent2 = builder.build_blocks(self.ctx, sel, log=lambda *a: None)
        single = {}
        for s in ({'driving': 'better_feel', 'difficulty': 'brutal'}, {'seamless': 'on'}):
            for e, d in builder.build_blocks(self.ctx, s, log=lambda *a: None)[1]:
                self.assertNotIn(e, single)
                single[e] = d
        self.assertEqual(dict(ent2), single)          # the combined build = the union of the single builds


@unittest.skipUnless(HAVE_GOG, 'needs the GOG game')
class SeamlessNoMarkers(unittest.TestCase):
    """seamless = on_nomarkers: the walk borders' loader beams and area signs get zero size and opacity 0; the farm
    teleport, ZAP stops and mission loaders keep theirs; still set ops only."""
    @classmethod
    def setUpClass(cls):
        cls.ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        cls.rep, cls.entries = builder.build_blocks(cls.ctx, {'seamless': 'on_nomarkers'}, log=lambda *a: None)

    def test_markers(self):
        ch = self.rep['changes']
        size = [c for c in ch if c['prop'] == 'sprite_size']
        opac = [c for c in ch if c['prop'] == 'opacity']
        self.assertEqual(len(size), 103)                 # 52 beams + 51 signs of the walk borders
        self.assertEqual(len(opac), 103)
        self.assertTrue(all(c['new'] == [0.0, 0.0, 1.0] for c in size))
        self.assertTrue(all(c['new'] == 0.0 and c['old'] > 0 for c in opac))
        blocks = {e.split('/')[-1] for e, _ in self.entries}
        self.assertFalse(any('map06' in b or b.endswith('.mission') for b in blocks))   # farm teleport, missions kept
        self.assertEqual(sorted({c['prop'] for c in ch}), ['Teleport_to', 'opacity', 'pos', 'sprite_size'])
        # every hidden sprite is a beam / sign, parented to a loader root or a MAPLOADERS folder (never a ZAP box)
        for entry, data in self.entries:
            old = self.ctx.block(entry)
            for c in [x for x in size if x['block'] == entry.split('/')[-1]]:
                d = self.ctx.doc(entry, c['fragment'])
                n = int(c['node'], 16)
                self.assertTrue(sm.is_marker(d, n))
                ptype = d.type_of(d.get(n, 'parent') or 0) or ''
                self.assertTrue(ptype.startswith('Trigger_Root(') or ptype == 'Folder', ptype)
            nb = builder.tb.Block(data, os.path.splitext(entry)[1])
            for k, nm in old['frs']:
                if old['block'].payloads[k] != nb.payloads[k]:
                    self.assertEqual(builder.creates_of(old['block'].payloads[k], self.ctx.T),
                                     builder.creates_of(nb.payloads[k], self.ctx.T), nm)
        self.assertEqual(self.rep['seamless']['result'], 'PASS')


if __name__ == '__main__':
    unittest.main()
