"""The `seamless` data option: the corridor teleport loaders become walk borders.

Background: the author's reverse-engineering notes (not included). A loader volume is a Trigger_Root
box with Trigger_Activate_Load_Unload_Save actions (ActionType 21). Without Teleport_to the engine keeps the player
where he is; the corridor loaders teleport because their boxes stand in the wrong order for walking (the exit box
comes BEFORE the other block's return box). The fix (package T1 of the author's seamless research tools,
not included): empty every Teleport_to of the pair and move one box past the other along the corridor, so
after the swap the other block's return box lies GAP metres behind the player. Set ops on existing nodes only (the
root's pos and the actions' Teleport_to): no create record changes, saves keep working.

Corridors (ground checked in both blocks with seamless/tools/ground_probe.py):
  bridge  Downtown 01 <-> Harbour 01: Downtown's box moves south past Harbour's (the T1 package).
  tunnel  Downtown 02 <-> Industrial 04, and Industrial 02 -> Downtown 02 (the same volume in both Industrial
          blocks): Downtown's box moves west through the tunnel past the Industrial box.
  (farm   Farm 01 <-> Farm 02 is NOT converted: the two blocks do not share the pass terrain; their surfaces are
          different meshes up to 20 m apart and neither block has ground at the other's box.)

Every value is computed from the player's own blocks: boxes are found by their action's TargetName + Teleport_to, the
move is centre + d * t with d the horizontal direction from the moved box to the other box and t chosen so the gap
between the two boxes along d is GAP (extents from the boxes' own sizes and rotations).
"""
import math
import os

from . import options as opt
import fragtool as _ft     # noqa: E402  (data/lib is on sys.path through options)

SHARED = opt.SHARED
GAP = 23.0                 # metres between the boxes along the corridor (T1)
MIN_GAP = 20.0             # the check refuses less

CORRIDORS = [
    dict(key='bridge', label='Downtown 01 <-> Harbour 01 (the bridge)',
         move=dict(block='submap01_map02_lostoros_downtown.submap',
                   target='SUBMAP01_Map04_LosToros_Train_Harbour', teleport='Teleport_Bridge_Downtown'),
         other=dict(block='submap01_map04_lostoros_train_harbour.submap',
                    target='SUBMAP01_Map02_LosToros_Downtown', teleport='Teleport_Bridge_HArbour'),
         twins=[]),
    dict(key='tunnel', label='Downtown 02 <-> Industrial 04 / Industrial 02 -> Downtown 02 (the tunnel)',
         move=dict(block='submap02_map02_lostoros_downtown.submap',
                   target='SUBMAP04_Map03_LosToros_Industrial', teleport='Teleport_Tunnel_Downtown'),
         other=dict(block='submap04_map03_lostoros_industrial.submap',
                    target='SUBMAP02_Map02_LosToros_Downtown', teleport='Teleport_Tunnel_Industrial'),
         twins=[dict(block='submap02_map03_lostoros_industrial.submap',
                     target='SUBMAP02_Map02_LosToros_Downtown', teleport='Teleport_Tunnel_Industrial')]),
]


class SeamlessError(Exception):
    pass


# ------------------------------------------------------------------ geometry (sections.py conventions)
def qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)]


def extent(vol, d):
    """half extent of a volume along the horizontal unit direction d = (dx, dz)."""
    if vol.get('radius'):
        return float(vol['radius'])
    hx, hy, hz = [float(s or 0) / 2 for s in vol['size']]
    ax = qrot(vol['rot'], [1.0, 0.0, 0.0])
    az = qrot(vol['rot'], [0.0, 0.0, 1.0])
    return abs(ax[0] * d[0] + ax[2] * d[1]) * hx + abs(az[0] * d[0] + az[2] * d[1]) * hz


def order(v, w, d):
    """box order along d (pointing from w towards where v must lie): gap > 0 = walk order."""
    s = (v['pos'][0] - w['pos'][0]) * d[0] + (v['pos'][2] - w['pos'][2]) * d[1]
    gap = s - extent(v, d) - extent(w, d)
    return dict(along=round(s, 3), gap=round(gap, 3), state='walk' if gap > 0 else 'wrong')


# ------------------------------------------------------------------ scanning loader volumes of a block
def scan_doc_volumes(entry, block, frs, T, fe, ft):
    """Every ActionType 21 loader volume of a parsed block: root node, fragment, world centre / rotation / size, the
    node's world position, the actions (node, TargetName, Teleport_to)."""
    out = {}
    cache = {}
    for k, nm in frs:
        if len(block.payloads[k]) <= 24:
            continue
        d = fe.FragDoc(block.payloads[k], T, nm)
        owner = None
        for n in d.created_ids():
            ty = d.type_of(n) or ''
            if not (ty.startswith('Trigger_Activate_Load_Unload_Save') and d.get(n, 'ActionType') == 21):
                continue
            root, seen = d.get(n, 'parent') or 0, set()
            while root and root not in seen and not (d.type_of(root) or '').startswith('Trigger_Root'):
                seen.add(root)
                root = d.get(root, 'parent') or 0
            if not root:
                continue
            if owner is None:
                owner = ft.owner_chain(block, frs, k, T, cache=cache)[0]
            key = (nm, root)
            if key not in out:
                W = d.world(root, owner)
                q = fe.qnorm(W[1])                        # true world rotation (compose uses conjugated quats)
                size = [d.get(root, 'width'), d.get(root, 'height'), d.get(root, 'length')]
                centre = list(W[0])
                if d.get(root, 'left') is not None:      # Trigger_Root(MockupBox): left/right/front/back
                    l, r_, f_, b_ = [float(d.get(root, kk) or 0) for kk in ('left', 'right', 'front', 'back')]
                    size = [r_ - l, d.get(root, 'height'), b_ - f_]
                    off = qrot(q, [(l + r_) / 2, 0.0, (f_ + b_) / 2])
                    centre = [centre[j] + off[j] for j in range(3)]
                par = d.get(root, 'parent') or 0
                out[key] = dict(entry=entry, block=entry.split('/')[-1], frag=nm, root=root, type=d.type_of(root),
                                name=d.get(root, 'name'), pos=centre, node_pos=list(W[0]), rot=list(q), size=size,
                                radius=d.get(root, 'radius'), parent_world=d.world(par, owner) if par else owner,
                                actions=[])
            out[key]['actions'].append(dict(node=n, name=d.get(n, 'name'), target=d.get(n, 'TargetName') or '',
                                            teleport=d.get(n, 'Teleport_to') or ''))
    return list(out.values())


def find_volume(vols, spec):
    hits = [v for v in vols if any(a['target'].lower() == spec['target'].lower() and a['teleport'] == spec['teleport']
                                   for a in v['actions'])]
    if len(hits) != 1:
        raise SeamlessError('%s: %d loader volumes load %s with Teleport_to %s (expected exactly 1)' % (
            spec['block'], len(hits), spec['target'], spec['teleport']))
    return hits[0]


def plan_corridor(c, vols_of):
    """(moved volume, other volume, twin volumes, direction, distance, new centre)."""
    a = find_volume(vols_of(c['move']['block']), c['move'])
    b = find_volume(vols_of(c['other']['block']), c['other'])
    twins = [find_volume(vols_of(t['block']), t) for t in c['twins']]
    for t in twins:
        if max(abs(t['pos'][j] - b['pos'][j]) for j in range(3)) > 0.01 or \
                max(abs(t['size'][j] - b['size'][j]) for j in range(3)) > 0.01:
            raise SeamlessError('%s %s is not the same box as %s %s' % (t['block'], t['name'], b['block'], b['name']))
    dx, dz = b['pos'][0] - a['pos'][0], b['pos'][2] - a['pos'][2]
    L = math.hypot(dx, dz)
    if L < 1.0:
        raise SeamlessError('%s: the two boxes share a centre' % c['key'])
    d = (dx / L, dz / L)
    ea, eb = extent(a, d), extent(b, d)
    s_now = (a['pos'][0] - b['pos'][0]) * d[0] + (a['pos'][2] - b['pos'][2]) * d[1]   # negative: a comes first
    t = (eb + ea + GAP) - s_now
    new = [a['pos'][0] + d[0] * t, a['pos'][1], a['pos'][2] + d[1] * t]
    return a, b, twins, d, t, new


def local_pos(vol, centre_world, fe):
    """node-local pos that puts the volume centre at centre_world (rotation unchanged)."""
    off = [vol['pos'][j] - vol['node_pos'][j] for j in range(3)]          # MockupBox centre offset (0 for boxes)
    wp = tuple(centre_world[j] - off[j] for j in range(3))
    PW = vol['parent_world']
    L = fe.compose(fe.inverse(PW), (wp, (0.0, 0.0, 0.0, 1.0)))
    return list(L[0])


# ------------------------------------------------------------------ the option generator (set ops)
def gen_seamless(ctx, level):
    if level == 'off':
        return []
    fe, ft = opt.fe, _ft
    vols_cache = {}

    def vols_of(blk):
        if blk not in vols_cache:
            entry = SHARED + blk
            info = ctx.block(entry)
            vols_cache[blk] = scan_doc_volumes(entry, info['block'], info['frs'], ctx.T, fe, ft)
        return vols_cache[blk]

    ops = []
    converted = set()
    for c in CORRIDORS:
        try:
            a, b, twins, d, t, new = plan_corridor(c, vols_of)
        except SeamlessError as e:
            raise opt.OptionError(str(e))
        converted |= {(v['block'], v['frag'], v['root']) for v in [a, b] + twins}
        for v in [a, b] + twins:
            doc = ctx.doc(v['entry'], v['frag'])
            for act in v['actions']:
                if act['teleport']:
                    ops.append(opt.Op(v['entry'], v['frag'], act['node'], 'Teleport_to', '', act['teleport'],
                                      '%s: no teleport after the swap (the player keeps walking)' % c['key'],
                                      doc.path_of(act['node'])))
        doc = ctx.doc(a['entry'], a['frag'])
        lp = [opt.f32(x) for x in local_pos(a, new, fe)]
        old = doc.get(a['root'], 'pos')
        ops.append(opt.Op(a['entry'], a['frag'], a['root'], 'pos', lp, old,
                          '%s: box moved %.1f m along the corridor to (%.2f, %.2f, %.2f), %.0f m past %s' % (
                              c['key'], t, new[0], new[1], new[2], GAP, b['name']), doc.path_of(a['root'])))
    if level == 'on_nomarkers':
        ops += marker_ops(ctx, vols_of, converted)
    return ops


# ------------------------------------------------------------------ border markers (level on_nomarkers)
# Every loader volume of the city / jungle / farm carries up to two generic_object_effect(Sprite) children: the loader
# beam (texture /Textures/Sprites/MapTeleport.bmp, 20-24 m, additive, instancing the Map_LoaderBeam prefab = one
# test_fade_forwarder node) and the area sign (/Textures/Hud/ExitMap_<area>.BMP, 2 m). The tunnel's Industrial sign
# hangs in MAPLOADERS_Enabled next to its box instead. ZAP stops (Trigger_Root_ZAP) and their destination icons use the
# same two textures but are other entities; mission loaders live in mission blocks: both untouched.
LOADER_BLOCKS = [
    'mainmap_map01_dea_hq_us_border.map', 'submap01_map01_dea_hq_us_border.submap',
    'submap01_map02_lostoros_downtown.submap', 'submap02_map02_lostoros_downtown.submap',
    'submap03_map02_lostoros_downtown.submap', 'submap04_map02_lostoros_downtown.submap',
    'submap01_map03_lostoros_industrial.submap', 'submap02_map03_lostoros_industrial.submap',
    'submap03_map03_lostoros_industrial.submap', 'submap04_map03_lostoros_industrial.submap',
    'submap01_map04_lostoros_train_harbour.submap', 'submap02_map04_lostoros_train_harbour.submap',
    'submap01_map06_virgillos_farm.submap', 'submap02_map06_virgillos_farm.submap',
    'submap01_map09_jungle.submap', 'submap02_map09_jungle.submap']
MARKER_TEX = ('/textures/sprites/mapteleport.bmp', '/textures/hud/exitmap_')
SIGN_NEAR = 15.0          # a loose sign in a loader folder belongs to the loader box within this (x/z metres)
HIDDEN_SIZE = [0.0, 0.0, 1.0]


def is_marker(doc, n):
    tex = (doc.get(n, 'texture') or '').lower()
    return (doc.type_of(n) or '').startswith('generic_object_effect') and tex.startswith(MARKER_TEX)


def walk_volumes(vols_of, converted):
    """{(block, frag, root)} of the loader volumes whose markers go: class WALK by the catalogue rule, plus the converted
    corridor boxes (one of them is ONEWAY by that rule only because Downtown returns to its twin Industrial 04)."""
    vb = {blk: vols_of(blk) for blk in LOADER_BLOCKS}
    keys = {}
    for blk, vols in vb.items():
        for v in vols:
            keys[(blk, v['name'], round(v['pos'][0], 1), round(v['pos'][2], 1))] = (blk, v['frag'], v['root'])
    out = set()
    for r in classify(vb):
        k = keys.get((r['block'], r['name'], r['pos'][0], r['pos'][2]))
        if k and (r['cls'] == 'WALK' or k in converted):
            out.add(k)
    return out | converted


def marker_ops(ctx, vols_of, converted):
    hide = walk_volumes(vols_of, converted)
    ops = []
    for blk in LOADER_BLOCKS:
        entry = SHARED + blk
        info = ctx.block(entry)
        vols = [v for v in vols_of(blk) if (blk, v['frag'], v['root']) in hide]
        if not vols:
            continue
        cache = {}
        for k, nm in info['frs']:
            if len(info['block'].payloads[k]) <= 24:
                continue
            doc = ctx.doc(entry, nm)
            mine = [v for v in vols if v['frag'] == nm]
            owner = None
            for n in doc.created_ids():
                if not is_marker(doc, n):
                    continue
                par = doc.get(n, 'parent') or 0
                ptype = doc.type_of(par) or ''
                hit = next((v for v in mine if v['root'] == par), None)
                if hit is None and ptype == 'Folder' and (doc.get(par, 'name') or '').upper().startswith('MAPLOADER'):
                    if owner is None:
                        owner = _ft.owner_chain(info['block'], info['frs'], k, ctx.T, cache=cache)[0]
                    w = doc.world(n, owner)[0]
                    near = [v for v in vols if math.hypot(v['pos'][0] - w[0], v['pos'][2] - w[2]) <= SIGN_NEAR]
                    hit = near[0] if near else None
                if hit is None:
                    continue
                what = 'beam' if 'mapteleport' in (doc.get(n, 'texture') or '').lower() else 'sign'
                note = 'border marker hidden (%s of %s; zero size, opacity 0)' % (what, hit['name'])
                old = doc.get(n, 'sprite_size')
                if [opt.f32(x) for x in (old or [])] != HIDDEN_SIZE:
                    ops.append(opt.Op(entry, nm, n, 'sprite_size', list(HIDDEN_SIZE), old, note, doc.path_of(n)))
                if (doc.get(n, 'opacity') or 0.0) != 0.0:
                    ops.append(opt.Op(entry, nm, n, 'opacity', 0.0, doc.get(n, 'opacity'), note, doc.path_of(n)))
    return ops


# ------------------------------------------------------------------ the build-time check (after the blocks are written)
def check(ctx, zip_entries, tb, ft, fe):
    """Re-scan the built blocks: every corridor loader has no Teleport_to, the moved box sits where planned, the
    box order is 'walk' with >= MIN_GAP, the twin boxes are unchanged, and each volume's class by the seamless
    tools' rule (no teleport + a return loader in the target block within 350 m = WALK)."""
    out_blocks = {e: data for e, data in zip_entries}
    base_vols, new_vols = {}, {}

    def scan(entry, raw):
        b = tb.Block(raw, os.path.splitext(entry)[1])
        frs, _ = ft.fragments(b)
        return scan_doc_volumes(entry, b, frs, ctx.T, fe, ft)

    def vols(blk, which):
        entry = SHARED + blk
        cache = base_vols if which == 'base' else new_vols
        if blk not in cache:
            raw = ctx.block(entry)['raw'] if which == 'base' or entry not in out_blocks else out_blocks[entry]
            cache[blk] = scan(entry, raw)
        return cache[blk]

    rep = dict(corridors=[], checks=[])
    fails = []

    def chk(ok, text):
        rep['checks'].append(('PASS' if ok else 'FAIL', text))
        if not ok:
            fails.append(text)

    for c in CORRIDORS:
        a0, b0, tw0, d, t, new = plan_corridor(c, lambda blk: vols(blk, 'base'))
        same = lambda v0, blk: [v for v in vols(blk, 'new') if v['frag'] == v0['frag'] and v['root'] == v0['root']]  # noqa: E731
        a1, b1 = same(a0, c['move']['block']), same(b0, c['other']['block'])
        chk(len(a1) == 1 and len(b1) == 1, '%s: both loader volumes present after the build' % c['key'])
        if len(a1) != 1 or len(b1) != 1:
            continue
        a1, b1 = a1[0], b1[0]
        tw1 = [same(v0, s['block']) for v0, s in zip(tw0, c['twins'])]
        pairs = [(a0, a1), (b0, b1)] + [(v0, x[0]) for v0, x in zip(tw0, tw1) if x]
        for v0, v in pairs:
            chk(all(not x['teleport'] for x in v['actions']), '%s %s: every action has an empty Teleport_to' % (v['block'], v['name']))
            chk([(x['node'], x['target']) for x in v['actions']] == [(x['node'], x['target']) for x in v0['actions']],
                '%s %s: same actions, targets unchanged (%s)' % (v['block'], v['name'], ', '.join(x['target'] for x in v['actions'])))
        moved = max(abs(a1['pos'][j] - new[j]) for j in range(3))
        chk(moved < 1e-3, '%s: %s centre (%.3f, %.3f, %.3f) = planned (off by %.5f m)' % (c['key'], a1['name'], a1['pos'][0], a1['pos'][1], a1['pos'][2], moved))
        chk(max(abs(a1['rot'][j] - a0['rot'][j]) for j in range(4)) < 1e-6 and a1['size'] == a0['size'],
            '%s: %s rotation and size unchanged' % (c['key'], a1['name']))
        chk(max(abs(b1['pos'][j] - b0['pos'][j]) for j in range(3)) < 1e-6,
            '%s: %s (%s) not moved' % (c['key'], b1['name'], b1['block']))
        o0 = order(a0, b0, d)
        o1 = order(a1, b1, d)
        chk(o0['state'] == 'wrong' and o1['state'] == 'walk' and o1['gap'] >= MIN_GAP,
            '%s: box order along the corridor %s (gap %.1f m) -> %s (gap %.1f m)' % (c['key'], o0['state'], o0['gap'], o1['state'], o1['gap']))
        for v0, x in zip(tw0, tw1):
            ok = len(x) == 1 and max(abs(x[0]['pos'][j] - v0['pos'][j]) for j in range(3)) < 1e-6
            chk(ok, '%s: twin %s (%s) not moved, order vs the moved box %s' % (
                c['key'], v0['name'], v0['block'], order(a1, x[0], d)['state'] if ok else '?'))
        rep['corridors'].append(dict(key=c['key'], label=c['label'], moved=dict(block=a1['block'], name=a1['name'],
                                     old=[round(x, 3) for x in a0['pos']], new=[round(x, 3) for x in a1['pos']],
                                     by_m=round(t, 2)), dir=[round(d[0], 4), round(d[1], 4)],
                                     order_before=o0, order_after=o1,
                                     teleports_cleared=sum(1 for v in [a0, b0] + tw0 for x in v['actions'] if x['teleport'])))
    rep['result'] = 'PASS' if not fails else 'FAIL'
    return rep, fails


# ------------------------------------------------------------------ the classification re-run (transitions.catalog rule)
def classify(vols_by_block):
    """{block: [volumes]} -> rows with class TELEPORT / WALK / ONEWAY (tools/seamless/transitions.py rule; the
    teleport distance is not needed here: a non-empty Teleport_to counts as TELEPORT, as all 7 corridor ones are
    > 25 m)."""
    rows = []
    folders = {b.rsplit('.', 1)[0].lower(): b for b in vols_by_block}
    for blk, vols in vols_by_block.items():
        src = blk.rsplit('.', 1)[0].lower()
        for v in vols:
            tg = [a['target'] for a in v['actions']]
            sub = [x for x in tg if x.upper().startswith('SUBMAP')]
            tgt = (sub or tg or [''])[0].lower()
            tblk = folders.get(tgt)
            ret = None
            if tblk:
                for w in vols_by_block[tblk]:
                    if any(a['target'].lower() == src for a in w['actions']):
                        dd = math.hypot(w['pos'][0] - v['pos'][0], w['pos'][2] - v['pos'][2])
                        ret = dd if ret is None or dd < ret else ret
            tele = any(a['teleport'] for a in v['actions'])
            cls = 'TELEPORT' if tele else ('WALK' if ret is not None and ret < 350 else 'ONEWAY')
            rows.append(dict(block=blk, name=v['name'], frag=v['frag'], root=v['root'],
                             pos=[round(x, 1) for x in v['pos']], target=tgt, cls=cls,
                             return_m=None if ret is None else round(ret, 1)))
    return rows
