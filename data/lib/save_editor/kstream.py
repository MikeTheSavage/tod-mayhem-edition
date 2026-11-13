"""Parse the decompressed save payload (the engine's per-node property transaction stream).
Record = [H] value.  H = (idx<<16) | ((block-1)<<12) | prop   (bit31 = 0; block 1..6)
  prop == 0      : script-thread record  [flag] (+ [size][size-1 dwords] when flag != 0)
  0 < prop < T   : C++ class saved property (RegisterProperty slot id)
  prop >= T      : script member (prop - T) of the node's Scriptbaked
Values use the engine's CopyNoAllocate layout (ktypes)."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ktypes

def hdr(v):
    return (v >> 16) & 0x7FFF, ((v >> 12) & 0xF) + 1, v & 0xFFF

MAXIDX = 12000   # AssetManager reserves 11200/4100/6000/2800/../100 node slots per block list

def valid_hdr(v, maxidx=MAXIDX):
    if v & 0x80000000: return False
    idx, blk, prop = hdr(v)
    return 1 <= blk <= 6 and 0 < idx <= maxidx

def key(v):
    idx, blk, prop = hdr(v)
    return (blk, idx)

class Rec:
    __slots__ = ('pos', 'end', 'blk', 'idx', 'prop', 'name', 'type', 'value', 'kind', 'raw')
    def __init__(self, **k):
        for a in self.__slots__: setattr(self, a, k.get(a))

def order_ok(prev, v, T=None):
    """prev = (blk, idx, prop, phase) of the previous record; v = candidate header."""
    if prev is None: return True
    idx, blk, prop = hdr(v)
    pb, pi, pp, ph = prev
    if (blk, idx) > (pb, pi): return True
    if (blk, idx) < (pb, pi): return False
    # same node
    if prop == 0: return ph == 0
    if ph == 0: return prop > pp
    return prop > pp

def parse(w, resolver=None, maxidx=None, strict=False):
    """w: tuple of dwords. resolver(blk, idx) -> NodeType or None. Returns (records, problems)."""
    recs, problems = [], []
    i, n = 0, len(w)
    prev = None
    while i < n:
        v = w[i]
        if not valid_hdr(v) or not order_ok(prev, v):
            problems.append((i, 'bad header %08x after %s' % (v, prev)))
            if strict: break
            # resync: next plausible header
            j = i + 1
            while j < n and not (valid_hdr(w[j]) and order_ok(prev, w[j])): j += 1
            recs.append(Rec(pos=i, end=j, kind='junk', raw=list(w[i:j])))
            i = j; continue
        idx, blk, prop = hdr(v)
        if prop == 0 and w[i + 1] in (0, 1) and (w[i + 1] == 0 or 3 <= w[i + 2] < 20000):
            flag = w[i + 1]
            if flag:
                size = w[i + 2]
                end = i + 2 + size
                raw = list(w[i + 2:end])
                recs.append(Rec(pos=i, end=end, blk=blk, idx=idx, prop=0, kind='thread', value=flag, raw=raw))
            else:
                end = i + 2
                recs.append(Rec(pos=i, end=end, blk=blk, idx=idx, prop=0, kind='thread', value=0, raw=[]))
            prev = (blk, idx, 0, 1); i = end; continue
        nt = resolver(blk, idx) if resolver else None
        info = nt.prop(prop) if nt is not None else None
        if info is not None:
            name, pt, kind = info
            try:
                val, end = ktypes.read(w, i + 1, pt)
            except Exception as e:
                val, end = None, None
            ok = end is not None and end <= n and (end == n or (valid_hdr(w[end]) and order_ok((blk, idx, prop, 1 if kind == 'script' else 0), w[end])))
            if ok:
                recs.append(Rec(pos=i, end=end, blk=blk, idx=idx, prop=prop, name=name, type=ktypes.type_str(pt), value=val, kind=kind))
                prev = (blk, idx, prop, 1 if kind == 'script' else 0); i = end; continue
            problems.append((i, 'typed read failed blk%d idx%x prop%x %s:%s' % (blk, idx, prop, name, ktypes.type_str(pt))))
        # unknown size: delimit by the next plausible header
        cur = (blk, idx, prop, 1 if (nt is not None and prop >= nt.T) else 0)
        j = i + 1
        while j < n and not (valid_hdr(w[j]) and order_ok(cur, w[j])): j += 1
        recs.append(Rec(pos=i, end=j, blk=blk, idx=idx, prop=prop, kind='raw', raw=list(w[i + 1:j])))
        prev = cur; i = j
    return recs, problems
