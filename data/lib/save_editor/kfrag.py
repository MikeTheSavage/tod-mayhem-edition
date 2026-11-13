"""Exact Kapow fragment/scene decoder (record stream), using the verified global property table."""
import sys, os, json, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ktypes
HERE = os.path.dirname(os.path.abspath(__file__))
GPROPS = json.load(open(os.path.join(HERE, 'data', 'global_properties.json')))
_PT = {}

def ptype(gid):
    if gid not in _PT:
        name, t = GPROPS[gid].rsplit(':', 1)
        _PT[gid] = (name, ktypes.parse_type(t))
    return _PT[gid]

def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def blocks(blob):
    size = len(blob)
    blk = (u32(blob, 0) + 0) & 0xFFFFFFFF
    seen = set()
    while 0 < blk and blk + 12 <= size and blk not in seen:
        seen.add(blk)
        nxt = (u32(blob, blk) + blk) & 0xFFFFFFFF
        rec_len = u32(blob, blk + 4)
        first = (u32(blob, blk + 8) + blk + 8) & 0xFFFFFFFF
        yield first, min(first + rec_len, size)
        blk = nxt

def decode(blob):
    ops = []
    for first, end in blocks(blob):
        n = (end - first) // 4
        w = struct.unpack_from('<%dI' % n, blob, first)
        i = 0
        while i + 2 <= n:
            nid, packed = w[i], w[i + 1]; i += 2
            hi = packed >> 24
            if hi == 0xFF:
                s, i = ktypes.read(w, i, ('string',))
                ops.append(('create', nid, s if isinstance(s, str) else s['s']))
            elif hi == 0xFE:
                ops.append(('delete', nid))
            else:
                gid = packed & 0xFFFFFF
                name, pt = ptype(gid)
                v, i = ktypes.read(w, i, pt)
                ops.append(('set', nid, gid, name, v))
        if i != n:
            raise ValueError('block not consumed exactly: %d/%d' % (i, n))
    return ops
