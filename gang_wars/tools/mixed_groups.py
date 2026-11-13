"""Where do two enemy model groups (all FactionID 9) stand close together? Read only.

  python -I mixed_groups.py <game folder> [range_m] [block-substring ...]

For every mission / submap block: character placeholders grouped by model (cartel = CMLong/CMShort, virgilio =
VB_Small/VB_Tall, army = Soldier, mexpolice, border, dea = DEA_Taskforce, outlaw = DEA_Outlaw, gangster, elvez =
ElvezGuard; police = FactionID 14 of any model), authored positions (builtin 12), and for each pair of groups the
number of cross-group placeholder pairs within range_m (default 25) and the closest distance. Positions are the
placement nodes in their fragment's frame (world for mission fragments)."""
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
    game = sys.argv[1]
    rng = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
    pats = [p.lower() for p in sys.argv[3:]]
    arch = tb.Archive(os.path.join(game, 'blocks.naz'))
    for name in sorted(arch.names):
        low = name.lower()
        if re.search(r'_(de|es|fr|it|uk)\.[a-z]+$', low) or not low.endswith(('.mission', '.submap')):
            continue
        if pats and not any(p in low for p in pats):
            continue
        blk = tb.Block(arch.read(name), os.path.splitext(name)[1])
        info = bytes(blk.info); starts = tb.record_starts(info, blk.n)
        members = []
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
                if not (typ and typ.startswith('character_placeholder')):
                    continue
                p = props[nid]
                fac = word(p.get(680, 9)); model = word(p.get(645, -1))
                g = 'police' if fac == 14 else (GROUP.get(model) if fac == 9 else None)
                pos = p.get(12)
                if g and isinstance(pos, list) and len(pos) == 3:
                    members.append((g, pos, en, tb.record_name(info, R).rsplit('/', 1)[-1]))
        groups = sorted(set(m[0] for m in members))
        if len(groups) < 2:
            continue
        cnt = collections.Counter(m[0] for m in members)
        print('%s  %s' % (os.path.basename(name), ' '.join('%s=%d' % kv for kv in sorted(cnt.items()))))
        for i, a in enumerate(groups):
            for b in groups[i + 1:]:
                A = [m for m in members if m[0] == a]; B = [m for m in members if m[0] == b]
                best = None; n = 0
                for x in A:
                    for y in B:
                        d = math.dist(x[1], y[1])
                        if d <= rng:
                            n += 1
                        if best is None or d < best[0]:
                            best = (d, x, y)
                print('   %-10s - %-10s pairs<=%.0fm %4d  closest %.1f m: %s (%s) / %s (%s) at %s' % (
                    a, b, rng, n, best[0], best[1][2], best[1][3], best[2][2], best[2][3],
                    ', '.join('%.1f' % v for v in best[1][1])))


if __name__ == '__main__':
    main()
