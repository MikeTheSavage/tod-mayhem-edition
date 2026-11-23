"""Compare hull_fuzz result files: original vs patched. Exit 1 if the patched builder hung, or returned a different
hull than the original where the original returned one (a NULL stays a NULL or the patched may give up on a case the
original built only by luck: counted and listed, but reported separately from a changed hull)."""
import sys

def load(p):
    d = {}
    for ln in open(p):
        a = ln.split()
        if len(a) == 5:
            d[int(a[0])] = (a[1], a[2], a[3], a[4])
    return d

o, p = load(sys.argv[1]), load(sys.argv[2])
hang_o = [k for k, v in o.items() if v[0] in ("HANG", "CRASH")]
hang_p = [k for k, v in p.items() if v[0] in ("HANG", "CRASH")]
same = diff = gaveup = 0
bad = []
for k, v in o.items():
    if v[0] in ("HANG", "CRASH"):
        continue
    w = p.get(k)
    if w is None:
        continue
    if v == w:
        same += 1
    elif v[0] == "HULL" and w[0] == "NULL":
        gaveup += 1
        bad.append((k, "orig built a hull, patched gave up", v, w))
    else:
        diff += 1
        bad.append((k, "different result", v, w))
print("cases %d: identical %d, differing %d, patched gave up where the original built %d; original hung/crashed on %d, patched hung/crashed on %d"
      % (len(o), same, diff, gaveup, len(hang_o), len(hang_p)))
for b in bad[:20]:
    print("  seed %d: %s  orig %s  patched %s" % b)
if hang_o:
    print("  seeds where the original hangs: %s%s" % (hang_o[:30], " ..." if len(hang_o) > 30 else ""))
sys.exit(1 if (hang_p or diff or gaveup) else 0)
