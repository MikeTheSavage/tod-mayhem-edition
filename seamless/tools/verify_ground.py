"""verify_ground.py - ground under the player at every converted border, in BOTH blocks (static, read only)

For each corridor of data/gameplay/seamless.py: the plan (moved box, other box) is computed from the game's blocks,
then points are sampled along the line through the two box centres, from 15 m before the other box to the far side
of the moved box (2 m steps, and +-3 m to each side). At every point the walkable surfaces of both block sets (map + submap:
what the engine holds before and after the swap) are compared with ground_probe.py. PASS when, at both swap points
(the moved box's near face and the other box's near face) and along the whole stretch between them, both block sets
have a surface within 0.25 m of each other (the same deck / tunnel floor placed in both blocks).

  python -I verify_ground.py GAME_DIR [--json out.json]
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ground_probe as gp                       # noqa: E402  (puts data/ and data/lib on sys.path)
import fragtool as ft                           # noqa: E402
import fragedit as fe                           # noqa: E402
from gameplay import seamless as sm             # noqa: E402

SETS = {   # block set = map + submap held after a swap into that submap
    'submap01_map02_lostoros_downtown.submap': 'mainmap_map02_lostoros_downtown.map',
    'submap01_map04_lostoros_train_harbour.submap': 'mainmap_map04_lostoros_train_harbour.map',
    'submap02_map02_lostoros_downtown.submap': 'mainmap_map02_lostoros_downtown.map',
    'submap04_map03_lostoros_industrial.submap': 'mainmap_map03_lostoros_industrial.map',
    'submap02_map03_lostoros_industrial.submap': 'mainmap_map03_lostoros_industrial.map',
}
TOL = 0.25


def main(game_dir, out_json=None):
    fs = gp.GameFS(game_dir, skip=['todgameplay_data.zip'])
    T = ft.types()
    vols_cache = {}

    def vols_of(blk):
        if blk not in vols_cache:
            e = sm.SHARED + blk
            raw = fs.read(e)[0]
            b = gp.tb.Block(raw, '.submap')
            frs, _ = ft.fragments(b)
            vols_cache[blk] = sm.scan_doc_volumes(e, b, frs, T, fe, ft)
        return vols_cache[blk]

    res, ok_all = [], True
    for c in sm.CORRIDORS:
        a, b, twins, d, t, new = sm.plan_corridor(c, vols_of)
        blocks = [c['move']['block'], c['other']['block']] + [x['block'] for x in c['twins']]
        geos = {blk: gp.BlockGeo(fs, [SETS[blk], blk], lambda *x: None) for blk in blocks}
        # line coordinate s along d, origin at the other box's centre
        moved = dict(a, pos=new)
        ea, eb = sm.extent(moved, d), sm.extent(b, d)
        s_a = (new[0] - b['pos'][0]) * d[0] + (new[2] - b['pos'][2]) * d[1]
        # both blocks need ground from before the other box to the far side of the moved box: a player of the moved
        # box's block cannot get past that box without swapping (beyond it is the other block's own territory)
        s_lo, s_hi = -eb - 15.0, s_a + ea
        # swap points: entering the moved box from the other side (s_a - ea) and the other box from beyond (eb)
        swap = {'swap into %s (moved box face)' % c['other']['block']: s_a - ea,
                'swap into %s (other box face)' % c['move']['block']: eb}
        lat = (-d[1], d[0])
        rect_pts = []
        s = s_lo
        while s <= s_hi + 1e-6:
            for o in (-3.0, 0.0, 3.0):
                rect_pts.append((s, o))
            s += 2.0
        for s_, o in [(v, 0.0) for v in swap.values()]:
            rect_pts.append((s_, o))
        xs = [b['pos'][0] + d[0] * s_ + lat[0] * o for s_, o in rect_pts]
        zs = [b['pos'][2] + d[1] * s_ + lat[1] * o for s_, o in rect_pts]
        rect = (min(xs) - 5, max(xs) + 5, min(zs) - 5, max(zs) + 5)
        y0 = min(a['pos'][1], b['pos'][1]) - 15
        y1 = max(a['pos'][1], b['pos'][1]) + 15
        crops = {k: g.crop(rect, (y0, y1)) for k, g in geos.items()}
        rows, missing, mismatch = [], 0, 0
        for (s_, o), x, z in zip(rect_pts, xs, zs):
            hs = {k: g.hits(x, z, y0, y1) for k, g in crops.items()}
            ref = hs[c['move']['block']]
            common = True
            for k, h in hs.items():
                if k == c['move']['block']:
                    continue
                if not any(abs(p - q) <= TOL for p in ref for q in h):
                    common = False
            if not all(hs.values()):
                missing += 1
            elif not common:
                mismatch += 1
            rows.append(dict(s=round(s_, 1), lateral=o, x=round(x, 2), z=round(z, 2), heights=hs, common=common))
        swaps = []
        for lab, s_ in swap.items():
            x, z = b['pos'][0] + d[0] * s_, b['pos'][2] + d[1] * s_
            hs = {k: g.hits(x, z, y0, y1) for k, g in crops.items()}
            ref = hs[c['move']['block']]
            common = all(any(abs(p - q) <= TOL for p in ref for q in h) for k, h in hs.items() if k != c['move']['block'])
            swaps.append(dict(label=lab, x=round(x, 2), z=round(z, 2), heights=hs, common=common and all(hs.values())))
        ok = missing == 0 and mismatch == 0 and all(sw['common'] for sw in swaps)
        ok_all &= ok
        for r in rows:
            if not all(r['heights'].values()) or not r['common']:
                print('    no common ground at s %.1f (lateral %+.0f) (%.1f, %.1f): %s' % (
                    r['s'], r['lateral'], r['x'], r['z'], '; '.join('%s %s' % (k.split('_map')[0] + '_' + k.split('_')[1], v)
                                                                    for k, v in r['heights'].items())))
        res.append(dict(corridor=c['key'], label=c['label'], points=len(rows), missing=missing, mismatch=mismatch,
                        swaps=swaps, result='PASS' if ok else 'FAIL', stretch_m=round(s_hi - s_lo, 1)))
        print('%s %s: %d points over %.0f m (centre line +-3 m), missing ground %d, surfaces differ %d' % (
            'PASS' if ok else 'FAIL', c['label'], len(rows), s_hi - s_lo, missing, mismatch))
        for sw in swaps:
            print('    %s at (%.1f, %.1f): %s -> %s' % (sw['label'], sw['x'], sw['z'],
                                                     '; '.join('%s %s' % (k.split('_map')[0], v) for k, v in sw['heights'].items()),
                                                     'same surface in all' if sw['common'] else 'NOT COMMON'))
    if out_json:
        json.dump(res, open(out_json, 'w'), indent=1)
    print('RESULT', 'PASS' if ok_all else 'FAIL')
    return 0 if ok_all else 1


if __name__ == '__main__':
    a = sys.argv[1:]
    sys.exit(main(a[0], a[a.index('--json') + 1] if '--json' in a else None))
