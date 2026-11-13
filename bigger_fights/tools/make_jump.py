"""make_jump.py - test zips for the bigger-fights proof runs: the bigger_fights data + the gang-wars NEW GAME jump.

The gang-wars override zips make MM010 end 3 s after its start with a chosen follow-up mission. They are built on vanilla
blocks, so here their set records are re-applied on the bigger-fights blocks.

  python -I make_jump.py diff  OVERRIDE.zip                 what the override changes against the GOG blocks
  python -I make_jump.py merge BF.zip OVERRIDE.zip OUT.zip [--mission N]     N replaces the follow-up mission number
"""
import os, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'data', 'lib'))
sys.path.insert(0, os.path.join(ROOT, 'data'))
sys.dont_write_bytecode = True
import todblock as tb, todfrag as tf, fragedit as fe, fragtool as ft  # noqa: E402
from gameplay import builder  # noqa: E402

GOG = r'C:\GOG Games\Total Overdose'
T = ft.types()
naz = tb.open_naz(GOG)


def changes(entry, new):
    """[(fragment name, node, prop, raw, value)] of a block against the GOG block"""
    raw0 = naz.read(entry)
    ext = os.path.splitext(entry)[1]
    b0, b1 = tb.Block(raw0, ext), tb.Block(new, ext)
    frs, _ = ft.fragments(b0)
    out = []
    for k, nm in frs:
        if b0.payloads[k] == b1.payloads[k]:
            continue
        f0, f1 = tf.Fragment(b0.payloads[k], T), tf.Fragment(b1.payloads[k], T)
        if len(f0.records) != len(f1.records):
            raise SystemExit('%s %s: the record count changed: not a set-only override' % (entry, nm))
        for r0, r1 in zip(f0.records, f1.records):
            if r0.raw != r1.raw:
                out.append((nm, r1.node, r1.prop, r1.raw, f1.value(r1) if r1.op == tf.SET else None, f0.value(r0) if r0.op == tf.SET else None))
    return out


def main():
    cmd = sys.argv[1]
    if cmd == 'diff':
        z = zipfile.ZipFile(sys.argv[2])
        for i in z.infolist():
            for nm, node, prop, raw, new, old in changes(i.filename, z.read(i)):
                print(os.path.basename(i.filename), os.path.basename(nm), hex(node), T.name(prop), old, '->', new)
    elif cmd == 'merge':
        bf = zipfile.ZipFile(sys.argv[2]); ov = zipfile.ZipFile(sys.argv[3]); out = sys.argv[4]
        mission = int(sys.argv[sys.argv.index('--mission') + 1]) if '--mission' in sys.argv else None
        entries = {i.filename: bf.read(i) for i in bf.infolist()}
        for i in ov.infolist():
            e = i.filename
            base = entries.get(e) or naz.read(e)
            ext = os.path.splitext(e)[1]
            b = tb.Block(base, ext)
            frs, _ = ft.fragments(b)
            byname = {nm: k for k, nm in frs}
            docs = {}
            for nm, node, prop, raw, new, old in changes(e, ov.read(i)):
                k = byname[nm]
                d = docs.setdefault(k, fe.FragDoc(b.payloads[k], T, nm))
                hits = [r for r in d.records if r.op == tf.SET and r.node == node and r.prop == prop]
                if not hits:
                    raise SystemExit('no record %#x %s in %s' % (node, T.name(prop), nm))
                if mission is not None and T.name(prop).lower() in ('followupmission', 'next_mission', 'nextmission') :
                    raw = tf.encode_value(mission, T.parse(T.ptype(prop)))
                hits[-1].raw = raw
                d.modified = True
                d._gen += 1
            for k, d in docs.items():
                b.payloads[k] = d.payload()
            entries[e] = b.serialize()
        builder.write_zip(out, sorted(entries.items()), naz)
        print('wrote', out, len(entries), 'blocks')


if __name__ == '__main__':
    main()
