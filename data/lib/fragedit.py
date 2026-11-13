"""Editable view of one TOD fragment (see todfrag.py for the format) plus the edit operations the labs need.

Operations (all keep every untouched record byte-identical; the stream is re-chunked with the editor rule):
  set_prop(node, prop, value)   change a property value, or add the set record if the entity has none for it
  move(node, pos=None, orient=None, space='local'|'world')
  clone(node, deep=True, parent=None, ...)   new entity (and descendants) from an existing one
  remove(node, deep=True)       drop an entity's records (refuses if something else still references it)

Engine facts used (GOG TOD.exe):
  - ApplyFragmentResource FUN_0085D990 is always called with the owner's id (FUN_0087F190 / FUN_0089A1A0), so a
    fragment's nodeIds are LOCAL keys: each create gets a fresh entity slot and the key -> entity map
    (AssetManager+0x1D0) resolves set records and Entity values. 0 = the owning (instancing) entity.
  - All shipped fragments keep: every create first, then one contiguous set-group per entity in create order
    (1575/1578; the 3 exceptions are Missions.fragment, Overdose.scene, PlayerData.fragment). New entities are
    appended so the invariant holds.
  - Transforms: pos (#12, vector) and orient (#13, quaternion x,y,z,w) are parent-relative. World = parent * local
    with the stored quaternion CONJUGATED (project convention, validated in game by the collision lab).
  - unique_id0/unique_id1 (#27/#28) form the entity's GUID; FUN_0088F510 builds the GUID path that keys model
    instance lighting (FUN_00887460 -> MeshColor lookup 0x85E6B0). Clones get fresh ids unless keep_unique_ids.
"""
import math, random, struct
import todfrag as tf

P_PARENT, P_POS, P_ORIENT, P_NAME, P_ORDER, P_UID0, P_UID1 = 7, 12, 13, 15, 5, 27, 28


# ------------------------------------------------------------------ quaternion helpers (project convention)
def qconj(q): return (-q[0], -q[1], -q[2], q[3])
def qnorm(q):
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return tuple(x / n for x in q)
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx+ax*bw+ay*bz-az*by, aw*by-ax*bz+ay*bw+az*bx, aw*bz+ax*by-ay*bx+az*bw, aw*bw-ax*bx-ay*by-az*bz)
def qrot(q, v):
    x, y, z, w = q; vx, vy, vz = v
    tx = 2*(y*vz - z*vy); ty = 2*(z*vx - x*vz); tz = 2*(x*vy - y*vx)
    return (vx+w*tx+(y*tz-z*ty), vy+w*ty+(z*tx-x*tz), vz+w*tz+(x*ty-y*tx))
IDENT = ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))
def compose(A, B):
    return (tuple(a + r for a, r in zip(A[0], qrot(A[1], B[0]))), qmul(A[1], B[1]))
def inverse(A):
    qi = qconj(A[1])
    p = qrot(qi, A[0])
    return ((-p[0], -p[1], -p[2]), qi)


class EditError(Exception):
    pass


class FragDoc:
    def __init__(self, payload, types, name=''):
        self.T = types
        self.name = name
        self.frag = tf.Fragment(payload, types)
        self.orig = bytes(payload)
        self.records = self.frag.records
        self.modified = False
        self.log = []
        self.used_ids = set()            # extra ids reserved by the caller (other fragments of the block)
        self._gen = 0

    # ------------------------------------------------------------------ indexing
    def creates(self):
        return [r for r in self.records if r.op == tf.CREATE]

    def created_ids(self):
        return list(self._index()[1].keys())

    def sets_of(self, node):
        return [r for r in self.records if r.op == tf.SET and r.node == node]

    def _index(self):
        key = (id(self.records), len(self.records), self._gen)
        if getattr(self, '_ikey', None) != key:
            props, creates = {}, {}
            for r in self.records:
                if r.op == tf.SET:
                    props.setdefault(r.node, {})[r.prop] = r      # last one wins (records apply in order)
                elif r.op == tf.CREATE:
                    creates.setdefault(r.node, r)
            self._props, self._creates, self._ikey = props, creates, key
        return self._props, self._creates

    def get(self, node, prop, default=None):
        prop = self.T.index(prop)
        r = self._index()[0].get(node, {}).get(prop)
        return default if r is None else self.frag.value(r)

    def type_of(self, node):
        for r in self.records:
            if r.op == tf.CREATE and r.node == node:
                return tf.decode_value(r.raw, 0, ('string',))
        return None

    def children(self, node):
        return [n for n in self.created_ids() if (self.get(n, P_PARENT) or 0) == node and n != node]

    def subtree(self, node):
        par = {n: (self.get(n, P_PARENT) or 0) for n in self.created_ids()}
        kids = {}
        for n, p in par.items():
            kids.setdefault(p, []).append(n)
        out, stack = [], [node]
        while stack:
            n = stack.pop(0)
            out.append(n)
            stack[0:0] = kids.get(n, [])
        return out                                  # pre-order in create order

    def path_of(self, node):
        names, n, seen = [], node, set()
        while n and n not in seen:
            seen.add(n)
            names.append(self.get(n, P_NAME) or '0x%08X' % n)
            n = self.get(n, P_PARENT) or 0
        return '/'.join(reversed(names))

    def find(self, sel):
        """Selector: int / '0x...' nodeId; {'path': 'A/B/C'} (names from a root entity, '*' allowed);
        {'name': n, 'type': t?, 'index': i?}."""
        ids = self.created_ids()
        if isinstance(sel, int):
            if sel not in ids: raise EditError('node %#x not created in %s' % (sel, self.name))
            return sel
        if isinstance(sel, str):
            if sel.lower().startswith('0x'):
                return self.find(int(sel, 16))
            sel = {'path': sel}
        if 'node' in sel:
            return self.find(sel['node'])
        if 'path' in sel:
            parts = [p for p in sel['path'].split('/') if p]
            cur = [n for n in ids if (self.get(n, P_PARENT) or 0) not in ids]
            first = True
            for p in parts:
                pool = cur if first else [c for n in cur for c in self.children(n)]
                cur = [n for n in pool if p == '*' or self.get(n, P_NAME) == p]
                first = False
            hits = cur
        else:
            hits = [n for n in ids if self.get(n, P_NAME) == sel.get('name')
                    and (sel.get('type') is None or self.type_of(n) == sel['type'])]
        if 'index' in sel:
            hits = hits[sel['index']:sel['index'] + 1]
        if len(hits) != 1:
            raise EditError('selector %r matched %d entities in %s' % (sel, len(hits), self.name))
        return hits[0]

    # ------------------------------------------------------------------ transforms
    def local(self, node):
        p = self.get(node, P_POS) or [0.0, 0.0, 0.0]
        q = self.get(node, P_ORIENT) or [0.0, 0.0, 0.0, 1.0]
        return (tuple(p), qconj(qnorm(q)))

    def world(self, node, owner=IDENT):
        chain, n, seen = [], node, set()
        while n and n not in seen:
            seen.add(n); chain.append(n)
            n = self.get(n, P_PARENT) or 0
        W = owner
        for n in reversed(chain):
            W = compose(W, self.local(n))
        return W

    # ------------------------------------------------------------------ record surgery
    def _encode(self, prop, value):
        t = self.T.parse(self.T.ptype(prop))
        return tf.encode_value(value, t)

    def _insert_set(self, rec):
        """Append a set record at the end of its entity's set group (or after the last record)."""
        last = None
        for i, r in enumerate(self.records):
            if r.op == tf.SET and r.node == rec.node:
                last = i
        if last is None:
            self.records.append(rec)
        else:
            self.records.insert(last + 1, rec)

    def set_prop(self, node, prop, value, note=True):
        prop = self.T.index(prop)
        raw = self._encode(prop, value)
        hits = [r for r in self.records if r.op == tf.SET and r.node == node and r.prop == prop]
        if hits:
            r = hits[-1]
            if tf.mask_string_padding(r.raw, 0, self.T.parse(self.T.ptype(prop))) == raw:
                return False                   # same value: keep the original bytes (incl. string padding)
            old = self.frag.value(r)
            r.raw = raw
            what = 'changed'
        else:
            old = None
            self._insert_set(tf.Record(node, tf.SET, prop, raw))
            what = 'added'
        self.modified = True
        self._gen += 1
        if note:
            self.log.append(dict(op='set', node='0x%08X' % node, prop=self.T.name(prop), index=prop, what=what,
                                 old=old, new=value))
        return True

    def move(self, node, pos=None, orient=None, space='local', owner=IDENT):
        if space == 'world':
            par = self.get(node, P_PARENT) or 0
            PW = self.world(par, owner) if par else owner
            cur = self.world(node, owner)
            wp = tuple(pos) if pos is not None else cur[0]
            wq = qconj(qnorm(tuple(orient))) if orient is not None else cur[1]
            L = compose(inverse(PW), (wp, wq))
            pos = list(L[0]); orient = list(qconj(qnorm(L[1])))
        if pos is not None:
            self.set_prop(node, P_POS, [tf.f32(x) + 0.0 for x in pos])           # + 0.0: no negative zeros
        if orient is not None:
            self.set_prop(node, P_ORIENT, [tf.f32(x) + 0.0 for x in qnorm(orient)])

    def new_ids(self, count, like):
        taken = set(self.created_ids()) | self.used_ids
        for r in self.records:
            taken.add(r.node)
        hi = like & ~0xFF8FFFFF                    # keep the block bits (20..22) of the source
        slot = max([tf.node_slot(n) for n in taken] + [0]) + 1
        out = []
        while len(out) < count:
            n = hi | slot
            if n not in taken:
                out.append(n); taken.add(n)
            slot += 1
        self.used_ids.update(out)
        return out

    def _remap_value(self, prop, raw, m):
        t = self.T.parse(self.T.ptype(prop))
        if 'entity' not in repr(t):
            return raw
        v = tf.decode_value(raw, 0, t)
        def walk(v, t):
            if t[0] == 'entity':
                return m.get(v, v) if v is not None else None
            if t[0] == 'list':
                return [walk(x, t[1]) for x in v]
            if t[0] == 'dict':
                return [[k, walk(x, t[1])] for k, x in v]
            if t[0] == 'struct':
                return {fn: walk(v[fn], ft) for fn, ft in t[2]}
            return v
        nv = walk(v, t)
        return raw if nv == v else tf.encode_value(nv, t)

    def clone(self, node, deep=True, parent=None, name=None, keep_unique_ids=False, order='append', seed=None):
        src = self.subtree(node) if deep else [node]
        idx = self._index()[0]
        self.last_clone_snapshot = {n: {p: r.raw for p, r in idx.get(n, {}).items()} for n in src}
        self.last_clone_types = {n: self.type_of(n) for n in src}
        new = self.new_ids(len(src), node)
        m = dict(zip(src, new))
        rnd = random.Random(seed if seed is not None else node)
        taken_uids = set()
        for r in self.records:
            if r.op == tf.SET and r.prop in (P_UID0, P_UID1):
                taken_uids.add(struct.unpack('<i', r.raw[:4])[0])
        creates = {r.node: r for r in self.records if r.op == tf.CREATE}
        last_create = max(i for i, r in enumerate(self.records) if r.op == tf.CREATE)
        new_creates = [tf.Record(m[n], tf.CREATE, raw=creates[n].raw) for n in src]
        self.records[last_create + 1:last_create + 1] = new_creates
        for n in src:
            for r in self.sets_of(n):
                raw = r.raw
                if r.prop == P_PARENT and n == node and parent is not None:
                    raw = self._encode(P_PARENT, parent)
                else:
                    raw = self._remap_value(r.prop, raw, m)
                if r.prop in (P_UID0, P_UID1) and not keep_unique_ids:
                    v = struct.unpack('<i', raw[:4])[0]
                    if v != 0:
                        nv = 0
                        while nv == 0 or nv in taken_uids:
                            nv = rnd.randint(-2**31, 2**31 - 1)
                        taken_uids.add(nv)
                        raw = struct.pack('<i', nv)
                self.records.append(tf.Record(m[n], tf.SET, r.prop, raw, r.hi))
        self.modified = True
        self._gen += 1
        root = m[node]
        if parent is not None and not any(r.prop == P_PARENT for r in self.sets_of(node)):
            self.set_prop(root, P_PARENT, parent, note=False)
        if name is not None:
            self.set_prop(root, P_NAME, name, note=False)
        if order == 'append':
            par = self.get(root, P_PARENT) or 0
            sib = [self.get(c, P_ORDER) or 0 for c in self.children(par) if c != root]
            if self.get(root, P_ORDER) is not None:
                self.set_prop(root, P_ORDER, max(sib + [0]) + 1, note=False)
        self.log.append(dict(op='clone', source='0x%08X' % node, deep=deep, new={'0x%08X' % k: '0x%08X' % v for k, v in m.items()},
                             root='0x%08X' % root, path=self.path_of(root), name=name))
        return root, m

    def references_to(self, nodes):
        nodes = set(nodes); out = []
        for r in self.records:
            if r.op != tf.SET or r.node in nodes:
                continue
            t = self.T.parse(self.T.ptype(r.prop))
            if 'entity' in repr(t) and self.frag.value(r) is not None:
                def hits(v, t):
                    if t[0] == 'entity': return v in nodes
                    if t[0] == 'list': return any(hits(x, t[1]) for x in v)
                    if t[0] == 'dict': return any(hits(x, t[1]) for _, x in v)
                    if t[0] == 'struct': return any(hits(v[fn], ft) for fn, ft in t[2])
                    return False
                if hits(self.frag.value(r), t):
                    out.append((r.node, self.T.name(r.prop)))
        return out

    def remove(self, node, deep=True):
        ns = set(self.subtree(node) if deep else [node])
        refs = self.references_to(ns)
        if refs:
            raise EditError('cannot remove %#x: still referenced by %r' % (node, refs[:5]))
        self.records = [r for r in self.records if r.node not in ns]
        self.modified = True
        self._gen += 1
        self.log.append(dict(op='remove', node='0x%08X' % node, removed=len(ns)))

    # ------------------------------------------------------------------ output
    def payload(self):
        if not self.modified:
            return self.orig
        self.frag.rechunk(self.records)
        return self.frag.serialize()
