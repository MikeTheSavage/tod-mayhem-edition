# dump_info.py - read a Windows minidump of TOD.exe (x86) without a debugger: exception, faulting thread context,
# modules, and a heuristic stack scan (every dword on the faulting thread's stack that points into a module's code is
# printed with module+offset). Usage: python -I dump_info.py <dmp> [--all-threads] [--words N]
import struct, sys

def u32(b, o): return struct.unpack_from('<I', b, o)[0]

class Dump:
    def __init__(s, path):
        s.b = open(path, 'rb').read()
        b = s.b
        assert b[:4] == b'MDMP', 'not a minidump'
        n, rva = u32(b, 8), u32(b, 12)
        s.streams = {}
        for i in range(n):
            t, size, r = struct.unpack_from('<III', b, rva + 12 * i)
            s.streams.setdefault(t, []).append((r, size))
        s.mem = []   # (start, size, file_off)
        if 9 in s.streams:   # Memory64ListStream
            r, _ = s.streams[9][0]
            cnt, = struct.unpack_from('<Q', b, r); base, = struct.unpack_from('<Q', b, r + 8)
            off = base
            for i in range(cnt):
                st, sz = struct.unpack_from('<QQ', b, r + 16 + 16 * i)
                s.mem.append((st, sz, off)); off += sz
        if 5 in s.streams:   # MemoryListStream
            r, _ = s.streams[5][0]
            cnt = u32(b, r)
            for i in range(cnt):
                st, = struct.unpack_from('<Q', b, r + 4 + 16 * i)
                sz, fo = struct.unpack_from('<II', b, r + 4 + 16 * i + 8)
                s.mem.append((st, sz, fo))
        s.mods = []
        if 4 in s.streams:
            r, _ = s.streams[4][0]
            cnt = u32(b, r)
            for i in range(cnt):
                o = r + 4 + 108 * i
                base, = struct.unpack_from('<Q', b, o); size = u32(b, o + 8); name_rva = u32(b, o + 20)
                ln = u32(b, name_rva); name = b[name_rva + 4:name_rva + 4 + ln].decode('utf-16le')
                s.mods.append((base, size, name.split('\\')[-1], name))
        s.threads = []
        if 3 in s.streams:
            r, _ = s.streams[3][0]
            cnt = u32(b, r)
            for i in range(cnt):
                o = r + 4 + 48 * i
                tid = u32(b, o); teb, = struct.unpack_from('<Q', b, o + 16)
                sst, = struct.unpack_from('<Q', b, o + 24); ssz, sfo = struct.unpack_from('<II', b, o + 32)
                csz, cfo = struct.unpack_from('<II', b, o + 40)
                s.threads.append(dict(tid=tid, teb=teb, stack=sst, stack_size=ssz, stack_off=sfo, ctx=b[cfo:cfo + csz]))

    def read(s, va, n):
        for st, sz, fo in s.mem:
            if st <= va and va + n <= st + sz: return s.b[fo + va - st: fo + va - st + n]
        return None

    def sym(s, va):
        for base, size, name, _ in s.mods:
            if base <= va < base + size: return '%s+0x%X' % (name, va - base)
        return None

def ctx_x86(c):
    # x86 CONTEXT: flags 0, dr0-7 4..28, floatsave 28..140, segs 140.., edi 156 esi 160 ebx 164 edx 168 ecx 172 eax 176
    # ebp 180 eip 184 cs 188 eflags 192 esp 196
    names = ['edi', 'esi', 'ebx', 'edx', 'ecx', 'eax', 'ebp', 'eip']
    r = {k: u32(c, 156 + 4 * i) for i, k in enumerate(names)}
    r['esp'] = u32(c, 196)
    return r

def main():
    d = Dump(sys.argv[1])
    words = 1200
    if '--words' in sys.argv: words = int(sys.argv[sys.argv.index('--words') + 1])
    if 7 in d.streams:
        r, _ = d.streams[7][0]
        arch = struct.unpack_from('<H', d.b, r)[0]
        print('arch', arch, '(0 = x86, 9 = x64)')
    exc_tid = None
    if 6 in d.streams:
        r, _ = d.streams[6][0]
        exc_tid = u32(d.b, r)
        code, flags = struct.unpack_from('<II', d.b, r + 8)
        addr, = struct.unpack_from('<Q', d.b, r + 24)
        nparam = u32(d.b, r + 32)
        params = struct.unpack_from('<15Q', d.b, r + 40)[:nparam]
        csz, cfo = struct.unpack_from('<II', d.b, r + 8 + 152)
        print('exception thread %d code %08X at %08X (%s) params %s' % (exc_tid, code, addr, d.sym(addr), [hex(p) for p in params]))
        ctx = d.b[cfo:cfo + csz]
        if csz >= 200:
            regs = ctx_x86(ctx)
            print('context', ' '.join('%s=%08X' % kv for kv in regs.items()))
    print('modules:')
    for base, size, name, full in d.mods:
        print('  %08X %08X %s' % (base, size, full))
    for t in d.threads:
        if exc_tid is not None and t['tid'] != exc_tid and '--all-threads' not in sys.argv: continue
        regs = ctx_x86(t['ctx']) if len(t['ctx']) >= 200 else {}
        print('\nthread %d stack %08X+%X  %s' % (t['tid'], t['stack'], t['stack_size'], ' '.join('%s=%08X' % kv for kv in regs.items())))
        st = d.b[t['stack_off']:t['stack_off'] + t['stack_size']]
        esp = regs.get('esp', t['stack'])
        if exc_tid == t['tid'] and 6 in d.streams:
            r, _ = d.streams[6][0]
            csz, cfo = struct.unpack_from('<II', d.b, r + 8 + 152)
            esp = ctx_x86(d.b[cfo:cfo + csz])['esp']
            print('  (exception context esp %08X)' % esp)
        start = max(esp - t['stack'], 0)
        shown = 0
        for o in range(start - start % 4, len(st) - 3, 4):
            v = u32(st, o)
            sm = d.sym(v)
            if sm and not sm.lower().startswith(('kernelbase', )) or (sm and 'kernelbase' in sm.lower()):
                if any(x in sm.lower() for x in ('.exe', '.dll')):
                    print('  [%08X] %08X %s' % (t['stack'] + o, v, sm))
                    shown += 1
            if shown >= words: break

if __name__ == '__main__':
    main()
