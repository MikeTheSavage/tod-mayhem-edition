"""minimap_geo.py - the solid collision of a TOD map in world space, with each triangle's face material (read only).

What the engine collides with is what the player can stand on and bump into, so it is the honest source for a
top-down map: every placed Model's collision (HullList +54h and the per-pivot CollisionLists +44h: VertexCloud hulls
and OrientedBoxes, docs/subsystem_collision.md) and every solid CollisionBox entity, put into world space through the
fragment owner chain (the same walk as seamless/tools/ground_probe.py). A map = its .map block + all its .submap blocks
(shared language folder), models resolved in submap > map > overdose.main order.

Face materials are the engine's fixed list (bullet impact mapping FUN_0062BED0, docs/subsystem_vfx.md 11.2):
0 AllRound 1 Metal 2 Concrete 3 Wood 4 Dirt 5 Stone 6 Glass 7 Bush 8 Wirefence 9 Grass 10 Asphalt 11 Water 12 Cloth
13 SolidGlass 14-17 BrokenWindow 18 Default_no_decal; 255 = none (oriented boxes, authored boxes).

Readers: data/lib (todblock, fragedit, fragtool: the fragment writer's proven codec) and lib/todcoll.py (a copy of
the byte-exact collision codec from the author's research tools). Python stdlib only.
"""
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(REPO, 'data')
for p in (DATA, os.path.join(DATA, 'lib'), os.path.join(HERE, 'lib')):
    if p not in sys.path:
        sys.path.insert(0, p)
sys.dont_write_bytecode = True

import todblock as tb                   # noqa: E402
import fragedit as fe                   # noqa: E402
import fragtool as ft                   # noqa: E402
import todcoll as tc                    # noqa: E402

SHARED = 'data_pc/overdose_the_game/overdose/shared/'
MAIN = SHARED + 'overdose.main'
NO_MAT = 255


# ---------------------------------------------------------------- transforms (pos, quat xyzw); stored quaternions are conjugated
def qconj(q): return (-q[0], -q[1], -q[2], q[3])
def qnorm(q):
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return tuple(x / n for x in q)
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx+ax*bw+ay*bz-az*by, aw*by-ax*bz+ay*bw+az*bx, aw*bz+ax*by-ay*bx+az*bw, aw*bw-ax*bx-ay*by-az*bz)
def qrot(q, v):
    x, y, z, w = q; vx, vy, vz = v
    tx = 2*(y*vz - z*vy); ty = 2*(z*vx - x*vz); tz = 2*(x*vy - y*vx)
    return (vx+w*tx+(y*tz-z*ty), vy+w*ty+(z*tx-x*tz), vz+w*tz+(x*ty-y*tx))
def compose(A, B):
    return (tuple(a + r for a, r in zip(A[0], qrot(A[1], B[0]))), qmul(A[1], B[1]))
def apply(T, v):
    r = qrot(T[1], v)
    return (r[0] + T[0][0], r[1] + T[0][1], r[2] + T[0][2])


def norm(s):
    s = s.lower().replace('\\', '/')
    return s[6:] if s.startswith('/data/') else s.lstrip('/')


def pivot_world(info, R):
    pp = tc.deref(info, R + 0x30); n = tc.i32(info, R + 0x34)
    loc = []
    for i in range(n):
        pv = pp + i * 0x7C
        pos = struct.unpack_from('<3f', info, pv); q = struct.unpack_from('<4f', info, pv + 12)
        loc.append((pos, qconj(qnorm(q)), tc.i32(info, pv + 0x58)))
    out = [None] * n
    def get(i, d=0):
        if out[i] is None:
            p, q, par = loc[i]
            out[i] = compose(get(par, d + 1), (p, q)) if (0 <= par < n and par != i and d < 64) else (p, q)
        return out[i]
    for i in range(n):
        get(i)
    return out


def model_collision(info, R):
    """[(space 'model' | 'pivot', pivot index, shape)] (HullList shapes without a pivot byte tolerated)."""
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


def box_corners_tris(size, center, quat):
    hx, hy, hz = (size[0] * 0.5, size[1] * 0.5, size[2] * 0.5)
    q = qconj(qnorm(quat)) if any(quat) else (0, 0, 0, 1)
    vs = [apply((center, q), (sx * hx, sy * hy, sz * hz)) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    tris = []
    for a, b, c, d in faces:
        tris += [(a, b, c), (a, c, d)]
    return vs, tris


def solid(surf):
    return bool(tc.surf_flags_byte(surf) & 1)


# ---------------------------------------------------------------- what a placed model means on a map
K_TREE, K_PALM, K_BUSH = 1, 2, 3
# 6 .. 14 m wide models that are street furniture, vehicles or heaps, not structures
CLUTTER = re.compile(r'sign|light|lamp|vehicle|carwreck|speedboat|addjump|gravelpile|woodpile|junkyardpile|board_|conveyor|'
                     r'curb|^rail|table|counter|desk|ladder|baloon|cone|aachaingun|dish|mini_train|steelconstruction')


def model_role(name, ex, ez):
    """'keep' (the collision is drawn), 'drop' (street furniture, props, vehicles: clutter on a map of this scale), or
    (K_TREE / K_PALM / K_BUSH, radius m): a symbol at the placed position instead of its collision hull. name = the file
    name without .model, ex / ez = the size of the model's solid collision (0 = it has none)."""
    n = name.lower()
    if n.startswith(('veg_bush', 'jungle_bush', 'veg_desert_bush', 'veg_desert_yukka', 'flowerbush')):
        r = 1.8 if 'large' in n or 'big' in n or 'group' in n else (0.9 if 'small' in n or 'yukka' in n else 1.3)
        return K_BUSH, r
    if 'palm' in n:
        return K_PALM, min(3.6, max(2.2, max(ex, ez) * 0.5))
    if ('tree' in n or 'cactus' in n) and 'baserubble' not in n and '_line' not in n:
        return K_TREE, min(6.5, max(1.8, max(ex, ez) * 0.5 if ex else 2.4))
    w = max(ex, ez)
    if w <= 6.0:
        return 'drop'
    if w <= 14.0 and CLUTTER.search(n):
        return 'drop'
    return 'keep'


def model_extent(info, R):
    """(x size, z size) of a model's solid collision in its own space, (0, 0) without any."""
    PW = pivot_world(info, R)
    xs, zs = [], []
    for space, pi, s in model_collision(info, R):
        if not solid(s['surf']):
            continue
        M = ((0, 0, 0), (0, 0, 0, 1)) if space == 'model' else (PW[pi] if pi < len(PW) else ((0, 0, 0), (0, 0, 0, 1)))
        V = s['verts'] if s['type'] == 4 else box_corners_tris(s['size'], s['center'], s['quat'])[0]
        for v in V:
            w = apply(M, v)
            xs.append(w[0]); zs.append(w[2])
    return (max(xs) - min(xs), max(zs) - min(zs)) if xs else (0.0, 0.0)


def list_maps(fs):
    """{map key: [map block, submap blocks...]} of the shared folder (map key = 'map02_lostoros_downtown')."""
    naz = fs.naz()
    names = [n for n in naz.names if n.startswith(SHARED)]
    maps = {}
    for n in sorted(names):
        base = n[len(SHARED):]
        if base.startswith('mainmap_') and base.endswith('.map'):
            maps.setdefault(base[len('mainmap_'):-4], []).insert(0, base)
    for n in sorted(names):
        base = n[len(SHARED):]
        if base.startswith('submap') and base.endswith('.submap'):
            key = base.split('_', 1)[1][:-7]
            for k in maps:
                if key == k or key.startswith(k + '_'):
                    maps[k].append(base)
                    break
    return maps


class MapGeo:
    """Solid collision triangles of one map (map block + submaps) in world space, street furniture left out.
    tris: list of (x0, y0, z0, x1, y1, z1, x2, y2, z2, material); marks: list of (x, z, radius, kind) for trees and bushes
    (model_role)."""

    def __init__(self, fs, entries, log=print):
        self.entries = [SHARED + e for e in entries]
        T = ft.types()
        raws = {}
        for e in self.entries + [MAIN]:
            try:
                raws[e] = fs.read(e)[0]
            except KeyError:
                log('  %s: not in the archives, skipped' % e)
        models = {}
        for e in [MAIN] + self.entries:                   # later blocks win (submap > map > main)
            if e not in raws:
                continue
            b = tb.Block(raws[e], os.path.splitext(e)[1])
            info = bytes(b.info)
            for R in tb.record_starts(info, b.n):
                if tb.u32(info, R) == 3:
                    models[norm(tb.record_name(info, R))] = (info, R)
        tris, cache, roles, marks = [], {}, {}, []
        self.n_models = self.n_boxes = self.n_skipped = self.n_dropped = 0
        for e in self.entries:
            if e not in raws:
                continue
            b = tb.Block(raws[e], os.path.splitext(e)[1])
            frs, _ = ft.fragments(b)
            oc = {}
            for k, nm in frs:
                if len(b.payloads[k]) <= 24 or nm.lower().startswith('/data/scripts/fragments/'):
                    continue
                try:
                    d = fe.FragDoc(b.payloads[k], T, nm)
                except Exception as ex:                      # a fragment the codec cannot read: skipped, counted
                    self.n_skipped += 1
                    log('  %s %s: not read (%s)' % (e.split('/')[-1], nm, ex))
                    continue
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
                        key = norm(str(ref))
                        if key not in models:
                            continue
                        if key not in roles:
                            info, R = models[key]
                            ex, ez = model_extent(info, R)
                            roles[key] = model_role(key.split('/')[-1][:-6], ex, ez)
                        role = roles[key]
                        if role == 'drop':
                            self.n_dropped += 1
                            continue
                        if role != 'keep':
                            marks.append((W[0][0], W[0][2], role[1], role[0]))
                            continue
                        if key not in cache:
                            info, R = models[key]
                            cache[key] = (pivot_world(info, R), model_collision(info, R))
                        PW, shapes = cache[key]
                        self.n_models += 1
                        for space, pi, s in shapes:
                            if not solid(s['surf']):
                                continue
                            M = W if space == 'model' else compose(W, PW[pi]) if pi < len(PW) else W
                            if s['type'] == 4:
                                V = [apply(M, v) for v in s['verts']]
                                fm = s.get('facemat') or []
                                for fi, f in enumerate(s['faces']):
                                    mat = fm[fi] if fi < len(fm) else NO_MAT
                                    for q in range(1, len(f) - 1):
                                        a, bb, c = V[f[0]], V[f[q]], V[f[q + 1]]
                                        tris.append((a[0], a[1], a[2], bb[0], bb[1], bb[2], c[0], c[1], c[2], mat))
                            else:
                                vs, tt = box_corners_tris(s['size'], s['center'], s['quat'])
                                V = [apply(M, v) for v in vs]
                                for a_, b_, c_ in tt:
                                    a, bb, c = V[a_], V[b_], V[c_]
                                    tris.append((a[0], a[1], a[2], bb[0], bb[1], bb[2], c[0], c[1], c[2], NO_MAT))
                    else:
                        w, h, l = d.get(n, 'width'), d.get(n, 'height'), d.get(n, 'length')
                        fl = d.get(n, 'CollisionFlags') or 0
                        if None in (w, h, l) or not (int(fl) & 1):
                            continue
                        self.n_boxes += 1
                        vs, tt = box_corners_tris((w, h, l), (0, 0, 0), (0, 0, 0, 1))
                        V = [apply(W, v) for v in vs]
                        for a_, b_, c_ in tt:
                            a, bb, c = V[a_], V[b_], V[c_]
                            tris.append((a[0], a[1], a[2], bb[0], bb[1], bb[2], c[0], c[1], c[2], NO_MAT))
        self.tris, self.marks = tris, marks
        log('  %s: %d placed models (%d props left out, %d tree / bush symbols), %d solid boxes, %d solid triangles%s' % (
            ' + '.join(x.split('/')[-1] for x in self.entries), self.n_models, self.n_dropped, len(marks), self.n_boxes, len(tris),
            (', %d fragments not read' % self.n_skipped) if self.n_skipped else ''))
