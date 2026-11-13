"""disasm.py - read-only disassembly of the GOG TOD.exe (or any PE32) by virtual address.

usage: python -I disasm.py VA [COUNT]          disassemble COUNT instructions from VA (default 30)
       python -I disasm.py --bytes VA N        print N raw bytes at VA
       python -I disasm.py --range VA END      disassemble [VA, END)
Env TOD_EXE overrides the exe path (default C:/GOG Games/Total Overdose/TOD.exe). Opens the file read only.
"""
import os, struct, sys
import capstone

EXE = os.environ.get("TOD_EXE", r"C:/GOG Games/Total Overdose/TOD.exe")

class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.secs = []
        so = pe + 24 + optsz
        for i in range(nsec):
            name = d[so:so + 8].rstrip(b"\0").decode("latin1")
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, so + 8)
            self.secs.append((name, self.base + va, max(vsz, rsz), raw, rsz))
            so += 40

    def off(self, va):
        for name, sva, sz, raw, rsz in self.secs:
            if sva <= va < sva + sz:
                if va - sva >= rsz:
                    return None  # bss part
                return raw + (va - sva)
        return None

    def read(self, va, n):
        o = self.off(va)
        if o is None:
            raise ValueError("VA %08X not in a file-backed section" % va)
        return self.data[o:o + n]

def dis(pe, va, count=30, end=None):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = pe.read(va, (end - va) if end else count * 16)
    out = []
    for i, ins in enumerate(md.disasm(code, va)):
        if end is None and i >= count:
            break
        if end is not None and ins.address >= end:
            break
        out.append("%08X  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
    return out

if __name__ == "__main__":
    pe = PE(EXE)
    a = sys.argv[1:]
    if a and a[0] == "--bytes":
        print(pe.read(int(a[1], 16), int(a[2])).hex())
    elif a and a[0] == "--range":
        print("\n".join(dis(pe, int(a[1], 16), end=int(a[2], 16))))
    else:
        print("\n".join(dis(pe, int(a[0], 16), int(a[1]) if len(a) > 1 else 30)))
