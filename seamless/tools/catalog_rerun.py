"""catalog_rerun.py - the transition catalogue of tools/seamless re-run on the game as installed with a zip

Every ActionType 21 loader volume of the blocks that hold loaders (the 18 of the research catalogue), classified by the
research tool's own rule (transitions.py catalog(), vendored next to this file from the research tools/seamless):
TELEPORT / WALK / ONEWAY / MISSION. Run twice: on blocks.naz alone (must reproduce the research catalogue: 175
volumes, 164 WALK, 7 TELEPORT, 3 MISSION, 1 ONEWAY) and with the built zip first (the game's lookup order), then the
rows that changed are listed.

  python -I catalog_rerun.py GAME_DIR ZIP [--md out.md] [--json out.json]
"""
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, 'data')
for p in (DATA, os.path.join(DATA, 'lib')):
    if p not in sys.path:
        sys.path.insert(0, p)
sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.append(HERE)                  # transitions.py is vendored here: no dependency on the research folder

import todblock as tb                      # noqa: E402
import fragtool as ft                      # noqa: E402
import fragedit as fe                      # noqa: E402
from gameplay import seamless as sm        # noqa: E402
import transitions as TR                   # noqa: E402  (the research tool's catalogue rule, vendored)

SHARED = sm.SHARED
LOADER_BLOCKS = [
    'mainmap_map01_dea_hq_us_border.map', 'mm030_smashthestash.mission', 'mm066_brakeintosafe.mission',
    'submap01_map01_dea_hq_us_border.submap', 'submap01_map02_lostoros_downtown.submap',
    'submap01_map03_lostoros_industrial.submap', 'submap01_map04_lostoros_train_harbour.submap',
    'submap01_map06_virgillos_farm.submap', 'submap01_map09_jungle.submap', 'submap02_map02_lostoros_downtown.submap',
    'submap02_map03_lostoros_industrial.submap', 'submap02_map04_lostoros_train_harbour.submap',
    'submap02_map06_virgillos_farm.submap', 'submap02_map09_jungle.submap', 'submap03_map02_lostoros_downtown.submap',
    'submap03_map03_lostoros_industrial.submap', 'submap04_map02_lostoros_downtown.submap',
    'submap04_map03_lostoros_industrial.submap']
# blocks the targets resolve to (transitions.block_of_folder needs them in D, loaders or not)
TARGET_ONLY = ['submap02_map01_dea_hq_us_border.submap', 'mainmap_map02_lostoros_downtown.map',
               'mainmap_map03_lostoros_industrial.map', 'mainmap_map04_lostoros_train_harbour.map',
               'mainmap_map06_virgillos_farm.map', 'mainmap_map09_jungle.map']


class Reader:
    def __init__(self, game_dir, first_zip=None):
        self.arch = []
        if first_zip:
            self.arch.append(tb.Archive(first_zip))
        self.arch.append(tb.Archive(os.path.join(game_dir, 'blocks.naz')))

    def read(self, entry):
        for a in self.arch:
            hit = a.find('/' + entry)
            if hit is not None and hit[2].lower() == entry.lower():
                with open(a.path, 'rb') as f:
                    f.seek(hit[0])
                    return f.read(hit[1]), os.path.basename(a.path)
        raise KeyError(entry)


def build_D(reader):
    T = ft.types()
    D, docs, src = {}, {}, {}
    for blk in LOADER_BLOCKS + TARGET_ONLY:
        e = SHARED + blk
        raw, src[blk] = reader.read(e)
        b = tb.Block(raw, os.path.splitext(e)[1])
        frs, _ = ft.fragments(b)
        vols = sm.scan_doc_volumes(e, b, frs, T, fe, ft) if blk in LOADER_BLOCKS else []
        loaders = []
        for v in vols:
            for a in v['actions']:
                loaders.append(dict(fragment=v['frag'].split('/')[-1], target=a['target'], teleport=a['teleport'] or None,
                                    root=dict(name=v['name'], type=v['type'], pos=v['pos'], size=v['size'],
                                              radius=v['radius'])))
        D[blk] = dict(loaders=loaders, ents=[])
        docs[blk] = (b, frs)
    # teleport destinations: named nodes (world positions), as sections.catalog does
    names = {l['teleport'] for r in D.values() for l in r['loaders'] if l['teleport']}
    if names:
        for blk, (b, frs) in docs.items():
            cache = {}
            for k, nm in frs:
                if len(b.payloads[k]) <= 24:
                    continue
                d = fe.FragDoc(b.payloads[k], T, nm)
                owner = None
                for n in d.created_ids():
                    nmv = d.get(n, 'name')
                    if nmv in names:
                        if owner is None:
                            owner = ft.owner_chain(b, frs, k, T, cache=cache)[0]
                        D[blk]['ents'].append(dict(f='', t='Node', c='node', name=nmv,
                                                   p=[round(x, 3) for x in d.world(n, owner)[0]], size=None))
    return D, src


def run(game_dir, zip_path):
    out = {}
    for label, z in (('vanilla', None), ('with_zip', zip_path)):
        D, src = build_D(Reader(game_dir, z))
        cat = TR.catalog(D)
        out[label] = dict(counts=dict(collections.Counter(r['class'] for r in cat)), rows=cat,
                          from_zip=sorted(b for b, s in src.items() if s != 'blocks.naz'))
    key = lambda r: (r['block'], r['name'], tuple(round(x, 1) for x in r['pos']))  # noqa: E731
    before = {(r['block'], r['name'], i): r for i, r in enumerate(out['vanilla']['rows'])}
    changed = []
    for i, (a, b) in enumerate(zip(out['vanilla']['rows'], out['with_zip']['rows'])):
        if a['block'] != b['block'] or a['name'] != b['name']:
            raise SystemExit('row order differs at %d' % i)
        if a['class'] != b['class'] or [round(x, 2) for x in a['pos']] != [round(x, 2) for x in b['pos']] or \
                a['teleports'] != b['teleports']:
            changed.append(dict(block=a['block'], name=a['name'], before=a['class'], after=b['class'],
                                pos_before=[round(x, 1) for x in a['pos']], pos_after=[round(x, 1) for x in b['pos']],
                                teleport_before=a['teleports'], teleport_after=b['teleports'],
                                return_before=(a['return_loader'] or {}).get('dist'),
                                return_after=(b['return_loader'] or {}).get('dist')))
    out['changed'] = changed
    del key, before
    return out


def md(out):
    L = ['# Transition catalogue re-run (tools/seamless/transitions.py rule)', '',
         '| | volumes | WALK | TELEPORT | ONEWAY | MISSION |', '|---|---:|---:|---:|---:|---:|']
    for lab in ('vanilla', 'with_zip'):
        c = out[lab]['counts']
        L.append('| %s | %d | %d | %d | %d | %d |' % (lab, sum(c.values()), c.get('WALK', 0), c.get('TELEPORT', 0),
                                                      c.get('ONEWAY', 0), c.get('MISSION', 0)))
    L += ['', 'Blocks read from the zip: ' + ', '.join(out['with_zip']['from_zip']), '',
          '| block | volume | class | centre before -> after | Teleport_to before -> after | return loader m |',
          '|---|---|---|---|---|---|']
    for c in out['changed']:
        L.append('| %s | %s | %s -> %s | %s -> %s | %s -> %s | %s -> %s |' % (
            c['block'], c['name'], c['before'], c['after'], tuple(c['pos_before']), tuple(c['pos_after']),
            ','.join(c['teleport_before']) or '-', ','.join(c['teleport_after']) or '-', c['return_before'], c['return_after']))
    return '\n'.join(L) + '\n'


if __name__ == '__main__':
    a = sys.argv[1:]
    o = run(a[0], a[1])
    text = md(o)
    print(text)
    if '--md' in a:
        open(a[a.index('--md') + 1], 'w', encoding='utf-8', newline='\n').write(text)
    if '--json' in a:
        json.dump(dict(counts={k: o[k]['counts'] for k in ('vanilla', 'with_zip')}, changed=o['changed']),
                  open(a[a.index('--json') + 1], 'w'), indent=1)
    ok = o['vanilla']['counts'] == {'WALK': 164, 'TELEPORT': 7, 'MISSION': 3, 'ONEWAY': 1} or \
        sum(o['vanilla']['counts'].values()) == 175
    sys.exit(0 if ok else 1)
