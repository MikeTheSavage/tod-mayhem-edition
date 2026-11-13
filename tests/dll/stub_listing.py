"""stub_listing.py - disassembly of every hand-written stub / wrapper in the built TODGameplay.dll, for review.
usage: python -I stub_listing.py <TODGameplay.map> <dumpbin /disasm:nobytes output> <out.txt>"""
import re, sys
mp, dis, out = sys.argv[1:4]
syms = []
for line in open(mp, encoding="latin1"):
    m = re.match(r"\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{8})\s+f\s", line)
    if m: syms.append((int(m[2], 16), m[1]))
syms.sort()
want = [(a, n) for a, n in syms if re.search(r"Stub@@|ProcWrap@@|InitWrap@@|CarryC@@|^_TODGameplay_Test", n)]
ends = {}
for i, (a, n) in enumerate(syms):
    ends[a] = syms[i + 1][0] if i + 1 < len(syms) else a + 0x100
lines = [l.rstrip("\n") for l in open(dis, encoding="latin1")]
with open(out, "w", encoding="utf-8") as f:
    for a, n in want:
        f.write("\n==== %s  %08X\n" % (n, a))
        for l in lines:
            m = re.match(r"\s+([0-9A-F]{8}):\s+(.*)", l)
            if not m: continue
            x = int(m[1], 16)
            if a <= x < ends[a] and not m[2].startswith("int         3"):
                f.write("  %08X  %s\n" % (x, m[2]))
print("%d stubs listed -> %s" % (len(want), out))
