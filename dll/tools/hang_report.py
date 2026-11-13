"""hang_report.py - read a TODGameplay_hang_<date>_<time>.dmp (the [debug] hang_watchdog of TODGameplay.dll) or any
x86 minidump of TOD.exe and print every thread's stack against TOD.exe and TODGameplay.dll.

  python -I hang_report.py DUMP [--map DIR] [--exe TOD.exe] [--words N] [--thread TID] [--all]

  * the watchdog's own report (the dump's comment stream) first;
  * then each thread: what it is (its start address), state, EIP, and the return addresses found on its stack (a scan:
    every stack dword that points into a module's code right after a call instruction; some may be stale);
  * TODGameplay.dll / TODPhysics.dll addresses are named from TODGameplay.map / TODPhysics.map (--map DIR, else the
    dump's folder, else this repo's build folder); TOD.exe addresses get their VA and the start of the function they
    are in (scanned back to the padding before a prologue, like exe_dis.py); system DLLs get their nearest export.
  * the game thread (the one inside TOD.exe's main loop 0x43C4C2..0x43C53F) is printed first.
Code bytes come from the module files on disk (the dump has no code): --exe names TOD.exe if it moved.
"""
import os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dump_info import Dump, ctx_x86, u32  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LOOP_RETS = {0x43C4E2, 0x43C4ED, 0x43C4F6, 0x43C507, 0x43C516, 0x43C51B, 0x43C51D, 0x43C530, 0x43C53F}
KNOWN_TOD = {   # thread start routines / functions of TOD.exe worth a name (from the author's reverse-engineering notes)
    0x444830: "TOD worker thread (waits on 6 events)",
    0x43C4A0: "the main loop function (frame loop 0x43C4C2..0x43C53F)",
    0x8986E0: "UpdateLoadedBlocks_Impl",
    0x869E60: "Node::SendMessage",
    0x86A340: "Node::SendCommand",
}


# ------------------------------------------------------------------------------------------------ PE files on disk
class PE:
    def __init__(self, path):
        self.ok = False
        try:
            self.b = open(path, "rb").read()
        except OSError:
            return
        b = self.b
        if b[:2] != b"MZ":
            return
        pe = u32(b, 0x3C)
        if b[pe:pe + 4] != b"PE\0\0":
            return
        nsec = struct.unpack_from("<H", b, pe + 6)[0]
        opt = struct.unpack_from("<H", b, pe + 20)[0]
        self.pe32plus = struct.unpack_from("<H", b, pe + 24)[0] == 0x20B
        self.base = u32(b, pe + 24 + 28)
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + opt + 40 * i
            vs, va, rs, ro, ch = u32(b, o + 8), u32(b, o + 12), u32(b, o + 16), u32(b, o + 20), u32(b, o + 36)
            self.secs.append((va, max(vs, rs), ro, rs, ch))
        self.exports = []
        exp_rva = u32(b, pe + 24 + (112 if self.pe32plus else 96))
        if exp_rva:
            try:
                eo = self.off(exp_rva)
                nfun, nnames = u32(b, eo + 20), u32(b, eo + 24)
                funcs, names, ords = self.off(u32(b, eo + 28)), self.off(u32(b, eo + 32)), self.off(u32(b, eo + 36))
                for i in range(nnames):
                    no = self.off(u32(b, names + 4 * i))
                    nm = b[no:b.index(b"\0", no)].decode("ascii", "replace")
                    ordi = struct.unpack_from("<H", b, ords + 2 * i)[0]
                    if ordi < nfun:
                        self.exports.append((u32(b, funcs + 4 * ordi), nm))
                self.exports.sort()
            except (ValueError, struct.error, TypeError):
                self.exports = []
        self.ok = True

    def off(self, rva):
        for va, size, ro, rs, ch in self.secs:
            if va <= rva < va + size and rva - va < rs:
                return ro + rva - va
        return None

    def code(self, rva):
        for va, size, ro, rs, ch in self.secs:
            if va <= rva < va + size:
                return bool(ch & 0x20000000)
        return False

    def read(self, rva, n):
        o = self.off(rva)
        if o is None or rva < 0:
            return b""
        return self.b[o:o + n]

    def export_of(self, rva):
        best = None
        for r, nm in self.exports:
            if r <= rva:
                best = (r, nm)
            else:
                break
        return best if best and rva - best[0] < 0x8000 else None   # farther: an internal function, not this export

    def func_start(self, rva):   # back to the CC / C3 padding before a prologue (exe_dis.py's rule)
        for a in range(rva - 1, max(rva - 0x4000, 0x1000), -1):
            x = self.read(a - 1, 1)
            if x in (b"\xCC", b"\xC3") or (x == b"\x90" and self.read(a - 2, 1) in (b"\x90", b"\xCC", b"\xC3")):
                p = self.read(a, 3)
                if p[:3] == b"\x55\x8B\xEC" or p[:2] == b"\x6A\xFF" or p[:2] in (b"\x83\xEC", b"\x81\xEC") or \
                        p[:1] in (b"\x56", b"\x53", b"\x57", b"\x51", b"\x8B", b"\x64", b"\xA1", b"\x55"):
                    return a
        return None


def looks_like_call(b):   # b = the 7 bytes before the return address (gp_hang_logic.cpp looks_like_call)
    if len(b) < 7:
        return False
    if b[2] == 0xE8: return True
    if b[1] == 0xFF and b[2] == 0x15: return True
    if b[5] == 0xFF and (b[6] & 0xF8) == 0xD0: return True
    if b[5] == 0xFF and (b[6] & 0xF8) == 0x10 and (b[6] & 7) not in (4, 5): return True
    if b[4] == 0xFF and b[5] == 0x14 and (b[6] & 7) != 5: return True
    if b[4] == 0xFF and (b[5] & 0xF8) == 0x50 and (b[5] & 7) != 4: return True
    if b[3] == 0xFF and b[4] == 0x54: return True
    if b[1] == 0xFF and (b[2] & 0xF8) == 0x90 and (b[2] & 7) != 4: return True
    if b[0] == 0xFF and b[1] in (0x94, 0x14): return True
    return False


# ------------------------------------------------------------------------------------------------ map files
class MapFile:
    ROW = re.compile(r"^\s*0001:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8})\s+f\s+(?:i\s+)?(\S+)?")

    def __init__(self, path):
        self.syms, self.base, self.path, self.stamp = [], 0x10000000, path, None
        for line in open(path, encoding="ascii", errors="replace"):
            m = re.search(r"Timestamp is ([0-9a-fA-F]{1,8})", line)
            if m and self.stamp is None:
                self.stamp = int(m.group(1), 16)
                continue
            m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
            if m:
                self.base = int(m.group(1), 16)
                continue
            m = self.ROW.match(line)
            if m:
                self.syms.append((int(m.group(2), 16) - self.base, m.group(1), m.group(3) or ""))
        self.syms.sort()

    def find(self, rva):
        lo, hi = 0, len(self.syms) - 1
        if hi < 0 or rva < self.syms[0][0]:
            return None
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.syms[mid][0] <= rva:
                lo = mid
            else:
                hi = mid - 1
        return self.syms[lo]


def short_name(d):
    if d.startswith("?") and not d.startswith("??"):
        parts = []
        for p in d[1:].split("@"):
            if not p:
                break
            parts.append(p)
        return "::".join(reversed(parts)) if parts else d
    if d.startswith("_"):
        return d[1:].split("@")[0]
    return d


# ------------------------------------------------------------------------------------------------ report
class Namer:
    def __init__(self, d, mapdirs, exe):
        self.d, self.pe, self.maps, self.stale = d, {}, {}, []
        d.stamps = {}
        if 4 in d.streams:   # MINIDUMP_MODULE: BaseOfImage 0, SizeOfImage 8, CheckSum 12, TimeDateStamp 16
            r, _ = d.streams[4][0]
            for i in range(u32(d.b, r)):
                o = r + 4 + 108 * i
                d.stamps[struct.unpack_from("<Q", d.b, o)[0]] = u32(d.b, o + 16)
        for base, size, name, full in d.mods:
            low = name.lower()
            path = exe if (low == "tod.exe" and exe) else full
            if not os.path.exists(path):   # moved / removed: next to the dump, then the map folders (a build folder)
                for md in [os.path.dirname(os.path.abspath(d_path))] + mapdirs:
                    alt = os.path.join(md, name)
                    if os.path.exists(alt):
                        path = alt
                        break
            pe = PE(path)
            if pe.ok and pe.pe32plus:   # a WOW64 dump names System32; the 32-bit files are in SysWOW64
                alt = re.sub(r"(?i)\\system32\\", r"\\SysWOW64\\", path)
                pe = PE(alt) if alt != path else pe
            self.pe[base] = pe
            if low in ("todgameplay.dll", "todphysics.dll"):
                mn = os.path.splitext(name)[0] + ".map"
                for md in mapdirs:
                    p = os.path.join(md, mn)
                    if os.path.exists(p):
                        mf = MapFile(p)
                        stamp = d.stamps.get(base)
                        if stamp is not None and mf.stamp is not None and stamp != mf.stamp:
                            self.stale.append("%s (map %08X, module in the dump %08X)" % (p, mf.stamp, stamp))
                            continue   # a map of another build: wrong names, look further
                        self.maps[base] = mf
                        break

    def mod(self, va):
        for base, size, name, full in self.d.mods:
            if base <= va < base + size:
                return base, name
        return None, None

    def is_ret(self, v):
        base, name = self.mod(v)
        if base is None:
            return False
        pe = self.pe.get(base)
        if not pe or not pe.ok or not pe.code(v - base):
            return False
        return looks_like_call(pe.read(v - base - 7, 7))

    def name(self, va):
        base, name = self.mod(va)
        if base is None:
            return "0x%08X" % va
        off = va - base
        if base in self.maps:
            s = self.maps[base].find(off)
            if s:
                return "%s+0x%X  %s+0x%X (%s)" % (name, off, short_name(s[1]), off - s[0], s[2])
        pe = self.pe.get(base)
        if name.lower() == "tod.exe":
            if va in KNOWN_TOD:   # a known function's own start (a thread start routine)
                return "%s+0x%X  (0x%08X) %s" % (name, off, va, KNOWN_TOD[va])
            fs = pe.func_start(off) if pe and pe.ok else None
            known = KNOWN_TOD.get(base + fs) if fs is not None else None
            tail = ("  in %08X%s" % (base + fs, " " + known if known else "")) if fs is not None else ""
            return "%s+0x%X  (0x%08X)%s" % (name, off, va, tail)
        if pe and pe.ok and pe.exports:
            e = pe.export_of(off)
            if e:
                return "%s+0x%X  %s+0x%X" % (name, off, e[1], off - e[0])
        return "%s+0x%X" % (name, off)


def thread_infos(d):
    r = {}
    if 17 in d.streams:   # ThreadInfoListStream: header (size, entry size, count), MINIDUMP_THREAD_INFO entries
        o, _ = d.streams[17][0]
        hs, es, n = u32(d.b, o), u32(d.b, o + 4), u32(d.b, o + 8)
        for i in range(n):
            e = o + hs + es * i
            tid, flags = u32(d.b, e), u32(d.b, e + 4)
            kt, ut, start = struct.unpack_from("<QQQ", d.b, e + 32)
            r[tid] = dict(flags=flags, kernel_s=kt / 1e7, user_s=ut / 1e7, start=start)
    return r


def comment(d):
    for t in (10, 11):   # CommentStreamA / W
        if t in d.streams:
            o, n = d.streams[t][0]
            raw = d.b[o:o + n]
            return raw.decode("utf-16le" if t == 11 else "latin-1", "replace").rstrip("\0")
    return None


d_path = None


def main():
    global d_path
    a = sys.argv[1:]
    if not a or a[0].startswith("-"):
        sys.exit(__doc__)
    d_path = a[0]

    def opt(k, default=None):
        return a[a.index(k) + 1] if k in a else default
    words = int(opt("--words", "40"))
    only = int(opt("--thread", "0"))
    mapdirs = [x for x in (opt("--map"), os.path.dirname(os.path.abspath(d_path)), os.path.join(REPO, "build"), os.path.join(REPO, "dist")) if x]
    d = Dump(d_path)
    nm = Namer(d, mapdirs, opt("--exe"))
    print("dump %s: %d threads, %d modules" % (os.path.basename(d_path), len(d.threads), len(d.mods)))
    for base in nm.maps:
        print("  names for %s from %s (%d functions)" % (nm.mod(base)[1], nm.maps[base].path, len(nm.maps[base].syms)))
    for st in nm.stale:
        print("  skipped a map of another build: %s" % st)
    for base, size, name, full in d.mods:
        if name.lower() in ("todgameplay.dll", "todphysics.dll") and base not in nm.maps:
            print("  no map for %s (looked in %s): addresses only" % (name, ", ".join(mapdirs)))
    c = comment(d)
    if c:
        print("\n--- the watchdog's report (comment stream)")
        print(c.rstrip())
    infos = thread_infos(d)
    rows = []
    for t in d.threads:
        regs = ctx_x86(t["ctx"]) if len(t["ctx"]) >= 200 else {}
        # the stack bytes: the thread entry's own memory descriptor; some dbghelp versions leave its Rva 0 and put the
        # stacks in the memory list instead (the watchdog's dumps on Windows 11): read the range from there
        st = d.read(t["stack"], t["stack_size"]) if t["stack_off"] == 0 else None
        if st is None:
            st = d.b[t["stack_off"]:t["stack_off"] + t["stack_size"]]
        esp = regs.get("esp", t["stack"])
        start = max(esp - t["stack"], 0)
        start -= start % 4
        vals = [(t["stack"] + o, u32(st, o)) for o in range(start, len(st) - 3, 4)]
        game = any(v in LOOP_RETS for _, v in vals)
        rows.append((0 if game else 1, t, regs, vals, game))
    rows.sort(key=lambda r: r[0])
    for _, t, regs, vals, game in rows:
        if only and t["tid"] != only:
            continue
        inf = infos.get(t["tid"], {})
        what = "GAME THREAD (TOD.exe main loop)" if game else ""
        if inf.get("start"):
            what = (what + "; " if what else "") + "started at " + nm.name(inf["start"])
        print("\n=== thread %d%s" % (t["tid"], ("  " + what) if what else ""))
        if inf:
            print("    cpu: user %.2f s, kernel %.2f s" % (inf["user_s"], inf["kernel_s"]))
        if regs:
            print("    EIP %s" % nm.name(regs["eip"]))
            print("    " + " ".join("%s=%08X" % kv for kv in regs.items()))
        shown = 0
        for at, v in vals:
            if not nm.is_ret(v):
                continue
            print("    [%08X] %s" % (at, nm.name(v)))
            shown += 1
            if shown >= (words if (game or "--all" in a) else min(words, 16)):
                break
        if not shown:
            print("    (no return addresses on the stack)")


if __name__ == "__main__":
    main()
