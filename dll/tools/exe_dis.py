# exe_dis.py - disassemble TOD.exe (read only) around addresses: for each return address the call before it and the
# function start (scanned back to the CC/C3 padding before a prologue). Usage:
#   python -I exe_dis.py <TOD.exe> ret <va> [<va> ...]      call site + function start of each return address
#   python -I exe_dis.py <TOD.exe> dis <va> <count>          <count> instructions from va
import struct, sys
import capstone

def load(path):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    opt = struct.unpack_from('<H', b, pe + 20)[0]
    base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        va, = struct.unpack_from('<I', b, o + 12); vs, = struct.unpack_from('<I', b, o + 8)
        rs, ro = struct.unpack_from('<II', b, o + 16)
        secs.append((base + va, max(vs, rs), ro, rs))
    return b, secs

def rd(img, va, n):
    b, secs = img
    for sva, size, ro, rs in secs:
        if sva <= va < sva + size:
            o = va - sva
            return b[ro + o: ro + min(o + n, rs)]
    return b''

def func_start(img, va):
    for a in range(va - 1, va - 0x4000, -1):
        x = rd(img, a - 1, 1)
        if x in (b'\xCC', b'\xC3') or (x == b'\x90' and rd(img, a - 2, 1) in (b'\x90', b'\xCC', b'\xC3')):
            p = rd(img, a, 3)
            if p[:3] == b'\x55\x8B\xEC' or p[:2] == b'\x6A\xFF' or p[:2] in (b'\x83\xEC', b'\x81\xEC') or p[:1] in (b'\x56', b'\x53', b'\x57', b'\x51', b'\x8B', b'\x64', b'\xA1', b'\x55'):
                return a
    return None

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

def dis(img, va, count):
    code = rd(img, va, count * 15)
    out = []
    for i in md.disasm(code, va):
        out.append('  %08X  %-8s %s' % (i.address, i.mnemonic, i.op_str))
        if len(out) >= count: break
    return out

def call_before(img, ret):
    for back in (2, 3, 5, 6, 7):
        code = rd(img, ret - back, back)
        for i in md.disasm(code, ret - back):
            if i.address + i.size == ret and i.mnemonic == 'call':
                return '%08X call %s' % (i.address, i.op_str)
    return '(no call before %08X)' % ret

if __name__ == '__main__':
    img = load(sys.argv[1])
    if sys.argv[2] == 'ret':
        for a in sys.argv[3:]:
            v = int(a, 16)
            print('%08X: %s   func ~%s' % (v, call_before(img, v), '%08X' % func_start(img, v) if func_start(img, v) else '?'))
    else:
        print('\n'.join(dis(img, int(sys.argv[3], 16), int(sys.argv[4]))))
