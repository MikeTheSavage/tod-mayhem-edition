"""Every entity of every block that sets FactionID (raw 680), by entity type and value (read only).

  python -I faction_users.py <game folder>"""
import sys, os, re, collections
LAB = os.environ.get("TOD_AI_LAB", "")  # the author's AI tuning lab readers (not included)
sys.path.insert(0, LAB); sys.path.insert(0, os.path.join(LAB, 'proj'))
sys.dont_write_bytecode = True
import todblock as tb
import todfrag as tf


def word(v):
    return v['i'] if isinstance(v, dict) and 'i' in v else v


def main():
    arch = tb.Archive(os.path.join(sys.argv[1], 'blocks.naz'))
    agg = collections.Counter(); where = collections.defaultdict(set)
    for name in sorted(arch.names):
        low = name.lower()
        if re.search(r'_(de|es|fr|it|uk)\.[a-z]+$', low) or not low.endswith(('.mission', '.submap', '.map', '.main', '.scene')):
            continue
        blk = tb.Block(arch.read(name), os.path.splitext(name)[1])
        info = bytes(blk.info); starts = tb.record_starts(info, blk.n)
        for k, R in enumerate(starts):
            if tb.u32(info, R) != 4:
                continue
            try:
                ops, st = tf.walk(blk.payloads[k])
            except Exception:
                continue
            names = tf.entity_names(ops)
            for o in ops:
                if o['op'] == 'setprop' and o.get('idx') == 680:
                    typ = (names.get(o['nodeId'], (None, None))[0] or '?').split('(')[0]
                    v = word(o['value'])
                    agg[(typ, v)] += 1; where[(typ, v)].add(os.path.basename(name))
    for (t, v), n in sorted(agg.items(), key=lambda x: (str(x[0][1]), x[0][0])):
        w = sorted(where[(t, v)])
        print('%-4s %-40s %5d  %s' % (v, t, n, ', '.join(w[:6]) + (' ...' if len(w) > 6 else '')))


if __name__ == '__main__':
    main()
