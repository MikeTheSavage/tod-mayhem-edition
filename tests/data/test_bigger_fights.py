"""Static verification of the `bigger_fights` data option against the GOG game's blocks (read only).

The bigger pools in overdose.main, the dormant copies and the dormant vehicles of the mission blocks: what the save-breaking
option adds, that nothing in a copy reaches outside it, and that no block with buddies or an escort gets a vehicle.
"""
import os
import sys
import unittest

from common import DATA, GOG, HAVE_GOG

sys.path.insert(0, os.path.join(DATA, 'lib'))
import todblock as tb
import todfrag as tf
import fragedit as fe
import fragtool as ft
from gameplay import builder, bigger_fights as bf, options as opt
from gameplay.gamefs import GameFS


@unittest.skipUnless(HAVE_GOG, 'needs the GOG game')
class BiggerFights(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        cls.rep, cls.entries = builder.build_blocks(cls.ctx, {'bigger_fights': 'on'}, log=lambda *a: None)
        cls.by_name = dict(cls.entries)
        cls.T = ft.types()

    def docs(self, entry):
        b = tb.Block(self.by_name[entry], os.path.splitext(entry)[1])
        frs, _ = ft.fragments(b)
        return {nm: fe.FragDoc(b.payloads[k], self.T, nm) for k, nm in frs}

    def names(self, entry):
        out_ = set()
        for d in self.docs(entry).values():
            for n in d.created_ids():
                x = d.get(n, 'name')
                if x:
                    out_.add(x)
        return out_

    def test_pools(self):
        names = self.names(opt.MAIN)
        for i in range(10, 15):
            self.assertIn('AICharacter%d' % i, names)

    def test_mission_report(self):
        ms = self.rep['bigger_fights']['missions']
        self.assertGreater(len(ms), 30)
        for m in ms:
            if m['friendly']:
                self.assertEqual((m['tier1'], m['tier2']), (0, 0), m['entry'])
                # the friends of an intermission only stand around mission starts: it is free roam, its cars stay
                if not os.path.basename(m['entry']).lower().startswith('intermission_'):
                    self.assertEqual(m['squads'], 0, m['entry'])
            for w in bf.NO_SQUAD_WORDS:
                if w in os.path.basename(m['entry']).lower():
                    self.assertEqual(m['squads'], 0, m['entry'])

    def test_kill_missions_get_copies_that_are_no_targets(self):
        e = [x for x in self.by_name if 'subm_19_harb_tower_kill' in x][0]
        seen = 0
        for d in self.docs(e).values():
            for n in d.created_ids():
                nm = d.get(n, 'name') or ''
                if nm.startswith('BF') and nm.endswith('e'):
                    seen += 1
                    self.assertFalse(d.get(n, 'MissionObjectiveKill'))
                    self.assertFalse(d.get(n, 'IsVIP'))
                    self.assertTrue((d.get(n, 'flags') or 0) & 1, 'a copy starts dormant')
        self.assertGreaterEqual(seen, 15)

    def test_no_copy_or_gunman_is_a_target_anywhere(self):
        """in every block: a copy or a car's gunman is never a kill objective or a VIP, and copies and cars start dormant"""
        seen = 0
        for e in self.by_name:
            if not e.endswith('.mission'):
                continue
            for d in self.docs(e).values():
                for n in d.created_ids():
                    nm = d.get(n, 'name') or ''
                    if not nm.startswith('BF'):
                        continue
                    if nm[:3] == 'BFG' or nm.endswith('e'):
                        seen += 1
                        self.assertFalse(d.get(n, 'MissionObjectiveKill'), (e, nm))
                        # the police assault's gunmen are VIPs in the game's own data, the copies never: psycho moves and elite
                        # drops skip a VIP, so no car gunman can ever leave a drop
                        self.assertEqual(bool(d.get(n, 'IsVIP')), nm[:3] == 'BFG', (e, nm))
                        if nm[:3] != 'BFG':
                            self.assertTrue((d.get(n, 'flags') or 0) & 1, (e, nm))
                    if nm[:3] == 'BFS':
                        self.assertTrue((d.get(n, 'flags') or 0) & 1, (e, nm))
        self.assertGreater(seen, 500)

    def test_goodie_pool_is_not_touched(self):
        """elite drops borrow from the five goodie props of DefaultGoodieRessources: the bigger pools leave that fragment alone"""
        new = tb.Block(self.by_name[opt.MAIN], '.main')
        old = tb.Block(self.ctx.fs.read(opt.MAIN)[0], '.main')
        frs, _ = ft.fragments(new)
        hit = [k for k, nm in frs if nm.lower().endswith('/defaultgoodieressources.fragment')]
        self.assertEqual(len(hit), 1)
        self.assertEqual(new.payloads[hit[0]], old.payloads[hit[0]])

    def test_vehicles_dormant_with_gunmen(self):
        e = [x for x in self.by_name if 'mm075_hacienda' in x][0]
        folders = gunmen = cars = 0
        for d in self.docs(e).values():
            for n in d.created_ids():
                nm = d.get(n, 'name') or ''
                if nm[:3] == 'BFS':
                    folders += 1
                    self.assertTrue((d.get(n, 'flags') or 0) & 1)
                elif nm[:3] == 'BFV':
                    cars += 1
                elif nm[:3] == 'BFG':
                    gunmen += 1
                    self.assertEqual(d.get(n, 'FactionID'), 9)
        self.assertEqual((folders, cars, gunmen), (bf.SQUADS_PER_BLOCK, bf.SQUADS_PER_BLOCK, bf.SQUADS_PER_BLOCK * bf.GUNMEN_PER_CAR))


if __name__ == '__main__':
    unittest.main()
