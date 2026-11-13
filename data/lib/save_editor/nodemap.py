"""Predict runtime node indices (creation order) for a block: the root scene/fragment nodes, then every
instantiated fragment's nodes in create order, nested fragments expanded when their 'fragment' property
record is applied (depth-first, in stream order)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kfrag, re
# Engine classes whose constructor creates extra (internal) entities right after themselves; measured from the
# stored-id gaps in every shipped fragment (Shadow 29/29 gaps of 2, Bullet 16/16 of 11, LensFlareManager 9/9 of 5).
EXTRA_NODES = {'shadow': 1, 'bullet': 10, 'lensflaremanager': 4}

def _cls(typestr):
    m = re.fullmatch(r'([^()]+)\(([^()]+)\)', typestr)
    return (m.group(2) if m else typestr).lower()

def build(frags, root, start_idx, external=None, mode='A', fallback=None):
    """frags: {name: blob} of the block; fallback: further {name: blob} dicts searched for fragments the block
    references but does not contain (e.g. Lever.fragment / PKS_HighMounted.fragment live in overdose.main and are
    instantiated into submap blocks)."""
    short = {}
    allf = dict(frags)
    for fb in (fallback or []):
        for nm, b in fb.items():
            allf.setdefault(nm, b)
    for nm in list(frags) + [n for fb in (fallback or []) for n in fb]:
        short.setdefault(nm.split('/')[-1].lower(), nm)
    dec = {}
    def getdec(nm):
        if nm not in dec: dec[nm] = kfrag.decode(allf[nm])
        return dec[nm]
    nodes = []          # list of (runtime_idx, typestr, fragment, stored_id, name)
    names = {}
    counter = [start_idx]
    def inst(nm, depth, parent_path):
        ops = getdec(nm)
        local = {}
        for o in ops:
            if o[0] == 'create':
                local[o[1]] = counter[0]
                nodes.append([counter[0], o[2], nm, o[1], None, parent_path])
                counter[0] += 1 + EXTRA_NODES.get(_cls(o[2]), 0)
        byid = {n[3]: n for n in nodes[len(nodes) - len(local):]} if local else {}
        pend = []
        for o in ops:
            if o[0] == 'set' and o[3] == 'name' and o[1] in byid:
                byid[o[1]][4] = o[4] if isinstance(o[4], str) else (o[4] or {}).get('s')
            if o[0] == 'set' and o[3] == 'fragment' and o[4]:
                ref = o[4] if isinstance(o[4], str) else o[4]['s']
                r = short.get(ref.split('/')[-1].lower())
                if r is None:
                    if external is not None: external.append((nm, o[1], ref))
                    continue
                if mode == 'A':
                    inst(r, depth + 1, parent_path + '/' + (byid.get(o[1], [None]*5)[4] or hex(o[1])))
                else:
                    pend.append((r, o[1]))
        for r, nid in pend:
            inst(r, depth + 1, parent_path + '/' + (byid.get(nid, [None]*5)[4] or hex(nid)))
    inst(root, 0, '')
    return nodes

if __name__ == '__main__':
    import savefile
    frags = savefile.block_frags(savefile.MAIN_BLOCK)
    nodes = build(frags, savefile.MAIN_SCENE, 5)
    print(len(nodes), 'block-1 nodes, last index', hex(nodes[-1][0]))
    for n in nodes[:int(sys.argv[1]) if len(sys.argv) > 1 else 80]:
        print(hex(n[0]), n[1], n[2].split('/')[-1], hex(n[3]), n[4], n[5])
