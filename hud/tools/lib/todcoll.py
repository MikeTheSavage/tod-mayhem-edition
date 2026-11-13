"""TOD (Kapow) collision decoder/encoder library.

Collision lives in MODEL records (asset type 3) of every asset block:
  pivot+0x44 -> CollisionList {vector<Shape*> (16 B), u8 orFlags, u8 andFlags, u16 0xBAAC}  (0x14 B)
  model+0x54 -> HullList {vector<Element*> (16 B), u32 accel (always 0 on disk; quadtree built at load)} (0x14 B)
                Element (0x1C B) = {Shape*, 6 runtime words (0 on disk)}
Shape type ids on disk = index in the engine's shape-class registry (FUN_0093d360):
  0 base/sphere 0x34, 1 OrientedBox 0x50, 2 ? 0x38, 3 ? 0x4C, 4 VertexCloud (convex hull) 0x54.
Only types 1 and 4 occur in the shipped data.
"""
import struct
SENT_VEC = 0xBAAFFA00

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def i32(b, o): return struct.unpack_from('<i', b, o)[0]
def f32(b, o): return struct.unpack_from('<f', b, o)[0]
def a4(x): return (x + 3) & ~3

def deref(info, fa):
    v = u32(info, fa)
    if v == 0 or v == 0x80000000:
        return None
    return (fa + (v & ~3)) & 0xFFFFFFFF

def surf_flags_byte(w):
    """Kapow collision flag byte (Solid=1 BulletProof=2 Opaque=4 CameraBlock=8 Trigger=16 NavBlock=32 Physics=64
    ActivateTriggers=128) from a SurfacePropFields word (bits 10..13 and 23..26), = FUN_008c4b80's inverse."""
    return ((w >> 10) & 0xF) | (((w >> 23) & 0xF) << 4)

def decode_faces(stream, bits, nentries):
    val = int.from_bytes(stream, 'little')
    term = (1 << bits) - 1
    faces, cur = [], []
    for k in range(nentries):
        x = (val >> (bits * k)) & term
        if x == term:
            faces.append(cur); cur = []
        else:
            cur.append(x)
    return faces, cur

def encode_faces(faces, bits):
    term = (1 << bits) - 1
    val, k = 0, 0
    for f in faces:
        for x in f + [term]:
            val |= x << (bits * k); k += 1
    nbytes = (bits * k + 7) >> 3
    return val.to_bytes(nbytes, 'little'), k

def read_hull(info, sp):
    """Decode a type-4 VertexCloud shape at info offset sp. Returns a dict (semantic + raw keep-fields)."""
    t = u32(info, sp)
    assert t == 4
    w2c = u32(info, sp + 0x2C)
    nv = (w2c >> 8) & 0xFF; bits = (w2c >> 16) & 0xF; ne = w2c >> 20
    vp = deref(info, sp + 0x30)
    verts = [struct.unpack_from('<3f', info, vp + 12 * i) for i in range(nv)]
    h = dict(type=4,
             sphere=struct.unpack_from('<4f', info, sp + 4),
             extrude=struct.unpack_from('<3f', info, sp + 0x14),
             flags20=u32(info, sp + 0x20), surf=u32(info, sp + 0x24), x28=u32(info, sp + 0x28),
             lowbyte=w2c & 0xFF, bits=bits, verts=verts)
    # +0x34: ptr -> vector<u16>; +0x38: ptr -> vector<u32> (per-face surface words)
    for key, off, esz in (('u16list', 0x34, 2), ('facesurf', 0x38, 4)):
        p = deref(info, sp + off)
        if p is None:
            h[key] = None
        else:
            dp = deref(info, p); n = i32(info, p + 4)
            h[key] = dict(hdr=(u32(info, p + 8), u32(info, p + 12)),
                          data=[struct.unpack_from('<H' if esz == 2 else '<I', info, dp + esz * i)[0] for i in range(n)])
    # +0x3C: vector<u8> per-face material (facecoll.mat index, 0xFF = none)
    n = i32(info, sp + 0x40)
    mp = deref(info, sp + 0x3C)
    h['facemat'] = list(info[mp:mp + n]) if n else []
    h['facemat_hdr'] = (u32(info, sp + 0x44), u32(info, sp + 0x48))
    fp = deref(info, sp + 0x4C)
    stream = info[fp:fp + ((bits * ne + 7) >> 3)]
    faces, tail = decode_faces(stream, bits, ne)
    h['faces'] = faces
    assert not tail
    h['stream_raw'] = bytes(stream)
    ap = deref(info, sp + 0x50)
    if ap is None:
        h['aux'] = None
    else:
        aux = dict(raw=bytes(info[ap:ap + 0x54]))
        sl = i32(info, ap + 0x14); spn = deref(info, ap + 0x18)
        aux['name'] = info[spn:spn + sl].decode('latin-1') if (spn is not None and sl) else ''
        for vo in (0, 0x24, 0x34, 0x44):
            c = i32(info, ap + vo + 4)
            if c:
                dp = deref(info, ap + vo)
                aux['vec%x' % vo] = bytes(info[dp:dp + 16 * c])
        h['aux'] = aux
    return h

def read_box(info, sp):
    assert u32(info, sp) == 1
    return dict(type=1, raw=bytes(info[sp:sp + 0x50]),
                sphere=struct.unpack_from('<4f', info, sp + 4),
                flags20=u32(info, sp + 0x20), surf=u32(info, sp + 0x24),
                size=struct.unpack_from('<3f', info, sp + 0x28), center=struct.unpack_from('<3f', info, sp + 0x34),
                quat=struct.unpack_from('<4f', info, sp + 0x40))

def read_shape(info, sp):
    t = u32(info, sp)
    if t == 4: return read_hull(info, sp)
    if t == 1: return read_box(info, sp)
    raise ValueError('shape type %d' % t)


# ----------------------------------------------------------------------------------------------------------
# Region decode / encode (byte-exact).  A "region" is everything the engine's NativeSerializer emits for one
# collision list: the list object, the element pointer array, and every element with its children, in the
# serializer's allocation order (FUN_008c5370 / FUN_0085aba0 -> per element FUN_008c53e0 / FUN_00859ac0,
# shape payload FUN_008b4b20 for VertexCloud, FUN_008ad980 for OrientedBox).  All allocations are 4-aligned.
# Pointers on disk are self-relative (target = field + value) with no tag bits.  Pad bytes that the original
# exporter left uninitialised are captured in 'pads' so the round trip is exact; new data pads with 0.
# ----------------------------------------------------------------------------------------------------------

class Cur:
    """Read cursor: asserts the serializer's allocation order and records the pad bytes after each allocation."""
    def __init__(self, info, pos):
        self.info = info; self.pos = pos
    def take(self, off, n):
        assert off == self.pos, 'allocation order mismatch: expected %x got %x' % (self.pos, off)
        end = off + n
        pend = a4(end)
        pad = bytes(self.info[end:pend])
        self.pos = pend
        return pad

def _rd_vecobj(info, p):
    return dict(cnt=i32(info, p + 4), cap=u32(info, p + 8), flags=u32(info, p + 12))

def read_shape_region(info, sp, cur):
    """Decode one shape plus its children at sp, following allocation order with cursor cur."""
    t = u32(info, sp)
    if t == 1:
        cur.take(sp, 0x50)
        b = read_box(info, sp)
        b['obj'] = b.pop('raw')
        return b
    assert t == 4, 'shape type %d' % t
    cur.take(sp, 0x54)
    h = read_hull(info, sp)
    h['obj_keep'] = bytes(info[sp:sp + 0x54])     # scalar fields re-emitted from this; pointer/count words recomputed
    pads = {}
    cur.take(deref(info, sp + 0x30), 12 * len(h['verts']))
    for key, off, esz in (('u16list', 0x34, 2), ('facesurf', 0x38, 4)):
        p = deref(info, sp + off)
        if p is None:
            continue
        cur.take(p, 16)
        vo = _rd_vecobj(info, p)
        dp = deref(info, p)
        if vo['cnt']:
            pads[key] = cur.take(dp, esz * vo['cnt'])
        else:
            assert dp == cur.pos
        h[key]['vecobj'] = vo
    n = i32(info, sp + 0x40); mp = deref(info, sp + 0x3C)
    if n:
        pads['facemat'] = cur.take(mp, n)
    else:
        h['facemat_emptyptr'] = (mp - cur.pos) if mp is not None else None
    w2c = u32(info, sp + 0x2C); bits = (w2c >> 16) & 0xF; ne = w2c >> 20
    fp = deref(info, sp + 0x4C)
    pads['stream'] = cur.take(fp, (bits * ne + 7) >> 3)
    ap = deref(info, sp + 0x50)
    if ap is not None:
        cur.take(ap, 0x54)
        for vo in (0, 0x24, 0x34):
            dp = deref(info, ap + vo); c = i32(info, ap + vo + 4)
            assert c == 0 and dp == cur.pos, 'aux vector %x not empty' % vo
        sl = i32(info, ap + 0x14); spn = deref(info, ap + 0x18)
        if sl:                      # empty name: no allocation, raw words +0x18/+0x1C kept (8, 0x80000000)
            pads['auxstr'] = cur.take(spn, sl + 1)
            assert info[spn + sl] == 0
        dp = deref(info, ap + 0x44); c = i32(info, ap + 0x48)
        if c:
            pads['aux44'] = cur.take(dp, 16 * c)
        else:
            assert dp == cur.pos
    h['pads'] = pads
    return h

def read_region(info, lo, kind):
    """kind 'piv' (pivot+0x44 CollisionList) or 'f54' (model+0x54 HullList); lo = list object offset.
    Returns (decoded dict, end offset of the region)."""
    cur = Cur(info, lo)
    cur.take(lo, 0x14)
    hdr = dict(vec=_rd_vecobj(info, lo), w10=u32(info, lo + 0x10))
    n = hdr['vec']['cnt']
    ap = deref(info, lo)
    cur.take(ap, 4 * n)
    shapes = []
    for j in range(n):
        ep = deref(info, ap + 4 * j)
        if kind == 'f54':
            cur.take(ep, 0x1C)
            wr = bytes(info[ep + 4:ep + 0x1C])
            s = read_shape_region(info, deref(info, ep), cur)
            s['wrapper_tail'] = wr
        else:
            s = read_shape_region(info, ep, cur)
        shapes.append(s)
    return dict(kind=kind, hdr=hdr, shapes=shapes), cur.pos

class Out:
    def __init__(self, base):
        self.base = base; self.buf = bytearray()
    @property
    def pos(self):
        return self.base + len(self.buf)
    def alloc(self, n, pad=None):
        off = self.pos
        self.buf += bytes(n)
        need = a4(self.pos) - self.pos
        if need:
            self.buf += pad if (pad is not None and len(pad) == need) else bytes(need)
        return off
    def put(self, off, data):
        o = off - self.base
        self.buf[o:o + len(data)] = data
    def put_u32(self, off, v):
        self.put(off, struct.pack('<I', v & 0xFFFFFFFF))
    def put_ptr(self, field, target):
        self.put_u32(field, 0 if target is None else (target - field))

def new_hull_obj(sphere=(0.0, 0.0, 0.0, -1.0), extrude=(0.0, 0.0, 0.0), flags20=0xB8FFFFFF, surf=0x02007C00):
    """Scalar part of a fresh VertexCloud object (radius -1 = bounding sphere recomputed by the engine)."""
    keep = bytearray(0x54)
    struct.pack_into('<I4f3fIII', keep, 0, 4, *sphere, *extrude, flags20, surf, 0)
    struct.pack_into('<II', keep, 0x44, 0, SENT_VEC)
    return bytes(keep)

def write_shape(out, s):
    if s['type'] == 1:
        sp = out.alloc(0x50)
        out.put(sp, s['obj'])
        return sp
    h = s
    pads = h.get('pads', {})
    sp = out.alloc(0x54)
    out.put(sp, h.get('obj_keep') or new_hull_obj(h.get('sphere', (0, 0, 0, -1.0)), h.get('extrude', (0, 0, 0)),
                                                  h.get('flags20', 0xB8FFFFFF), h.get('surf', 0x02007C00)))
    stream, ne = encode_faces(h['faces'], h['bits'])
    raw = h.get('stream_raw')
    if raw is not None and len(raw) == len(stream) and stream:
        used = h['bits'] * ne - 8 * (len(stream) - 1)
        mask = (1 << used) - 1
        stream = stream[:-1] + bytes([(stream[-1] & mask) | (raw[-1] & ~mask & 0xFF)])
    nv = len(h['verts'])
    out.put_u32(sp + 0x2C, (h['lowbyte'] & 0xFF) | ((nv & 0xFF) << 8) | ((h['bits'] & 0xF) << 16) | ((ne & 0xFFF) << 20))
    vp = out.alloc(12 * nv)
    out.put(vp, b''.join(struct.pack('<3f', *v) for v in h['verts']))
    out.put_ptr(sp + 0x30, vp)
    for key, off, fmt in (('u16list', 0x34, '<H'), ('facesurf', 0x38, '<I')):
        L = h.get(key)
        if L is None:
            out.put_u32(sp + off, 0)
            continue
        n = len(L['data'])
        vo = L.get('vecobj', dict(cnt=n, cap=n, flags=SENT_VEC))
        p = out.alloc(16)
        out.put_ptr(sp + off, p)
        if n:
            dp = out.alloc(struct.calcsize(fmt) * n, pads.get(key))
            out.put(dp, b''.join(struct.pack(fmt, x) for x in L['data']))
        else:
            dp = out.pos
        out.put_ptr(p, dp)
        out.put(p + 4, struct.pack('<iII', n, vo['cap'], vo['flags']))
    n = len(h['facemat'])
    if n:
        mp = out.alloc(n, pads.get('facemat'))
        out.put(mp, bytes(h['facemat']))
    else:
        e = h.get('facemat_emptyptr', 0)
        mp = None if e is None else out.pos + e
    out.put_ptr(sp + 0x3C, mp)
    out.put_u32(sp + 0x40, n)
    if 'obj_keep' not in h:
        out.put_u32(sp + 0x44, n)
    fp = out.alloc(len(stream), pads.get('stream'))
    out.put(fp, stream)
    out.put_ptr(sp + 0x4C, fp)
    aux = h.get('aux')
    if aux is None:
        out.put_u32(sp + 0x50, 0)
        return sp
    ap = out.alloc(0x54)
    out.put(ap, aux['raw'])
    out.put_ptr(sp + 0x50, ap)
    for vo in (0, 0x24, 0x34):
        out.put_ptr(ap + vo, out.pos)
        out.put(ap + vo + 4, struct.pack('<ii', 0, 0))
    name = (aux.get('name') or '').encode('latin-1')
    if name:
        s_off = out.alloc(len(name) + 1, pads.get('auxstr'))
        out.put(s_off, name + b'\0')
        out.put_ptr(ap + 0x18, s_off)
        out.put(ap + 0x14, struct.pack('<i', len(name)))
        out.put(ap + 0x1C, struct.pack('<i', len(name)))
    else:                           # the engine's empty-string form: len 0, ptr word 8, cap word 0x80000000
        out.put(ap + 0x14, struct.pack('<iII', 0, 8, 0x80000000))
    v44 = aux.get('vec44', b'')
    c = len(v44) // 16
    if c:
        dp = out.alloc(len(v44), pads.get('aux44'))
        out.put(dp, v44)
    else:
        dp = out.pos
    out.put_ptr(ap + 0x44, dp)
    out.put(ap + 0x48, struct.pack('<ii', c, c))
    return sp

AUX_TEMPLATE = bytes(struct.pack('<IiiI', 0, 0, 0, SENT_VEC) + struct.pack('<IiII', 0xBAAD0100, 0, 0, 0)
                     + struct.pack('<I', 0xBAADF000) + struct.pack('<IiiI', 0, 0, 0, SENT_VEC) * 3)
assert len(AUX_TEMPLATE) == 0x54

def write_region(reg, base):
    """Encode a decoded region at absolute offset base (4-aligned). Returns bytes."""
    out = Out(base)
    lo = out.alloc(0x14)
    n = len(reg['shapes'])
    vec = reg['hdr']['vec']
    out.put(lo + 4, struct.pack('<iII', n, vec['cap'] if vec['cnt'] == n else n, vec['flags']))
    out.put_u32(lo + 0x10, reg['hdr']['w10'])
    ap = out.alloc(4 * n)
    out.put_ptr(lo, ap)
    for j, s in enumerate(reg['shapes']):
        if reg['kind'] == 'f54':
            ep = out.alloc(0x1C)
            out.put(ep + 4, s.get('wrapper_tail', bytes(24)))
            sp = write_shape(out, s)
            out.put_ptr(ep, sp)
        else:
            ep = write_shape(out, s)
        out.put_ptr(ap + 4 * j, ep)
    return bytes(out.buf)
