"""Kapow script value types: parse type strings and read/write values in the engine's CopyNoAllocate
dword layout (save files, fragments). Struct layouts from work/struct_defs.json (exe machine code)."""
import json, struct, os
HERE = os.path.dirname(os.path.abspath(__file__))
_STRUCTS = None
FIXED = {'number': 1, 'integer': 1, 'truth': 1, 'entity': 1, 'color': 1, 'vector': 3, 'quaternion': 4}

def structs():
    global _STRUCTS
    if _STRUCTS is None:
        p = os.path.join(HERE, 'data', 'struct_defs.json')
        if not os.path.exists(p):
            p = os.path.join(HERE, '..', 'work', 'struct_defs.json')
        _STRUCTS = {k.lower(): (k, v) for k, v in json.load(open(p)).items()}
    return _STRUCTS

def parse_type(t):
    t = t.strip()
    tl = t.lower()
    if tl in FIXED or tl == 'string':
        return ('entity' if tl == 'entity' else tl,)
    if tl.startswith('list(') and t.endswith(')'):
        return ('list', parse_type(t[5:-1]))
    if tl.startswith('dict(') and t.endswith(')'):
        return ('dict', parse_type(t[5:-1]))
    s = structs().get(tl)
    if s:
        return ('struct', s[0], tuple((f['name'], parse_type(f['type'])) for f in s[1]))
    raise ValueError('unknown type %r' % t)

def type_str(pt):
    k = pt[0]
    if k == 'list': return 'list(%s)' % type_str(pt[1])
    if k == 'dict': return 'dict(%s)' % type_str(pt[1])
    if k == 'struct': return pt[1]
    return 'Entity' if k == 'entity' else k

def f32(v): return struct.unpack('<f', struct.pack('<I', v & 0xFFFFFFFF))[0]
def u32f(x): return struct.unpack('<I', struct.pack('<f', x))[0]

def read(w, i, pt):
    """Read a value of parsed type pt from dword list w at index i. Returns (value_json, new_index).
    value_json is lossless: floats keep their raw bits in 'raw' when they would not round-trip."""
    k = pt[0]
    if k in ('number',):
        return fnum(w[i]), i + 1
    if k in ('integer', 'color'):
        v = w[i]; return (v - (1 << 32) if v & 0x80000000 else v), i + 1
    if k == 'truth':
        return (w[i] if w[i] > 1 else bool(w[i])), i + 1
    if k == 'entity':
        return ent(w[i]), i + 1
    if k == 'vector':
        return [fnum(w[i]), fnum(w[i + 1]), fnum(w[i + 2])], i + 3
    if k == 'quaternion':
        return [fnum(w[i + j]) for j in range(4)], i + 4
    if k == 'string':
        n = w[i]
        if n == 0:
            return None, i + 1
        if n > 4096:
            raise ValueError('string too long %d' % n)
        raw = struct.pack('<%dI' % n, *w[i + 1:i + 1 + n])
        z = raw.find(b'\0')
        s = raw[:z].decode('latin-1')
        pad = raw[z:]
        out = {'s': s} if pad == b'\0' * len(pad) else {'s': s, 'pad': pad.hex()}
        if (len(s) + 4) // 4 != n:
            out['n'] = n
        return (out if len(out) > 1 else s), i + 1 + n
    if k == 'list':
        n = w[i]
        if n > 100000:
            raise ValueError('list too long %d' % n)
        i += 1; out = []
        for _ in range(n):
            v, i = read(w, i, pt[1]); out.append(v)
        return out, i
    if k == 'dict':
        n = w[i] & 0x7fffff; i += 1; out = []
        for _ in range(n):
            key, i = read(w, i, ('string',))
            v, i = read(w, i, pt[1]); out.append([key, v])
        return out, i
    if k == 'struct':
        out = {}
        for fname, ft in pt[2]:
            v, i = read(w, i, ft); out[fname] = v
        return out, i
    raise ValueError(k)

def fnum(v):
    x = f32(v)
    if x != x or u32f(x) != v:          # NaN / -0.0 / denormal edge: keep raw bits
        return {'f32': '%08x' % v}
    return x

def ent(v):
    if v == 0xFFFFFFFF: return None
    return 'E%06x' % v                  # E<block nibble><index 5 hex>: (block<<20)|index

def write(pt, v):
    k = pt[0]
    if k == 'number':
        if isinstance(v, dict): return [int(v['f32'], 16)]
        return [u32f(float(v))]
    if k in ('integer', 'color'):
        return [int(v) & 0xFFFFFFFF]
    if k == 'truth':
        return [int(v) & 0xFFFFFFFF]
    if k == 'entity':
        if v is None: return [0xFFFFFFFF]
        if isinstance(v, int): return [v]
        return [int(v[1:], 16)]
    if k == 'vector':
        return sum((write(('number',), x) for x in v), [])
    if k == 'quaternion':
        return sum((write(('number',), x) for x in v), [])
    if k == 'string':
        if v is None: return [0]
        if isinstance(v, str): s, pad, n = v, None, None
        else: s, pad, n = v['s'], v.get('pad'), v.get('n')
        b = s.encode('latin-1')
        if pad is not None:
            b = b + bytes.fromhex(pad)
        else:
            b = b + b'\0'
            b += b'\0' * ((-len(b)) % 4)
        nn = len(b) // 4
        out = [nn if n is None else n] + list(struct.unpack('<%dI' % nn, b))
        return out
    if k == 'list':
        out = [len(v)]
        for x in v: out += write(pt[1], x)
        return out
    if k == 'dict':
        out = [len(v)]
        for key, x in v: out += write(('string',), key) + write(pt[1], x)
        return out
    if k == 'struct':
        out = []
        for fname, ft in pt[2]: out += write(ft, v[fname])
        return out
    raise ValueError(k)
