"""mission_dict.py - the game's MissionDict: mission numbers, names and follow-ups (read only)."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'data', 'lib'))
sys.path.insert(0, os.path.join(ROOT, 'data'))
sys.dont_write_bytecode = True
import todblock as tb, todfrag as tf, fragedit as fe, fragtool as ft  # noqa: E402
from gameplay import options as opt  # noqa: E402

T = ft.types()
naz = tb.open_naz(r'C:\GOG Games\Total Overdose')
raw = naz.read(opt.MAIN)
b = tb.Block(raw, os.path.splitext(opt.MAIN)[1])
frs, _ = ft.fragments(b)
for k, nm in frs:
    if 'missiondict' in nm.lower():
        d = fe.FragDoc(b.payloads[k], T, nm)
        for i, n in enumerate(d.created_ids()):
            print(i, hex(n), d.get(n, 'name'), {p: d.get(n, p) for p in ('FollowUpMission', 'MissionNumber', 'Mission_Number', 'Type') if T.index(p) is not None} if False else '', [ (T.name(r.prop), d.value(r) if hasattr(d, 'value') else None) for r in d.records if r.op == tf.SET and r.node == n][:8] if i < 3 else '')
