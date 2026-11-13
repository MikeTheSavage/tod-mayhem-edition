"""Gang members near a mission's start teleport point (read only): which rival gangs stand close to where Ram starts.

  python -I start_area.py <game folder> <block substring> [radius_m]

Lists the block's teleport nodes (name contains TELEPORT) and, per teleport, the character placeholders of each gang
within radius (default 60 m), with their distance and folder (the parent node name)."""
import sys, os, re, math, collections
LAB = os.environ.get("TOD_AI_LAB", "")  # the author's AI tuning lab readers (not included)
sys.path.insert(0, LAB); sys.path.insert(0, os.path.join(LAB, 'proj'))
sys.dont_write_bytecode = True
import todblock as tb
import todfrag as tf

GROUP = {0: 'cartel', 1: 'cartel', 2: 'virgilio', 3: 'virgilio', 15: 'army', 24: 'mexpolice', 5: 'border', 9: 'dea',
         30: 'outlaw', 27: 'gangster', 22: 'elvez'}


def word(v):
    return v['i'] if isinstance(v, dict) and 'i' in v else v


def main():
    game, sub = sys.argv[1], sys.argv[2].lower()
    rad = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
    arch = tb.Archive(os.path.join(game, 'blocks.naz'))
    for name in sorted(arch.names):
        low = name.lower()
        if sub not in low or re.search(r'_(de|es|fr|it|uk)\.[a-z]+$', low):
            continue
        blk = tb.Block(arch.read(name), os.path.splitext(name)[1])
        info = bytes(blk.info); starts = tb.record_starts(info, blk.n)
        tele, mem = [], []
        for k, R in enumerate(starts):
            if tb.u32(info, R) != 4:
                continue
            try:
                ops, st = tf.walk(blk.payloads[k])
            except Exception:
                continue
            names = tf.entity_names(ops)
            props = collections.defaultdict(dict)
            for o in ops:
                if o['op'] == 'setprop':
                    props[o['nodeId']][o['idx']] = o['value']
            for nid, (typ, en) in names.items():
                p = props[nid]; pos = p.get(12)
                if not (isinstance(pos, list) and len(pos) == 3):
                    continue
                if en and 'TELEPORT' in en.upper():
                    tele.append((en, pos))
                if typ and typ.startswith('character_placeholder'):
                    fac = word(p.get(680, 9)); model = word(p.get(645, -1))
                    g = 'police' if fac == 14 else (GROUP.get(model) if fac == 9 else 'f%d' % fac)
                    par = word(p.get(7)); pn = names.get(par, (None, None))[1] if isinstance(par, int) else None
                    mem.append((g, pos, en, pn))
        print('==', name, len(tele), 'teleports', len(mem), 'placeholders')
        for tn, tp in tele:
            near = sorted((math.dist(tp, m[1]), m) for m in mem if math.dist(tp, m[1]) <= rad)
            cnt = collections.Counter(m[0] for d, m in near)
            print('  %-50s %s  %s' % (tn, ', '.join('%.0f' % v for v in tp), dict(cnt)))
            for d, m in near[:40]:
                print('      %5.1f m  %-9s %-40s folder %s' % (d, m[0], m[2], m[3]))


if __name__ == '__main__':
    main()
