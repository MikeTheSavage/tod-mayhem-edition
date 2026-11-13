"""the police car prefab's track points and placeholder properties (read only)"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'data', 'lib')); sys.path.insert(0, os.path.join(ROOT, 'data'))
sys.dont_write_bytecode = True
import todblock as tb, todfrag as tf, fragedit as fe, fragtool as ft
from gameplay import bigger_fights as bf, options as opt
T = ft.types()
naz = tb.open_naz(r'C:\GOG Games\Total Overdose')
e = opt.SHARED + bf.PREFAB_BLOCK
pf = bf.load_prefab(naz.read(e), e, tb, tf, fe, ft, T)
d, p = pf['d'], pf['p']
seen = {}
for n in pf['points'] + [pf['vehicle'], pf['track']]:
    print(hex(n), p.base(n), d.get(n, 'name'))
    for r in p.sets.get(n, []):
        nm = T.name(r.prop)
        if nm in ('parent',):
            continue
        try:
            v = d.value(r) if hasattr(d, 'value') else tf.decode(r)
        except Exception as ex:
            v = r.raw.hex()[:40]
        print('    ', nm, v)
