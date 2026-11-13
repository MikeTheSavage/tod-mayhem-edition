"""TOD TEXT asset codec (type 2, '.txt', engine class TextAsset "textres", GOG TOD.exe md5 d9beb351b3c78ee9f14a766a368f3ff9).

Record in the block's info buffer (offsets from the record start R; pointers are self-relative: abs = field + (v & ~3)):
  +0x00 u32 type = 2        +0x04 selfrel name        +0x08 GlobalId        +0x0C 0xBAADF00D
  +0x10 u64 timestamp       +0x18 flags               +0x1C u32 24
  +0x20 List_1       {selfrel ptr, size, capacity, 0xBAAFFA00}  always empty (ptr = stream start)
  +0x30 TextIndicies {selfrel ptr, size, capacity, 0xBAAFFA00}  u16 byte offset of every entry in the stream
  +0x40 List_3       {selfrel ptr, size, capacity, 0xBAAFFA00}  Huffman bit stream (bytes)
  +0x50 selfrel -> Dictionary root      +0x54 0xBAADF00D
  +0x58 name, NUL, filler up to align16
  root: Dictionary nodes, 12 bytes each, written in pre-order (node, bit-0 subtree, bit-1 subtree):
        +0 selfrel m_Next (bit 0), +4 selfrel m_Prev (bit 1), +8 u16 contents (0x0A74 = internal node, else a
        UTF-16 code unit), +10 u16 0xBAAC. Internal: Next = 12 (child 0 follows), Prev -> child 1. Leaf: 0, 0.
  stream (directly after the last node), filler to align4, indices (u16 x n), filler to align16 = record end.
Entry k = key then value, both NUL-terminated, Huffman-coded LSB first, starting at byte indices[k] of the stream.
The engine decoder (GetGameString 0x861800 / GetCharacterInfo 0x861760) walks the tree from the root for every
character and stops when the low byte of the leaf is 0, so a code unit with low byte 0 (U+0100, U+2500, ...) ends
the string early; no length check is made (callers use 1024-unit buffers).
The tree is an optimal Huffman tree for the counts of all key and value characters plus two NULs per entry
(verified on all 75 shipped assets); the shipped tie-breaking is not reproduced, so the writer keeps the shipped
tree whenever it covers every character of the new text (byte-exact identity) and otherwise builds a new tree.
"""
import collections
import struct

INTERNAL = 0x0A74
NODE_PAD = 0xBAAC
LIST_TAIL = 0xBAAFFA00
MAX_UNITS = 1023            # 1024-unit buffers in every caller (0x6F89E0, 0x8A9E70, 0x8FD870, 0x8FD920, 0x8FE280)
MAX_STREAM_OFFSET = 0xFFFF  # entry offsets are u16


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def a4(x): return (x + 3) & ~3
def a16(x): return (x + 15) & ~15


class TextRecord:
    """One TEXT record (raw bytes) with its decoded tree and entries."""

    def __init__(self, raw):
        self.raw = r = bytes(raw)
        if u32(r, 0) != 2:
            raise ValueError('not a TEXT record (type %d)' % u32(r, 0))

        def sp(o):
            v = u32(r, o)
            return None if v == 0 else o + (v & ~3)
        self.l1_ptr, self.l1_n = sp(0x20), u32(r, 0x24)
        self.idx_ptr, self.idx_n = sp(0x30), u32(r, 0x34)
        self.st_ptr, self.st_n = sp(0x40), u32(r, 0x44)
        self.root = sp(0x50)
        np_ = sp(4)
        self.name = r[np_:r.index(b'\0', np_)].decode('latin-1')
        self.indices = list(struct.unpack_from('<%dH' % self.idx_n, r, self.idx_ptr))
        self.stream = r[self.st_ptr:self.st_ptr + self.st_n]
        self.nodes = 0
        self.tree = self._read_tree(self.root)

    def _read_tree(self, o):
        nx, pv, c, pad = struct.unpack_from('<IIHH', self.raw, o)
        self.nodes += 1
        if c == INTERNAL:
            return (self._read_tree(o + (nx & ~3)), self._read_tree(o + 4 + (pv & ~3)))
        return c

    def decode(self, k):
        """(key codes, value codes, end bit) of entry k, exactly as the engine walks it."""
        st = self.stream
        pos = self.indices[k] * 8
        res = []
        for _ in range(2):
            s = []
            while True:
                t = self.tree
                while isinstance(t, tuple):
                    t = t[(st[pos >> 3] >> (pos & 7)) & 1]
                    pos += 1
                if t & 0xFF == 0:
                    if t:
                        raise ValueError('entry %d: leaf %#x ends the string (low byte 0)' % (k, t))
                    break
                s.append(t)
            res.append(s)
        return res[0], res[1], pos

    def pairs(self):
        return [self.decode(k)[:2] for k in range(self.idx_n)]


def codes_to_str(cs):
    return ''.join(chr(c) for c in cs)


def str_to_codes(s):
    out = []
    for ch in s:
        c = ord(ch)
        if c > 0xFFFF:
            raise ValueError('character U+%X is outside the 16-bit range the engine stores' % c)
        out.append(c)
    return out


def tree_codes(tree):
    out = {}

    def rec(n, p):
        if isinstance(n, tuple):
            rec(n[0], p + [0])
            rec(n[1], p + [1])
        else:
            out[n] = p
    rec(tree, [])
    return out


def frequencies(pairs):
    f = collections.Counter()
    for k, v in pairs:
        f.update(k)
        f.update(v)
        f[0] += 2
    return f


def build_tree(freq):
    """Deterministic optimal Huffman tree (two queues: leaves sorted by (count, code), merged nodes in creation
    order; on equal weight the leaf goes first; first popped = bit 0)."""
    leaves = collections.deque(sorted((w, c) for c, w in freq.items() if w > 0))
    if len(leaves) == 1:
        w, c = leaves[0]
        return (c, 0 if c else 0x20)        # a tree needs an internal root; never happens with real text
    merged = collections.deque()

    def pop():
        if not merged or (leaves and leaves[0][0] <= merged[0][0]):
            return leaves.popleft()
        return merged.popleft()
    while len(leaves) + len(merged) > 1:
        x = pop()
        y = pop()
        merged.append((x[0] + y[0], (x[1], y[1])))
    return merged[0][1]


def serialize_tree(tree):
    out = bytearray()

    def rec(n):
        o = len(out)
        out.extend(b'\0' * 12)
        if isinstance(n, tuple):
            rec(n[0])
            p1 = len(out)
            rec(n[1])
            struct.pack_into('<IIHH', out, o, 12, p1 - (o + 4), INTERNAL, NODE_PAD)
        else:
            struct.pack_into('<IIHH', out, o, 0, 0, n, NODE_PAD)
    rec(tree)
    return bytes(out)


def encode_stream(pairs, tree):
    codes = tree_codes(tree)
    out = bytearray()
    offs = []
    for key, val in pairs:
        offs.append(len(out))
        bits = []
        for s in (key, val):
            for c in list(s) + [0]:
                bits.extend(codes[c])
        chunk = bytearray((len(bits) + 7) // 8)
        for i, b in enumerate(bits):
            if b:
                chunk[i >> 3] |= 1 << (i & 7)
        out += chunk
    return bytes(out), offs


def tree_covers(tree, pairs):
    have = set(tree_codes(tree))
    need = set(frequencies(pairs))
    return need <= have, sorted(need - have)


def build_record(template, pairs, tree=None):
    """New record bytes: template's header and name area kept, tree/stream/indices rebuilt.
    tree=None: keep the template tree if it covers every character, else build a new optimal tree.
    Returns (bytes, info dict)."""
    note = {}
    if tree is None:
        ok, missing = tree_covers(template.tree, pairs)
        if ok:
            tree = template.tree
            note['tree'] = 'shipped tree kept'
        else:
            tree = build_tree(frequencies(pairs))
            note['tree'] = 'new tree (characters not in the shipped tree: %s)' % ' '.join('U+%04X' % c for c in missing)
    tr = serialize_tree(tree)
    stream, offs = encode_stream(pairs, tree)
    if offs and offs[-1] > MAX_STREAM_OFFSET:
        raise ValueError('text too long for one asset: entry %d starts at stream byte %d (offsets are u16, max %d)'
                         % (len(offs) - 1, offs[-1], MAX_STREAM_OFFSET))
    root = template.root
    st = root + len(tr)
    ix = a4(st + len(stream))
    end = a16(ix + 2 * len(offs))
    rec = bytearray(end)
    rec[:root] = template.raw[:root]
    rec[root:st] = tr
    rec[st:st + len(stream)] = stream
    struct.pack_into('<%dH' % len(offs), rec, ix, *offs)
    og = template.raw[template.st_ptr + template.st_n:template.idx_ptr]     # uninitialised filler: keep when same size
    if len(og) == ix - (st + len(stream)):
        rec[st + len(stream):ix] = og
    ot = template.raw[template.idx_ptr + 2 * template.idx_n:]
    if len(ot) == end - (ix + 2 * len(offs)):
        rec[ix + 2 * len(offs):end] = ot

    def setlist(field, ptr, n):
        struct.pack_into('<IIII', rec, field, ptr - field, n, n, LIST_TAIL)
    setlist(0x20, st, 0)
    setlist(0x30, ix, len(offs))
    setlist(0x40, st, len(stream))
    struct.pack_into('<I', rec, 0x50, root - 0x50)
    note.update(nodes=len(tr) // 12, stream_bytes=len(stream), entries=len(offs), record_bytes=end)
    return bytes(rec), note


def check_entry(key, val):
    """Engine limits for one entry. Returns (errors, warnings)."""
    err, warn = [], []
    for what, s in (('key', key), ('value', val)):
        for c in s:
            if c == 0 or c & 0xFF == 0:
                err.append('%s has U+%04X: the decoder stops at a code unit whose low byte is 0' % (what, c))
                break
        if len(s) > MAX_UNITS:
            err.append('%s is %d units; the engine buffers hold %d plus the NUL' % (what, len(s), MAX_UNITS))
    if any(c > 0x7F for c in key):
        warn.append('key has non-ASCII characters (the identifier path casts to 8 bits)')
    return err, warn


def find_text_records(info, starts):
    out = []
    for i, R in enumerate(starts):
        if u32(info, R) == 2:
            end = starts[i + 1] if i + 1 < len(starts) else len(info)
            out.append((i, R, end, TextRecord(info[R:end])))
    return out
