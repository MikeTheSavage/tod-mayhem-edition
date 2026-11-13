"""findop.py - list instructions whose operand text contains a substring (linear sweep of .text, read only).
usage: python -I findop.py "0xa3d891" [max]"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import PE, EXE
import capstone
pe = PE(EXE)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pat = sys.argv[1]; mx = int(sys.argv[2]) if len(sys.argv) > 2 else 40
n = 0
for name, sva, sz, raw, rsz in pe.secs:
    if name != ".text": continue
    code = pe.data[raw:raw + rsz]
    off = 0
    while off < len(code) and n < mx:
        got = False
        for ins in md.disasm(code[off:], sva + off):
            got = True
            if pat in ins.op_str:
                print("%08X  %-20s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str)); n += 1
                if n >= mx: break
            off = ins.address + ins.size - sva
        if not got: off += 1
