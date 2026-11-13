"""ground_probe.py - is there walkable ground at a point in a block set? (static, read only)

For each block set (the map block + one submap: what the engine holds after a swap) every placed Model's collision
(HullList + per-pivot CollisionList, VertexCloud hulls and oriented boxes, solid surfaces only) and every solid
CollisionBox entity is put into world space (owner chain applied, docs/subsystem_collision.md 3 and 6), then vertical
rays are cast on a grid. A cell counts as ground when a solid upward-facing surface lies within the height window.

  python -I ground_probe.py GAME_DIR CORRIDOR [--step 2] [--json out.json]
  CORRIDOR = bridge | tunnel | farm | jungle | all

Reads the blocks through data/gameplay/gamefs.py with todgameplay_data.zip skipped (vanilla geometry), the collision
readers export_map.py and todcoll.py from the author's research tools (not included; set TOD_COLLISION_TOOLS to
their folder; imported, not modified).
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, 'data')
for p in (DATA, os.path.join(DATA, 'lib')):
    if p not in sys.path:
        sys.path.insert(0, p)
COLL = os.environ.get('TOD_COLLISION_TOOLS', '')
sys.path.append(COLL)
sys.path.append(os.path.join(COLL, 'proj'))
sys.dont_write_bytecode = True

import numpy as np                      # noqa: E402
import todblock as tb                   # noqa: E402
import fragedit as fe                   # noqa: E402
import fragtool as ft                   # noqa: E402
from gameplay.gamefs import GameFS      # noqa: E402
import export_map as xm                 # noqa: E402  (collision readers; its own import of todblock is a twin copy)
import todcoll as tc                    # noqa: E402

SHARED = 'data_pc/overdose_the_game/overdose/shared/'
MAIN = SHARED + 'overdose.main'

# corridor: block sets (label -> [map block, submap]), the probe rectangle (x0, x1, z0, z1) and the height window
CORRIDORS = {
    'bridge': dict(sets={'Downtown 01': ['mainmap_map02_lostoros_downtown.map', 'submap01_map02_lostoros_downtown.submap'],
                         'Harbour 01': ['mainmap_map04_lostoros_train_harbour.map', 'submap01_map04_lostoros_train_harbour.submap']},
                   rect=(-322, -290, -600, -280), y=(-6.0, 14.0)),
    'tunnel': dict(sets={'Downtown 02': ['mainmap_map02_lostoros_downtown.map', 'submap02_map02_lostoros_downtown.submap'],
                         'Industrial 04': ['mainmap_map03_lostoros_industrial.map', 'submap04_map03_lostoros_industrial.submap'],
                         'Industrial 02': ['mainmap_map03_lostoros_industrial.map', 'submap02_map03_lostoros_industrial.submap']},
                   rect=(-500, -280, 400, 510), y=(-14.0, 4.0)),
    'farm': dict(sets={'Farm 01': ['mainmap_map06_virgillos_farm.map', 'submap01_map06_virgillos_farm.submap'],
                       'Farm 02': ['mainmap_map06_virgillos_farm.map', 'submap02_map06_virgillos_farm.submap']},
                 rect=(920, 1200, -1520, -1080), y=(-70.0, 20.0)),
    'jungle': dict(sets={'Jungle 01': ['mainmap_map09_jungle.map', 'submap01_map09_jungle.submap'],
                         'Jungle 02': ['mainmap_map09_jungle.map', 'submap02_map09_jungle.submap']},
                   rect=(-560, -470, -2420, -2330), y=(-115.0, -80.0)),
}


def model_collision(info, R):
    """export_map.model_collision, tolerant of HullList shapes without a pivot byte (oriented boxes)."""
    res = []
    pp = tc.deref(info, R + 0x30)
    for i in range(tc.i32(info, R + 0x34)):
        c = tc.deref(info, pp + i * 0x7C + 0x44)
        if c is None:
            continue
        reg, _ = tc.read_region(info, c, 'piv')
        res += [('pivot', i, s) for s in reg['shapes']]
    f = tc.deref(info, R + 0x54)
    if f is not None:
        reg, _ = tc.read_region(info, f, 'f54')
        res += [('model', s.get('lowbyte', 0) & 0x7F, s) for s in reg['shapes']]
    return res


def solid(surf):
    return bool(tc.surf_flags_byte(surf) & 1)


class BlockGeo:
    """Solid collision triangles (world space) of one block set."""

    def __init__(self, fs, entries, log=print):
        self.entries = [SHARED + e for e in entries]
        T = ft.types()
        raws = {e: fs.read(e)[0] for e in self.entries + [MAIN]}
        models = {}
        for e in [MAIN] + self.entries:                   # later blocks win (submap > map > main)
            b = tb.Block(raws[e], os.path.splitext(e)[1])
            info = bytes(b.info)
            for R in tb.record_starts(info, b.n):
                if tb.u32(info, R) == 3:
                    models[xm.norm(tb.record_name(info, R))] = (info, R)
        tris, cache, n_models, n_boxes = [], {}, 0, 0
        for e in self.entries:
            b = tb.Block(raws[e], os.path.splitext(e)[1])
            frs, _ = ft.fragments(b)
            oc = {}
            for k, nm in frs:
                if len(b.payloads[k]) <= 24 or nm.lower().startswith('/data/scripts/fragments/'):
                    continue
                d = fe.FragDoc(b.payloads[k], T, nm)
                owner = None
                for n in d.created_ids():
                    ty = d.type_of(n) or ''
                    is_model = ty == 'Model' or ty.endswith('(Model)')
                    is_box = ty == 'CollisionBox' or ty.endswith('(CollisionBox)')
                    if not (is_model or is_box):
                        continue
                    if owner is None:
                        owner = ft.owner_chain(b, frs, k, T, cache=oc)[0]
                    W = d.world(n, owner)
                    if is_model:
                        ref = d.get(n, 'modelres')
                        if not ref or not str(ref).lower().endswith('.model'):
                            continue
                        key = xm.norm(str(ref))
                        if key not in models:
                            continue
                        if key not in cache:
                            info, R = models[key]
                            cache[key] = (xm.pivot_world(info, R), model_collision(info, R))
                        PW, shapes = cache[key]
                        n_models += 1
                        for space, pi, s in shapes:
                            if not solid(s['surf']):
                                continue
                            M = W if space == 'model' else xm.compose(W, PW[pi])
                            if s['type'] == 4:
                                V = [xm.apply(M, v) for v in s['verts']]
                                for f in s['faces']:
                                    for q in range(1, len(f) - 1):
                                        tris.append((V[f[0]], V[f[q]], V[f[q + 1]]))
                            else:
                                vs, tt = xm.box_corners_tris(s['size'], s['center'], s['quat'])
                                V = [xm.apply(M, v) for v in vs]
                                tris += [(V[a], V[b_], V[c]) for a, b_, c in tt]
                    else:
                        w, h, l = d.get(n, 'width'), d.get(n, 'height'), d.get(n, 'length')
                        fl = d.get(n, 'CollisionFlags') or 0
                        if None in (w, h, l) or not (int(fl) & 1):
                            continue
                        n_boxes += 1
                        vs, tt = xm.box_corners_tris((w, h, l), (0, 0, 0), (0, 0, 0, 1))
                        V = [xm.apply(W, v) for v in vs]
                        tris += [(V[a], V[b_], V[c]) for a, b_, c in tt]
        self.T = np.asarray(tris, dtype=np.float64).reshape(-1, 3, 3)
        self.models, self.boxes = n_models, n_boxes
        mn, mx = self.T.min(axis=1), self.T.max(axis=1)
        self.mn, self.mx = mn, mx
        log('  %s: %d placed models, %d solid boxes, %d solid triangles' % (
            ' + '.join(x.split('/')[-1] for x in self.entries), n_models, n_boxes, len(self.T)))

    def crop(self, rect, y):
        x0, x1, z0, z1 = rect
        m = (self.mx[:, 0] >= x0) & (self.mn[:, 0] <= x1) & (self.mx[:, 2] >= z0) & (self.mn[:, 2] <= z1) & \
            (self.mx[:, 1] >= y[0] - 50) & (self.mn[:, 1] <= y[1] + 50)
        return Cropped(self.T[m])


class Cropped:
    def __init__(self, T):
        self.T = T
        self.mn, self.mx = T.min(axis=1), T.max(axis=1)
        e1, e2 = T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]
        n = np.cross(e1, e2)
        ln = np.linalg.norm(n, axis=1)
        ln[ln == 0] = 1
        self.ny = np.abs(n[:, 1] / ln)                    # winding is not trusted: |normal.y| = flatness

    def hits(self, x, z, ylo, yhi, min_flat=0.5):
        """heights of walkable (|n.y| >= min_flat) surfaces under (x, z) within [ylo, yhi]."""
        m = (self.mn[:, 0] <= x) & (self.mx[:, 0] >= x) & (self.mn[:, 2] <= z) & (self.mx[:, 2] >= z) & \
            (self.mx[:, 1] >= ylo) & (self.mn[:, 1] <= yhi) & (self.ny >= min_flat)
        out = []
        for t in self.T[m]:
            a, b, c = t
            # barycentric in xz
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-12:
                continue
            l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) < -1e-6:
                continue
            yy = l1 * a[1] + l2 * b[1] + l3 * c[1]
            if ylo <= yy <= yhi:
                out.append(round(float(yy), 2))
        return sorted(set(out))


def probe_points(geo_by_label, pts, ywin):
    """[(label, x, z, {set label: [heights]})]"""
    out = []
    for lab, x, z in pts:
        row = {}
        for k, g in geo_by_label.items():
            row[k] = g.hits(x, z, ywin[0], ywin[1])
        out.append((lab, x, z, row))
    return out


def grid(geo_by_label, rect, ywin, step):
    x0, x1, z0, z1 = rect
    xs = np.arange(x0, x1 + 1e-6, step)
    zs = np.arange(z0, z1 + 1e-6, step)
    maps = {}
    for k, g in geo_by_label.items():
        m = np.zeros((len(zs), len(xs)), dtype=np.int8)
        for i, z in enumerate(zs):
            for j, x in enumerate(xs):
                m[i, j] = 1 if g.hits(float(x), float(z), ywin[0], ywin[1]) else 0
        maps[k] = m
    return xs, zs, maps


def run(game_dir, which, step=2.0, log=print):
    fs = GameFS(game_dir, skip=['todgameplay_data.zip'])
    res = {}
    for name in ([which] if which != 'all' else list(CORRIDORS)):
        c = CORRIDORS[name]
        log('%s: rect x %s..%s z %s..%s, y window %s..%s' % ((name,) + c['rect'] + c['y']))
        geos = {}
        for lab, ents in c['sets'].items():
            geos[lab] = BlockGeo(fs, ents, log).crop(c['rect'], c['y'])
        xs, zs, maps = grid(geos, c['rect'], c['y'], step)
        res[name] = dict(rect=c['rect'], y=c['y'], step=step, xs=xs.tolist(), zs=zs.tolist(),
                         maps={k: m.tolist() for k, m in maps.items()}, geos=geos)
    return res


def ascii(r, labels):
    """one char per cell: '#' all sets have ground, letters = only that set, '.' none."""
    xs, zs = r['xs'], r['zs']
    keys = list(labels)
    lines = ['legend: # = ground in all of %s; ' % ', '.join(keys) +
             ', '.join('%s = only %s' % (chr(97 + i), k) for i, k in enumerate(keys)) + '; + = some; . = none',
             'x from %s to %s step %s (columns); z from %s (top) to %s' % (xs[0], xs[-1], r['step'], zs[-1], zs[0])]
    for i in range(len(zs) - 1, -1, -1):
        row = []
        for j in range(len(xs)):
            have = [k for k in keys if r['maps'][k][i][j]]
            if len(have) == len(keys):
                row.append('#')
            elif len(have) == 1:
                row.append(chr(97 + keys.index(have[0])))
            elif have:
                row.append('+')
            else:
                row.append('.')
        lines.append('%8.0f %s' % (zs[i], ''.join(row)))
    return '\n'.join(lines)


if __name__ == '__main__':
    a = sys.argv[1:]
    step = float(a[a.index('--step') + 1]) if '--step' in a else 2.0
    r = run(a[0], a[1], step)
    for name, v in r.items():
        print('\n== %s' % name)
        print(ascii(v, v['maps'].keys()))
    if '--json' in a:
        out = a[a.index('--json') + 1]
        json.dump({k: {kk: vv for kk, vv in v.items() if kk != 'geos'} for k, v in r.items()}, open(out, 'w'))
