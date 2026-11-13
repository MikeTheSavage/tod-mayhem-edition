"""Read-only access to TOD's archives and asset blocks (GOG build), plus an exact block writer.

Ported (read side) from TOD HD's app/tod_hd_game.py (Archive = OpenZip FUN_00419100, Block layout), which was checked
against the GOG TOD.exe and all 696 shipped blocks. Kept self-contained here so the anim codec tools never import from
TOD HD.

Block layout   [299-byte prefix for .map] + <IIIiII timestamp, propCk, cmdCk, N, S, M> + info[S] + sizes[N] + payloads
               (no padding, file ends after the last payload, M == max(sizes)).
Records        16-aligned in info, walked by the engine with each type's size pass (advance align16(size)).
"""
import os
import struct
import zlib

NAZ_SIG = 0x16ED5B50
MAP_PREFIX = 299
BLOCK_EXTS = {".main": 0, ".mission": 0, ".submap": 0, ".playerdata": 0, ".map": MAP_PREFIX, ".cutscene": 0}
SENTINEL = 0xBAADF00D


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def a4(x):
    return (x + 3) & ~3


def a16(x):
    return (x + 15) & ~15


def tod_crc(b):
    """FUN_004657c0: the engine's file-name hash (CRC-32 poly, LSB-first feed, 4 zero bytes appended)."""
    r = 0
    for c in b + b"\0\0\0\0":
        for bit in range(8):
            top = r & 0x80000000
            r = ((r << 1) & 0xFFFFFFFF) | ((c >> bit) & 1)
            if top:
                r ^= 0x04C11DB7
    return r


def engine_name(name):
    b = name.encode("latin-1") if isinstance(name, str) else name
    b = b.replace(b"\\", b"/")
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in b)


class Archive:
    """Port of OpenZip (FUN_00419100). table: {crc: (data offset, size, name)}; the last duplicate wins.
    entries: list of dicts with the central-directory fields (for rewriting)."""

    def __init__(self, path):
        self.path = str(path)
        with open(self.path, "rb") as f:
            f.seek(0, 2)
            self.fsize = fsize = f.tell()
            f.seek(fsize - 22)
            e = f.read(22)
            self.eocd = e
            self.naz = u32(e, 0) == NAZ_SIG
            count = e[10] | e[11] << 8
            self.cdsize, self.cdoff = u32(e, 12), u32(e, 16)
            f.seek(self.cdoff)
            cd = f.read(self.cdsize)
        self.cd = cd
        self.table, self.dups, self.entries = {}, 0, []
        p = 0
        for _ in range(count):
            q = p - 8 if self.naz else p
            crc32 = u32(cd, q + 0x10) if q + 0x10 >= 0 else None
            csize = i32(cd, q + 0x14) if q + 0x14 >= 0 else None
            size = i32(cd, q + 0x18)
            nlen, xlen, clen = struct.unpack_from("<HHH", cd, q + 0x1C)
            lhoff = i32(cd, q + 0x2A)
            raw = cd[p + 0x2E:p + 0x2E + nlen]
            if self.naz:
                raw = bytes(((c << 2) | (c >> 6)) & 0xFF for c in raw)
            name = engine_name(raw)
            crc = tod_crc(name)
            if crc in self.table:
                self.dups += 1
            data_off = lhoff + xlen + 0x1E + nlen
            self.table[crc] = (data_off, size, name.decode("latin-1"))
            self.entries.append({"cd_off": p, "name": name.decode("latin-1"), "size": size, "csize": csize,
                                 "crc32": crc32, "lhoff": lhoff, "nlen": nlen, "xlen": xlen, "clen": clen,
                                 "data_off": data_off})
            p += 0x2E + clen + xlen + nlen
        self.names = {v[2]: (v[0], v[1]) for v in self.table.values()}

    def read(self, name):
        off, size = self.names[name]
        with open(self.path, "rb") as f:
            f.seek(off)
            data = f.read(size)
        if len(data) != size:
            raise IOError("short read of %s" % name)
        return data

    def find(self, request):
        """FindFile: the first character of the request is dropped, A-Z lowercased."""
        return self.table.get(tod_crc(engine_name(request[1:])))


class Block:
    def __init__(self, data, ext):
        d = bytes(data)
        ext = ext.lower()
        base = BLOCK_EXTS[ext]
        self.ext, self.prefix = ext, d[:base]
        (self.timestamp, self.prop_ck, self.cmd_ck, n, s, self.max_asset) = struct.unpack_from("<IIIiII", d, base)
        tbl = base + 24 + s
        sizes = list(struct.unpack_from("<%dI" % n, d, tbl))
        p = tbl + 4 * n
        self.payloads = []
        for x in sizes:
            self.payloads.append(d[p:p + x])
            p += x
        if p != len(d):
            raise ValueError("file length not accounted for (%d != %d)" % (p, len(d)))
        if self.max_asset != (max(sizes) if n else 0):
            raise ValueError("maximumAssetSize %d != max(sizes)" % self.max_asset)
        self.info = bytearray(d[base + 24:base + 24 + s])
        self.n = n

    @property
    def sizes(self):
        return [len(x) for x in self.payloads]

    def serialize(self):
        n = len(self.payloads)
        out = bytearray(self.prefix)
        out += struct.pack("<IIIiII", self.timestamp, self.prop_ck, self.cmd_ck, n, len(self.info),
                           max(self.sizes) if n else 0)
        out += self.info
        out += struct.pack("<%dI" % n, *self.sizes)
        for p in self.payloads:
            out += p
        return bytes(out)


def is_record_head(info, o):
    if o + 16 > len(info) or u32(info, o + 12) != SENTINEL or u32(info, o) > 10:
        return False
    np_ = (o + 4 + u32(info, o + 4)) & 0xFFFFFFFC
    if np_ >= len(info) or info[np_] != 0x2F:
        return False
    e = info.find(b"\0", np_, min(len(info), np_ + 1024))
    if e < 0:
        return False
    return all(32 <= c < 127 for c in info[np_:e])


def record_starts(info, n):
    """Heuristic scan (TOD HD Block._records): every 16-aligned head. Must find exactly n with the first at 0."""
    starts = [o for o in range(0, len(info) - 15, 16) if is_record_head(info, o)]
    if len(starts) != n or (starts and starts[0] != 0):
        raise ValueError("record scan found %d records, header says %d" % (len(starts), n))
    return starts


def record_name(info, R):
    np_ = (R + 4 + u32(info, R + 4)) & 0xFFFFFFFC
    e = info.find(b"\0", np_)
    return info[np_:e].decode("latin-1")


def open_naz(game_dir=r"C:\GOG Games\Total Overdose"):
    return Archive(os.path.join(game_dir, "blocks.naz"))


def load_block(arch, name):
    return Block(arch.read(name), os.path.splitext(name)[1])
