"""Catalogue every block-load transition (ActionType 21 loader volume) of TOD and classify it (static, read-only).

  python transitions.py dump    OUT_ENTS.json                       re-scan every shared block (~4 min)
  python transitions.py catalog ENTS.json -o catalog.json [--md table.md]

A loader volume is one Trigger_Root(CollisionBox|CollisionSphere) with >= 1 Trigger_Activate_Load_Unload_Save
(ActionType 21) action. For each volume:
  * source block, root name, centre, size; targets (SUBMAP and/or MAINMAP folders); Teleport_to node (if any)
  * teleport destination (node found by name in the target block, overdose.main, or anywhere) and its distance
  * the nearest RETURN loader in the target block (a volume there that loads the source submap) and its distance
  * model + CollisionBox count of source and target block within 40 m (x/z) of the volume centre
Class:
  TELEPORT  a Teleport_to is set and the destination is > 25 m from the volume (the player is moved)
  WALK      no teleport and a return loader (target submap or its map block) within 350 m: the two blocks share
            a physical border and the player keeps walking (the engine keeps his position when no Teleport_to)
  ONEWAY    no teleport and no return loader (a one-way walk-in, e.g. into an interior block)
  (the detail counts are informational: big terrain meshes have one pivot, so counts near borders are low)
  MISSION   the volume sits in a .mission block (mission-scoped loader)
"""
import os, sys, json, math, collections
sys.dont_write_bytecode = True


def folder_of_block(b):
    return os.path.splitext(b)[0].lower()


def block_of_folder(f, D):
    f = f.lower()
    for ext in ('.submap', '.map'):
        if f + ext in D:
            return f + ext
    return None


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def dxz(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def volumes(D):
    g = collections.OrderedDict()
    for b, r in D.items():
        for l in r['loaders']:
            ro = l['root'] or {}
            key = (b, ro.get('name'), tuple(ro.get('pos') or []))
            v = g.setdefault(key, dict(block=b, name=ro.get('name'), pos=ro.get('pos'), size=ro.get('size'),
                                       radius=ro.get('radius'), type=ro.get('type'), fragment=l['fragment'],
                                       targets=[], teleports=[]))
            v['targets'].append(l['target'])
            if l['teleport'] and l['teleport'] not in v['teleports']:
                v['teleports'].append(l['teleport'])
    return list(g.values())


def density(D, b, p, R):
    if not b or b not in D:
        return 0
    n = 0
    for e in D[b]['ents']:
        if e['c'] in ('model', 'collision_box') and any(abs(x) > 1e-3 for x in e['p']) and dxz(e['p'], p) < R:
            n += 1
    return n


def find_node(D, name, blocks):
    for b in blocks:
        if b and b in D:
            for e in D[b]['ents']:
                if e['name'] == name:
                    return b, e['p']
    for b in D:
        for e in D[b]['ents']:
            if e['name'] == name:
                return b, e['p']
    return None, None


def catalog(D, R=40.0):
    V = volumes(D)
    by_block = collections.defaultdict(list)
    for v in V:
        by_block[v['block']].append(v)
    out = []
    for v in V:
        src = v['block']
        subt = [t for t in v['targets'] if t.upper().startswith('SUBMAP')]
        mapt = [t for t in v['targets'] if t.upper().startswith('MAINMAP')]
        tgt = subt[0] if subt else (mapt[0] if mapt else None)
        tblk = block_of_folder(tgt, D) if tgt else None
        rec = dict(v, target=tgt, target_block=tblk, map_change=bool(mapt))
        p = v['pos']
        rec['tele_dest'] = None
        rec['tele_dist'] = None
        if v['teleports']:
            fb, fp = find_node(D, v['teleports'][0], [tblk, 'overdose.main', src])
            if fp:
                rec['tele_dest'] = dict(block=fb, pos=fp)
                rec['tele_dist'] = round(dist(p, fp), 1)
        srcf = folder_of_block(src)
        ret = None
        mapblk = None
        if tblk and tblk.startswith('submap'):
            mapblk = [b for b in D if b.startswith('mainmap_') and b.endswith('.map') and b[8:-4] == tblk.split('_', 1)[1][:-7]]
            mapblk = mapblk[0] if mapblk else None
        srcmap = None
        if src.startswith('mainmap_'):
            srcmap = src[8:-4]
        if tblk:
            for w in by_block.get(tblk, []) + by_block.get(mapblk, []):
                if any(t.lower() == srcf or (srcmap and t.lower().endswith(srcmap) and t.upper().startswith('SUBMAP')) for t in w['targets']):
                    d = dxz(w['pos'], p)
                    if ret is None or d < ret[0]:
                        ret = (d, w['name'], w['pos'])
        rec['return_loader'] = None if ret is None else dict(dist=round(ret[0], 1), name=ret[1], pos=ret[2])
        rec['dens_src'] = density(D, src, p, R)
        rec['dens_tgt'] = density(D, tblk, p, R)
        if src.endswith('.mission'):
            cls = 'MISSION'
        elif rec['tele_dist'] is not None and rec['tele_dist'] > 25:
            cls = 'TELEPORT'
        else:
            cls = 'WALK' if (ret is not None and ret[0] < 350) else 'ONEWAY'
        rec['class'] = cls
        out.append(rec)
    return out


def short(n):
    if not n:
        return '-'
    for a in ('_LosToros', '_lostoros', '_DEA_HQ_US_Border', '_dea_hq_us_border', '_Train_Harbour', '_train_harbour',
              '_Virgillos_Farm', '_virgillos_farm', '.submap', '.map', '.mission'):
        n = n.replace(a, '')
    return n.replace('submap0', 'S').replace('SUBMAP0', 'S').replace('mainmap_', 'M:').replace('MAINMAP_', 'M:')


def write_md(cat, path):
    L = ['| # | Source | Volume | Centre (x, y, z) | Size (m) | Target | Map change | Teleport_to (dist m) | Return loader (m) | Detail src/tgt | Class |',
         '|---:|---|---|---|---|---|---|---|---|---|---|']
    for i, r in enumerate(cat, 1):
        sz = ('r %.0f' % r['radius']) if r['radius'] else ' x '.join('%.0f' % x for x in r['size'] if x is not None)
        tp = '-' if not r['teleports'] else '%s (%s)' % (r['teleports'][0], r['tele_dist'])
        rl = '-' if not r['return_loader'] else '%.0f' % r['return_loader']['dist']
        L.append('| %d | %s | %s | %s | %s | %s | %s | %s | %s | %d/%d | %s |' % (
            i, short(r['block']), r['name'], '(%.0f, %.0f, %.0f)' % tuple(r['pos']), sz, short(r['target']),
            'yes' if r['map_change'] else '', tp, rl, r['dens_src'], r['dens_tgt'], r['class']))
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(L) + '\n')


def main():
    a = sys.argv[1:]
    if a[0] == 'dump':
        here = os.path.dirname(os.path.abspath(__file__))
        os.execv(sys.executable, [sys.executable, '-I', os.path.join(here, 'dump_positions.py'), a[1]])
    if a[0] == 'catalog':
        D = json.load(open(a[1]))
        cat = catalog(D)
        o = a[a.index('-o') + 1] if '-o' in a else None
        if o:
            json.dump(cat, open(o, 'w'), indent=1)
        if '--md' in a:
            write_md(cat, a[a.index('--md') + 1])
        c = collections.Counter(r['class'] for r in cat)
        print('volumes', len(cat), dict(c), 'actions', sum(len(r['targets']) for r in cat))
        for r in cat:
            print('%-8s %-12s %-58s -> %-12s tele=%s(%s) ret=%s det=%d/%d pos=%s' % (
                r['class'], short(r['block']), (r['name'] or '')[:58], short(r['target']),
                r['teleports'][0] if r['teleports'] else '-', r['tele_dist'],
                r['return_loader']['dist'] if r['return_loader'] else '-', r['dens_src'], r['dens_tgt'],
                [round(x) for x in r['pos']]))


if __name__ == '__main__':
    main()
