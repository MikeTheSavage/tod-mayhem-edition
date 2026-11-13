"""TOD (Kapow engine, GOG build) FRAGMENT codec: exact reader and writer for level/scene fragments.

Everything here follows the engine code (GOG TOD.exe, md5 d9beb351b3c78ee9f14a766a368f3ff9):

FRAGMENT asset (type 4) INFO record (0x28 bytes + name): +0x1C = 0x18, +0x20 = 0xFFFFFFE0 (FragmentData, relocated
by the loader into the payload), +0x24 flags (0xBAADF00D-filled; bit 1 set = never applied by
ApplyFragmentResource FUN_0085D990). The record never depends on the payload, so an edited fragment only changes
its payload (and the block's sizes table / max-payload word).

Payload = FragmentInfo (0x18 bytes) + chunks:
  +0x00 u32 self-relative offset to the first chunk (0x18), 0 for an empty fragment
  +0x04 0, +0x08 0x80000000, +0x0C 10240 (chunk capacity), +0x10 0, +0x14 editor leftover (kept verbatim)
  chunk: u32 next (self-relative, = 12 + rec_len; 0 on the last chunk), u32 rec_len (bytes of records),
         u32 first (self-relative from +8, always 4), then rec_len bytes of records. Chunks are contiguous.
  The editor fills a chunk while the next whole record still fits in 10240 bytes (verified on every shipped
  multi-chunk fragment); records never straddle chunks (FUN_00406320 jumps chunks only at a chunk end).
Record (FUN_0085D990): u32 nodeId, u32 packed; hi = packed >> 24:
  0xFF create: string (u32 word count + chars, NUL, pad) = entity type / script name -> DataType::LoadScript
  0xFE delete: no payload
  else set property index = packed & 0xFFFFFF; value = the property type's stream format (vtable +0x24):
    number/integer/truth/color: 1 word; vector 3; quaternion 4 (x,y,z,w)
    Entity: 1 word = nodeId, 0xFFFFFFFF = none (FUN_0086C980 -> FindEntityById FUN_00879740)
    string: u32 wc, wc words (wc = (len+4)//4; 0 = null string)            (FUN_0087A2A0 / FUN_00879F70)
    list(T): u32 n, n x T                                                     (FUN_008711A0)
    dict(T): u32 n, n x (string key, T)                                       (FUN_00868830)
    struct: fields in declaration order, no count                            (FUN_0087AFA0)
  The high byte of packed is the property index's bits 24..31 for set records (always 0 in shipped data).
nodeId: slot = id & 0xFF8FFFFF (low 20 bits), bits 20..22 = block index + 1 (0 = the block being loaded).
  Create records store the slot in AssetManager+0x1C8 and AddEntity FUN_00875FA0 puts the new entity in that
  slot of the CURRENT block's entity array (the block bits of a create record are ignored);
  set records and Entity values are looked up with FindEntityById (block bits honoured).
"""
import json, os, re, struct

HERE = os.path.dirname(os.path.abspath(__file__))
CHUNK_CAP = 10240
SIMPLE_WORDS = {'number': 1, 'integer': 1, 'truth': 1, 'color': 1, 'vector': 3, 'quaternion': 4, 'nothing': 0}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


# ----------------------------------------------------------------------------------------------- type system
class Types:
    """Global property table (proven by the block-header CRCs) + struct definitions."""

    def __init__(self, table=None, structs=None):
        t = json.load(open(table or os.path.join(HERE, 'property_table.json')))
        self.props = [tuple(x) for x in t['properties']]
        self.builtin = t['builtin']
        s = json.load(open(structs or os.path.join(HERE, 'structs.json')))
        self.structs = {k: [tuple(f) for f in v] for k, v in s['structs'].items()}
        self._parsed = {}
        self.by_name = {}
        for i, (n, ty) in enumerate(self.props):
            self.by_name[n.lower()] = i          # the engine's map: last registration wins (database over builtin)

    def index(self, name):
        if isinstance(name, int):
            return name
        if name.startswith('#'):
            return int(name[1:])
        return self.by_name[name.lower()]

    def name(self, idx):
        return self.props[idx][0] if 0 <= idx < len(self.props) else '#%d' % idx

    def ptype(self, idx):
        return self.props[idx][1]

    def parse(self, ty):
        if ty in self._parsed:
            return self._parsed[ty]
        m = re.fullmatch(r'(list|dict)\((.*)\)', ty, re.I)
        if m:
            r = (m.group(1).lower(), self.parse(m.group(2)))
        elif ty.lower() in SIMPLE_WORDS:
            r = ('simple', ty.lower())
        elif ty.lower() == 'string':
            r = ('string',)
        elif ty.lower() == 'entity':
            r = ('entity',)
        elif ty in self.structs:
            r = ('struct', ty, [(f, self.parse(ft)) for f, ft in self.structs[ty]])
        else:
            raise KeyError('unknown type %r' % ty)
        self._parsed[ty] = r
        return r


# ----------------------------------------------------------------------------------------------- values
def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def value_words(b, o, t):
    """Exact number of words the engine consumes for a value of parsed type t at byte offset o."""
    k = t[0]
    if k == 'simple':
        return SIMPLE_WORDS[t[1]]
    if k == 'entity':
        return 1
    if k == 'string':
        return 1 + u32(b, o)
    if k == 'list':
        n, w = u32(b, o), 1
        for _ in range(n):
            w += value_words(b, o + 4 * w, t[1])
        return w
    if k == 'dict':
        n, w = u32(b, o), 1
        for _ in range(n):
            w += 1 + u32(b, o + 4 * w)
            w += value_words(b, o + 4 * w, t[1])
        return w
    if k == 'struct':
        w = 0
        for _, ft in t[2]:
            w += value_words(b, o + 4 * w, ft)
        return w
    raise KeyError(k)


def decode_value(b, o, t):
    k = t[0]
    if k == 'simple':
        s = t[1]
        if s == 'number':
            return struct.unpack_from('<f', b, o)[0]
        if s == 'integer':
            return struct.unpack_from('<i', b, o)[0]
        if s == 'truth':
            v = u32(b, o)
            return bool(v) if v in (0, 1) else v
        if s == 'color':
            return '0x%08X' % u32(b, o)
        if s == 'vector':
            return list(struct.unpack_from('<3f', b, o))
        if s == 'quaternion':
            return list(struct.unpack_from('<4f', b, o))
        return None
    if k == 'entity':
        v = u32(b, o)
        return None if v == 0xFFFFFFFF else v
    if k == 'string':
        wc = u32(b, o)
        if wc == 0:
            return None
        raw = b[o + 4:o + 4 + 4 * wc]
        return raw.split(b'\0', 1)[0].decode('latin-1')
    if k == 'list':
        n, w, out = u32(b, o), 1, []
        for _ in range(n):
            out.append(decode_value(b, o + 4 * w, t[1]))
            w += value_words(b, o + 4 * w, t[1])
        return out
    if k == 'dict':
        n, w, out = u32(b, o), 1, []
        for _ in range(n):
            key = decode_value(b, o + 4 * w, ('string',)); w += 1 + u32(b, o + 4 * w)
            out.append([key, decode_value(b, o + 4 * w, t[1])]); w += value_words(b, o + 4 * w, t[1])
        return out
    if k == 'struct':
        w, out = 0, {}
        for fn, ft in t[2]:
            out[fn] = decode_value(b, o + 4 * w, ft)
            w += value_words(b, o + 4 * w, ft)
        return out
    raise KeyError(k)


def encode_string(s):
    if s is None:
        return struct.pack('<I', 0)
    raw = s.encode('latin-1') + b'\0'
    wc = (len(raw) + 3) // 4
    return struct.pack('<I', wc) + raw + bytes(4 * wc - len(raw))


def encode_value(v, t):
    k = t[0]
    if k == 'simple':
        s = t[1]
        if s == 'number':
            return struct.pack('<f', v)
        if s == 'integer':
            return struct.pack('<i', int(v)) if int(v) < 0x80000000 else struct.pack('<I', int(v))
        if s == 'truth':
            return struct.pack('<I', int(v))
        if s == 'color':
            return struct.pack('<I', int(v, 16) if isinstance(v, str) else int(v))
        if s == 'vector':
            return struct.pack('<3f', *v)
        if s == 'quaternion':
            return struct.pack('<4f', *v)
        return b''
    if k == 'entity':
        return struct.pack('<I', 0xFFFFFFFF if v is None else int(v))
    if k == 'string':
        return encode_string(v)
    if k == 'list':
        return struct.pack('<I', len(v)) + b''.join(encode_value(x, t[1]) for x in v)
    if k == 'dict':
        return struct.pack('<I', len(v)) + b''.join(encode_string(key) + encode_value(x, t[1]) for key, x in v)
    if k == 'struct':
        return b''.join(encode_value(v[fn], ft) for fn, ft in t[2])
    raise KeyError(k)


def mask_string_padding(b, o, t):
    """Copy of value bytes with the don't-care bytes (after a string's NUL) zeroed, for semantic comparison."""
    out = bytearray(b[o:o + 4 * value_words(b, o, t)])
    def walk(p, t):                        # p = offset inside out
        k = t[0]
        if k == 'string':
            wc = u32(out, p)
            if wc:
                body = out[p + 4:p + 4 + 4 * wc]
                z = body.find(b'\0')
                if z >= 0:
                    out[p + 4 + z + 1:p + 4 + 4 * wc] = bytes(4 * wc - z - 1)
            return 1 + wc
        if k in ('simple', 'entity'):
            return SIMPLE_WORDS.get(t[1], 1) if k == 'simple' else 1
        if k == 'list':
            n, w = u32(out, p), 1
            for _ in range(n):
                w += walk(p + 4 * w, t[1])
            return w
        if k == 'dict':
            n, w = u32(out, p), 1
            for _ in range(n):
                w += walk(p + 4 * w, ('string',))
                w += walk(p + 4 * w, t[1])
            return w
        if k == 'struct':
            w = 0
            for _, ft in t[2]:
                w += walk(p + 4 * w, ft)
            return w
    walk(0, t)
    return bytes(out)


# ----------------------------------------------------------------------------------------------- records
CREATE, DELETE, SET = 'create', 'delete', 'set'


class Record:
    __slots__ = ('node', 'op', 'prop', 'hi', 'raw')

    def __init__(self, node, op, prop=None, raw=b'', hi=0):
        self.node, self.op, self.prop, self.raw, self.hi = node, op, prop, raw, hi

    def packed(self):
        if self.op == CREATE:
            return 0xFF000000
        if self.op == DELETE:
            return 0xFE000000
        return (self.hi << 24) | (self.prop & 0xFFFFFF)

    def to_bytes(self):
        return struct.pack('<II', self.node, self.packed()) + self.raw

    def size(self):
        return 8 + len(self.raw)


def parse_records(b, start, end, types):
    recs, p = [], start
    while p < end:
        node, packed = u32(b, p), u32(b, p + 4)
        hi = packed >> 24
        q = p + 8
        if hi == 0xFF:
            n = 4 * (1 + u32(b, q))
            r = Record(node, CREATE, raw=b[q:q + n])
        elif hi == 0xFE:
            n = 0
            r = Record(node, DELETE)
        else:
            idx = packed & 0xFFFFFF
            t = types.parse(types.ptype(idx))
            n = 4 * value_words(b, q, t)
            r = Record(node, SET, idx, b[q:q + n], hi)
        recs.append(r)
        p = q + n
    if p != end:
        raise ValueError('record stream overran its chunk (%d != %d)' % (p, end))
    return recs


class Fragment:
    """header: the 24-byte FragmentInfo (verbatim); chunks: list of record lists (the shipped chunking)."""

    def __init__(self, payload, types):
        b = bytes(payload)
        self.types = types
        self.header = b[:0x18]
        self.chunks = []
        first = u32(b, 0)
        if first == 0:
            if len(b) != 0x18:
                raise ValueError('empty fragment with trailing bytes')
            return
        if first != 0x18:
            raise ValueError('first chunk not at 0x18')
        c = 0x18
        while True:
            nxt, rl, fr = struct.unpack_from('<III', b, c)
            if fr != 4:
                raise ValueError('chunk records not right after the chunk header')
            self.chunks.append(parse_records(b, c + 12, c + 12 + rl, types))
            if nxt == 0:
                if c + 12 + rl != len(b):
                    raise ValueError('bytes after the last chunk')
                break
            if nxt != 12 + rl:
                raise ValueError('chunks not contiguous')
            c += nxt

    @property
    def records(self):
        return [r for ch in self.chunks for r in ch]

    def rechunk(self, records=None):
        """Re-split records with the editor rule: fill a chunk while the next whole record fits in 10240 bytes."""
        recs = self.records if records is None else records
        chunks, cur, used = [], [], 0
        for r in recs:
            if cur and used + r.size() > CHUNK_CAP:
                chunks.append(cur); cur, used = [], 0
            cur.append(r); used += r.size()
        if cur:
            chunks.append(cur)
        self.chunks = chunks

    def serialize(self):
        out = bytearray(self.header)
        if not self.chunks:
            struct.pack_into('<I', out, 0, 0)
            return bytes(out)
        struct.pack_into('<I', out, 0, 0x18)
        for i, ch in enumerate(self.chunks):
            body = b''.join(r.to_bytes() for r in ch)
            nxt = 0 if i == len(self.chunks) - 1 else 12 + len(body)
            out += struct.pack('<III', nxt, len(body), 4) + body
        return bytes(out)

    # ------------------------------------------------------------------ entity view
    def entities(self):
        """Ordered dict nodeId -> {'type', 'props': {idx: [records]}} built from the record stream."""
        ents = {}
        for r in self.records:
            e = ents.setdefault(r.node, {'type': None, 'created': False, 'deleted': False, 'sets': []})
            if r.op == CREATE:
                e['type'] = decode_value(r.raw, 0, ('string',)); e['created'] = True
            elif r.op == DELETE:
                e['deleted'] = True
            else:
                e['sets'].append(r)
        return ents

    def value(self, r):
        return decode_value(r.raw, 0, self.types.parse(self.types.ptype(r.prop)))


def node_slot(node):
    return node & 0xFF8FFFFF


def node_block(node):
    return (node >> 20) & 7
