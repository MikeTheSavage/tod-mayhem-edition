"""where the vehicle folders of a built zip hang (read only)"""
import os, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'data', 'lib')); sys.path.insert(0, os.path.join(ROOT, 'data'))
sys.dont_write_bytecode = True
import todblock as tb, todfrag as tf, fragedit as fe, fragtool as ft
from gameplay.bigger_fights import FragPlan
T = ft.types()
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    if sys.argv[2] not in i.filename.lower():
        continue
    print(i.filename)
    b = tb.Block(z.read(i), os.path.splitext(i.filename)[1])
    frs, _ = ft.fragments(b)
    for k, nm in frs:
        d = fe.FragDoc(b.payloads[k], T, nm)
        p = FragPlan(d, tf)
        tops = [n for n in p.create if not p.parent.get(n, 0)]
        print(' ', nm, len(p.create), 'creates; roots', [(hex(n), p.base(n), d.get(n, 'name')) for n in tops[:6]])
        for n in p.create:
            nm2 = d.get(n, 'name') or ''
            if nm2.startswith('BFS'):
                par = p.parent.get(n, 0)
                print('    ', nm2, hex(n), 'parent', hex(par), p.base(par) if par else None, d.get(par, 'name') if par else None)
