"""Read-only access to the textures inside the game's own archives, for the HD effects manifests.

A trimmed copy of the TOD HD texture tool's data layer (TOD HD's app\\tod_hd_game.py, same author): the archive
reader (a port of TOD.exe OpenZip 0x419100, names XOR-rotated in blocks.naz), the asset-block reader and the texture
record reader (GfxTexture fields, Texture::GetSizeForLevel 0x4633D0). Only the parts the manifest builder needs are
here; nothing writes anything.

`harvest(game_dir)` walks every block of every archive on the configpc.txt `filesystem =` line (zips first, the way
the game's FindFile picks them) and returns one row per DISTINCT level-0 texture byte string:
    {key, leaf, sha256, crc32, fmt, w, h, levels, prefix32, source}
which is exactly the identity the TOD HD d3d9.dll matches at SetTexture (description filter, the first 32 bytes of
level 0, then the SHA-256 or the CRC32 of level 0).
"""
import hashlib
import os
import struct
import zipfile
import zlib

SENTINEL = 0xBAADF00D
NULLPTR = 0x80000000
NAZ_SIG = 0x16ED5B50
MAP_PREFIX = 299
MAP_IDENTIFIER = b"DB851B2E-F452-47f7-9C7D-0BD4918F6F07"
BLOCK_EXTS = {".main": 0, ".mission": 0, ".submap": 0, ".playerdata": 0, ".map": MAP_PREFIX}
FMT_A8R8G8B8, FMT_DXT1 = 1, 7
FMT_NAME = {FMT_A8R8G8B8: "RGBA32", FMT_DXT1: "DXT1"}
DEFAULT_ARCHIVES = ["blocks.naz", "sounds.naz", "videos00.naz", "videos01.naz"]


class GameDataError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def a4(x):
    return (x + 3) & ~3


def a16(x):
    return (x + 15) & ~15


def selfptr(buf, off):
    v = u32(buf, off)
    if v == NULLPTR:
        return None
    return (off + v) & 0xFFFFFFFC


def cstring(buf, o, limit=1024):
    e = buf.find(b"\0", o, min(len(buf), o + limit))
    if e < 0:
        raise GameDataError("unterminated string at 0x%x" % o)
    return bytes(buf[o:e])


def size_for_level(w, h, level, compressed, bpp):
    """Texture::GetSizeForLevel 0x4633D0."""
    ww, hh = max(w >> level, 1), max(h >> level, 1)
    if compressed:
        return 8 * max(ww // 4, 1) * max(hh // 4, 1)
    return ((bpp & 0x3F) * hh * ww) >> 3


def texture_key(name):
    """'/data/Textures/x.bmp' -> 'data/textures/x' (case folded, extension dropped, '/ ' folded to '/')."""
    s = name.replace("\\", "/")
    i = s.lower().find("data/")
    if i >= 0:
        s = s[i:]
    s = s.lstrip("/")
    base, ext = os.path.splitext(s)
    if ext.lower() in (".bmp", ".dds", ".png", ".tga"):
        s = base
    return s.replace("/ ", "/").lower()


def tod_crc(b):
    """FUN_004657c0: the engine's file-name hash."""
    r = 0
    for c in b + b"\0\0\0\0":
        for bit in range(8):
            top = r & 0x80000000
            r = ((r << 1) & 0xFFFFFFFF) | ((c >> bit) & 1)
            if top:
                r ^= 0x04C11DB7
    return r


def engine_name(raw):
    b = raw.replace(b"\\", b"/")
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in b)


class Archive:
    """OpenZip (FUN_00419100): name -> (data offset, size). blocks.naz is a zip with rotated names."""

    def __init__(self, path):
        self.path = str(path)
        with open(self.path, "rb") as f:
            f.seek(0, 2)
            fsize = f.tell()
            f.seek(fsize - 22)
            e = f.read(20)
            self.naz = u32(e, 0) == NAZ_SIG
            count = e[10] | e[11] << 8
            cdsize, cdoff = u32(e, 12), u32(e, 16)
            f.seek(cdoff)
            cd = f.read(cdsize)
        table = {}
        p = 0
        for _ in range(count):
            q = p - 8 if self.naz else p
            size = i32(cd, q + 0x18)
            nlen, xlen, clen = struct.unpack_from("<HHH", cd, q + 0x1C)
            lhoff = i32(cd, q + 0x2A)
            raw = cd[p + 0x2E:p + 0x2E + nlen]
            if self.naz:
                raw = bytes(((c << 2) | (c >> 6)) & 0xFF for c in raw)
            name = engine_name(raw)
            table[tod_crc(name)] = (lhoff + xlen + 0x1E + nlen, size, name.decode("latin-1"))
            p += 0x2E + clen + xlen + nlen
        self.names = {v[2]: (v[0], v[1]) for v in table.values()}

    def read(self, name):
        off, size = self.names[name]
        with open(self.path, "rb") as f:
            f.seek(off)
            data = f.read(size)
        if len(data) != size:
            raise GameDataError("short read of %s from %s" % (name, self.path))
        return data


def texture_records(data, ext):
    """Yield (name, fmt, w, h, levels, payload) for every texture record of one asset block."""
    d = data
    base = BLOCK_EXTS[ext]
    if base and d[:36] != MAP_IDENTIFIER:
        raise GameDataError(".map identifier missing")
    if len(d) < base + 24:
        raise GameDataError("shorter than the header")
    _ts, _pc, _cc, n, s, _m = struct.unpack_from("<IIIiII", d, base)
    if not 0 <= n < 1_000_000:
        raise GameDataError("bad resource count %d" % n)
    info = d[base + 24:base + 24 + s]
    tbl = base + 24 + s
    sizes = list(struct.unpack_from("<%dI" % n, d, tbl))
    starts_payload, p = [], tbl + 4 * n
    for x in sizes:
        starts_payload.append(p)
        p += x
    if p != len(d):
        raise GameDataError("file length not accounted for")
    starts = []
    for o in range(0, len(info) - 15, 16):
        if u32(info, o + 12) != SENTINEL or u32(info, o) > 10:
            continue
        np_ = (o + 4 + u32(info, o + 4)) & 0xFFFFFFFC
        if np_ >= len(info) or info[np_] != 0x2F:
            continue
        try:
            nm = cstring(info, np_)
        except GameDataError:
            continue
        if all(32 <= c < 127 for c in nm):
            starts.append(o)
    if len(starts) != n:
        raise GameDataError("record scan found %d records, header says %d" % (len(starts), n))
    for k, R in enumerate(starts):
        if u32(info, R) != 0:
            continue
        name_ptr = (R + 4 + u32(info, R + 4)) & 0xFFFFFFFC
        name = cstring(info, name_ptr).decode("ascii")
        G = selfptr(info, R + 0x28)
        if G is None:
            continue
        w, h = u32(info, G + 0x14), u32(info, G + 0x18)
        fmt = i32(info, G + 0x1C)
        lw = u32(info, G + 0x24)
        levels = (lw >> 1) & 0xFF
        if fmt not in FMT_NAME:
            continue
        yield name, fmt, w, h, levels, d[starts_payload[k]:starts_payload[k] + sizes[k]]


def filesystem_archives(game_dir):
    """The archive names of the configpc.txt `filesystem =` line (the last one wins), else the default four."""
    cfg = os.path.join(game_dir, "configpc.txt")
    names = None
    if os.path.isfile(cfg):
        for ln in open(cfg, "rb").read().decode("latin-1").splitlines():
            s = ln.strip()
            if s.startswith("#") or "=" not in s:
                continue
            k, v = s.split("=", 1)
            if k.strip().lower() == "filesystem":
                names = [x.strip() for x in v.split(",") if x.strip()]
    return names or list(DEFAULT_ARCHIVES)


def _blocks_of(path):
    """(reader, [block names]) for one archive: a zip (an override pack) or a .naz."""
    if path.lower().endswith(".naz"):
        a = Archive(path)
        names = [n for n in a.names if os.path.splitext(n)[1].lower() in BLOCK_EXTS]
        return a.read, names
    zf = zipfile.ZipFile(path)
    names = [zi.filename for zi in zf.infolist() if os.path.splitext(zi.filename)[1].lower() in BLOCK_EXTS]
    return zf.read, names


def harvest(game_dir, archives=None, log=print, progress=None):
    """Every distinct level-0 texture of the archives the game reads. -> (rows, stats). Reads only."""
    archives = archives or filesystem_archives(game_dir)
    rows, stats = {}, dict(archives=[], blocks=0, records=0, errors=0)
    todo = []
    for an in archives:
        p = os.path.join(game_dir, an)
        if not os.path.isfile(p) or an.lower() in ("sounds.naz", "videos00.naz", "videos01.naz") or \
                an.lower().endswith("_sounds.naz"):
            continue
        try:
            rd, names = _blocks_of(p)
        except Exception as e:  # noqa: BLE001
            log("hdfx: %s not readable (%s), skipped" % (an, e))
            continue
        stats["archives"].append("%s (%d blocks)" % (an, len(names)))
        todo += [(an, rd, n) for n in names]
    for i, (an, rd, bn) in enumerate(todo):
        if progress:
            progress(i, len(todo))
        try:
            data = rd(bn)
            recs = list(texture_records(data, os.path.splitext(bn)[1].lower()))
        except Exception:  # noqa: BLE001  (a block we cannot read has no texture for us; counted)
            stats["errors"] += 1
            continue
        stats["blocks"] += 1
        for name, fmt, w, h, levels, payload in recs:
            stats["records"] += 1
            comp = fmt == FMT_DXT1
            n0 = size_for_level(w, h, 0, comp, 4 if comp else 32)
            if len(payload) < n0 or n0 < 32:
                continue
            l0 = payload[:n0]
            sh = hashlib.sha256(l0).hexdigest()
            key = texture_key(name)
            e = rows.get(sh)
            if e is None:
                rows[sh] = e = dict(key=key, keys=[key], leaf=key.split("/")[-1], sha256=sh,
                                    crc32="%08x" % (zlib.crc32(l0) & 0xFFFFFFFF), fmt=FMT_NAME[fmt], w=w, h=h,
                                    levels=levels, prefix32=bytes(l0[:32]).hex(), source=an, l0=None)
            elif key not in e["keys"]:
                e["keys"].append(key)
    if progress:
        progress(len(todo), len(todo))
    return list(rows.values()), stats


def read_level0(game_dir, keys, archives=None):
    """{key: [(fmt, w, h, level-0 bytes), ...]} of every distinct copy of the given texture keys (the sky faces), in
    one pass over the archives the game reads. Reads only."""
    archives = archives or filesystem_archives(game_dir)
    want = {k.lower() for k in keys}
    out, seen = {k: [] for k in want}, set()
    for an in archives:
        p = os.path.join(game_dir, an)
        if not os.path.isfile(p) or not (an.lower() == "blocks.naz" or an.lower().endswith(".zip")):
            continue
        rd, names = _blocks_of(p)
        for bn in names:
            try:
                recs = list(texture_records(rd(bn), os.path.splitext(bn)[1].lower()))
            except Exception:  # noqa: BLE001
                continue
            for name, fmt, w, h, levels, payload in recs:
                key = texture_key(name)
                if key not in want:
                    continue
                comp = fmt == FMT_DXT1
                n0 = size_for_level(w, h, 0, comp, 4 if comp else 32)
                l0 = bytes(payload[:n0])
                hsh = hashlib.sha256(l0).hexdigest()
                if (key, hsh) not in seen:
                    seen.add((key, hsh))
                    out[key].append((FMT_NAME[fmt], w, h, l0))
    return out
