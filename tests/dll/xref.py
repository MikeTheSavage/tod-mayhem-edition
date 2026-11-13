"""xref.py - find rel32 call/jmp sites that target a VA, or 32-bit immediates/displacements equal to a VA (read only).
usage: python -I xref.py VA [--imm]"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import PE, EXE
pe = PE(EXE)
target = int(sys.argv[1], 16)
imm = "--imm" in sys.argv
for name, sva, sz, raw, rsz in pe.secs:
    if name != ".text":
        continue
    d = pe.data[raw:raw + rsz]
    for i in range(len(d) - 5):
        op = d[i]
        if op in (0xE8, 0xE9):
            rel = struct.unpack_from("<i", d, i + 1)[0]
            if sva + i + 5 + rel == target:
                print("%08X %s" % (sva + i, "call" if op == 0xE8 else "jmp"))
        if imm and struct.unpack_from("<I", d, i)[0] == target:
            print("%08X imm/disp at +%d" % (sva + i, 0))
