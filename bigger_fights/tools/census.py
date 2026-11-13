"""Read-only census of the GOG blocks for Bigger Fights: enemy pool shells, placements per mission, AI vehicles."""
import os, sys, json, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'data', 'lib'))
sys.dont_write_bytecode = True
import todblock as tb, todfrag as tf, fragedit as fe, fragtool as ft

GOG = r'C:\GOG Games\Total Overdose'
T = ft.types()
a = tb.open_naz(GOG)
names = sorted(v[2] for v in a.table.values())
shared = [n for n in names if '/shared/' in n and os.path.splitext(n)[1] in ('.mission', '.main', '.submap', '.map')]

def load(entry):
    raw = a.read(entry)
    b = tb.Block(raw, os.path.splitext(entry)[1])
    frs, _ = ft.fragments(b)
    return raw, b, frs

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'main':
        raw, b, frs = load([n for n in names if n.endswith('shared/overdose.main')][0])
        print('blocks', len(frs), 'raw', len(raw), 'N', b.n)
        for k, nm in frs:
            if any(s in nm for s in ('Default', 'Resource', 'Enemy', 'AI', 'Character', 'Vehicle')):
                d = fe.FragDoc(b.payloads[k], T, nm)
                print(k, nm, len(b.payloads[k]), 'creates', len(d.created_ids()))
    elif cmd == 'frag':
        raw, b, frs = load([n for n in names if n.endswith(sys.argv[2])][0])
        for k, nm in frs:
            if sys.argv[3] in nm:
                d = fe.FragDoc(b.payloads[k], T, nm)
                print('==', nm, len(d.created_ids()))
                c = collections.Counter(d.type_of(n) for n in d.created_ids())
                print(c.most_common(40))
                lim = int(sys.argv[4]) if len(sys.argv) > 4 else 30
                for n in d.created_ids()[:lim]:
                    print(hex(n), d.type_of(n), d.path_of(n))
    elif cmd == 'ent':
        raw, b, frs = load([n for n in names if n.endswith(sys.argv[2])][0])
        for k, nm in frs:
            if sys.argv[3] in nm:
                d = fe.FragDoc(b.payloads[k], T, nm)
                lim = int(sys.argv[4]) if len(sys.argv) > 4 else 3
                for n in d.created_ids()[:lim]:
                    print(json.dumps(ft.entity_json(d, n), default=str))
    elif cmd == 'missions':
        rows = []
        for e in [n for n in shared if n.endswith('.mission')]:
            raw, b, frs = load(e)
            tot = hostile = big = buddy = vip = pas = 0
            fragsizes = []
            folders = collections.Counter()
            for k, nm in frs:
                d = fe.FragDoc(b.payloads[k], T, nm)
                for n in d.created_ids():
                    ty = d.type_of(n) or ''
                    if not ty.startswith('character_placeholder'):
                        continue
                    tot += 1
                    fac = d.get(n, 'FactionID', 0)
                    par = d.get(n, 'parent')
                    pty = d.type_of(par) if par else ''
                    if d.get(n, 'IsVIP'): vip += 1
                    if d.get(n, 'Is_Buddy'): buddy += 1
                    if (d.path_of(n).find('Passengers') >= 0): pas += 1
                    if fac == 9: hostile += 1
            rows.append((os.path.basename(e), len(raw), len(frs), tot, hostile, vip, buddy, pas))
        print('block rawsize frags placeholders faction9 vip buddy passengers')
        for r in rows: print(*r)
        print('total raw', sum(r[1] for r in rows))
    elif cmd == 'tree':
        raw, b, frs = load([n for n in names if n.endswith(sys.argv[2])][0])
        for k, nm in frs:
            if sys.argv[3] in nm:
                d = fe.FragDoc(b.payloads[k], T, nm)
                root = d.find(sys.argv[4])
                keys = sys.argv[5].split(',') if len(sys.argv) > 5 else []
                for n in d.subtree(root):
                    print(hex(n), d.type_of(n), d.path_of(n)[len(d.path_of(root)):], d.get(n, 'flags'), [ (p, d.get(n, p)) for p in keys if d.get(n, p) is not None])
    elif cmd == 'find':
        pat = sys.argv[3]
        for e in [n for n in shared if sys.argv[2] in n]:
            raw, b, frs = load(e)
            for k, nm in frs:
                d = fe.FragDoc(b.payloads[k], T, nm)
                for n in d.created_ids():
                    if pat in (d.type_of(n) or '') or pat in str(d.get(n, 'name')):
                        print(os.path.basename(e), os.path.basename(nm), hex(n), d.type_of(n), d.path_of(n)[:90]); 
    elif cmd == 'groups':
        stat = collections.Counter(); ex = {}
        for e in [n for n in shared if n.endswith('.mission') and sys.argv[2] in n]:
            raw, b, frs = load(e)
            for k, nm in frs:
                d = fe.FragDoc(b.payloads[k], T, nm)
                for n in d.created_ids():
                    ty = d.type_of(n) or ''
                    if not ty.startswith('character_placeholder') or d.get(n, 'FactionID', 0) != 9: continue
                    par = d.get(n, 'parent')
                    sib = d.children(par) if par else []
                    cnt = collections.Counter((d.type_of(s) or '').split('(')[0] for s in sib)
                    key = (((d.type_of(par) or 'none').split('(')[0]), cnt['character_placeholder'] if cnt['character_placeholder'] < 4 else '4+', tuple(sorted(c for c in cnt if c != 'character_placeholder')))
                    stat[key] += 1
                    ex.setdefault(key, (os.path.basename(e), d.path_of(n)[:80]))
        for k, v in stat.most_common(25): print(v, k, ex[k])
    elif cmd == 'phprops':
        c = collections.defaultdict(collections.Counter)
        keys = sys.argv[3].split(',')
        for e in [n for n in shared if n.endswith('.mission') and sys.argv[2] in n]:
            raw, b, frs = load(e)
            for k, nm in frs:
                d = fe.FragDoc(b.payloads[k], T, nm)
                for n in d.created_ids():
                    if (d.type_of(n) or '').startswith('character_placeholder') and d.get(n, 'FactionID', 0) == 9:
                        for p in keys: c[p][str(d.get(n, p))] += 1
        for p in keys: print(p, c[p].most_common(10))
