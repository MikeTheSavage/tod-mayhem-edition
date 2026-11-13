"""TOD fragment writer: read, round-trip, edit and package level / scene fragments (GOG build).

  python fragtool.py checksums                       prove the property/command tables against a block header
  python fragtool.py list <block>                    fragments of a block (record, size, entities)
  python fragtool.py dump <block> <fragment> [--entity SEL] [-o out.json]
  python fragtool.py roundtrip <block> [...] | --all  exact parse/serialize/rechunk/re-encode/block rebuild
  python fragtool.py edit <edit.json> --out DIR [--deploy TESTGAME]

<block> is an archive entry name (data_pc/overdose_the_game/overdose/shared/X.submap; a bare file name is looked up)
or a block file on disk (e.g. the output of clip_replace.py or TOD HD's rebuilt block, then give "entry" in edit.json).
<fragment> is a fragment record name or a unique substring of it. Entity selectors: "0x0030141B", "A/B/C" (names
from a root of the fragment), {"name": n, "type": t, "index": i}.

edit.json:
{ "description": "...",
  "blocks": [ { "block": "<entry or file>", "entry": "<entry, when block is a file>",
                "ops": [
     {"op": "set",   "fragment": F, "entity": SEL, "prop": "name or #index", "value": V},
     {"op": "move",  "fragment": F, "entity": SEL, "pos": [x,y,z], "orient": [x,y,z,w], "space": "local|world"},
     {"op": "clone", "fragment": F, "entity": SEL, "deep": true, "parent": SEL|null, "name": "new name",
                     "pos": [..], "orient": [..], "space": "local|world", "set": {prop: value}, "keep_unique_ids": false},
     {"op": "remove","fragment": F, "entity": SEL, "deep": true},
     {"op": "add_animslot", "fragment": "AnimDict.fragment", "source": "PlayerAnims/normal/moves/run",
                     "name": "run_player", "clip": "/characters/ram/ram_run_player.animation", "cps": 1.4706,
                     "deep": false, "set": {...}},
     {"op": "add_collisionbox", "fragment": F, "source": SEL, "name": "...", "pos": [..], "orient": [..],
                     "space": "world", "size": [width, height, length], "collisionflags": 1, "parent": SEL}
  ] } ] }
Values use the decoded forms printed by dump (numbers, ints, bools, [x,y,z], strings, nodeIds as "0x..." strings).

Every edit is verified before anything is written (see verify_block); the output is the rebuilt block(s), one
frag_override.zip (STORED, engine OpenZip/FindFile read-back) and the configpc.txt filesystem line.
"""
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, zipfile
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import todblock as tb
import todfrag as tf
import fragedit as fe

GOG_NAZ = os.environ.get('TOD_NAZ', r"C:\GOG Games\Total Overdose\blocks.naz")   # read only
ZIP_NAME = 'frag_override.zip'
BASE_ARCHIVES = ['blocks.naz', 'sounds.naz', 'videos00.naz', 'videos01.naz']
FORBIDDEN_DEPLOY = [os.path.normcase(os.path.abspath(p)) for p in
                    (r"C:\GOG Games\Total Overdose",)]
TYPES = None


def types():
    global TYPES
    if TYPES is None:
        TYPES = tf.Types()
    return TYPES


def sha(b):
    return hashlib.sha256(b).hexdigest()


class Fail(Exception):
    pass


# ------------------------------------------------------------------ block access
_arch = None
def archive():
    global _arch
    if _arch is None:
        _arch = tb.Archive(GOG_NAZ)
    return _arch


def resolve_entry(name):
    a = archive()
    if name in a.names:
        return name
    hits = [n for n in a.names if n.lower().endswith('/' + name.lower()) or n.lower() == name.lower()]
    if len(hits) != 1:
        raise Fail('block %r matches %d archive entries' % (name, len(hits)))
    return hits[0]


def load_block_bytes(block, entry=None):
    if os.path.exists(block):
        return open(block, 'rb').read(), (entry or os.path.basename(block)), os.path.splitext(block)[1]
    e = resolve_entry(block)
    return archive().read(e), e, os.path.splitext(e)[1]


def fragments(b):
    info = bytes(b.info)
    starts = tb.record_starts(info, b.n)
    return [(k, tb.record_name(info, R)) for k, R in enumerate(starts) if tb.u32(info, R) == 4], starts


def pick_fragment(frs, sub):
    exact = [x for x in frs if x[1] == sub]
    if exact:
        return exact[0]
    hits = [x for x in frs if sub.lower() in x[1].lower()]
    if len(hits) != 1:
        raise Fail('fragment %r matches %d records: %s' % (sub, len(hits), [h[1] for h in hits][:6]))
    return hits[0]


# ------------------------------------------------------------------ instancing chain (owner transforms)
def owner_chain(b, frs, k, T, depth=0, cache=None):
    """World transform of the entity that instantiates fragment record k inside the same block: the node whose
    'fragment' property (#9) names this fragment's file. Returns (transform, description list). A fragment that
    nothing in the block instantiates (the block's root, or a prefab used several times) gets identity."""
    cache = {} if cache is None else cache
    if k in cache:
        return cache[k]
    name = dict(frs)[k]
    base = name.replace(chr(92), '/').split('/')[-1].lower()
    hits = []
    for k2, nm2 in frs:
        if k2 == k:
            continue
        d = fe.FragDoc(b.payloads[k2], T, nm2)
        for n, recs in d._index()[0].items():
            r = recs.get(9)
            if r is None:
                continue
            v = d.frag.value(r)
            if v and v.replace(chr(92), '/').split('/')[-1].lower() == base:
                hits.append((k2, nm2, d, n))
    if len(hits) != 1 or depth > 8:
        res = (fe.IDENT, ['%s: %s' % (name.split('/')[-1], 'no instancing node in this block (root or external): identity'
                                       if not hits else '%d instancing nodes (prefab): identity' % len(hits))])
        cache[k] = res
        return res
    k2, nm2, d, n = hits[0]
    up, desc = owner_chain(b, frs, k2, T, depth + 1, cache)
    W = d.world(n, up)
    res = (W, desc + ['%s: instanced by 0x%08X "%s" in %s' % (name.split('/')[-1], n, d.path_of(n), nm2.split('/')[-1])])
    cache[k] = res
    return res


# ------------------------------------------------------------------ JSON view
def jval(v):
    if isinstance(v, float):
        return v
    return v


def entity_json(doc, n, owner=fe.IDENT):
    T = doc.T
    props = {}
    idx = doc._index()[0].get(n, {})
    for r in doc.sets_of(n):
        v = doc.frag.value(r)
        t = T.ptype(r.prop)
        if t == 'Entity' and v is not None:
            v = '0x%08X' % v
        props['%s' % T.name(r.prop)] = v
    W = doc.world(n, owner)
    return dict(node='0x%08X' % n, type=doc.type_of(n), path=doc.path_of(n),
                world=dict(pos=list(W[0]), orient=list(fe.qconj(W[1]))), props=props)


# ------------------------------------------------------------------ commands
def cmd_checksums(a):
    raw, entry, ext = load_block_bytes(a.block or 'overdose.main')
    b = tb.Block(raw, ext)
    pt = json.load(open(os.path.join(HERE, 'property_table.json')))
    ct = json.load(open(os.path.join(HERE, 'command_table.json')))
    def ck(strings):
        buf = bytearray()
        for i, s in enumerate(strings):
            buf += struct.pack('<I', i) + s.encode('latin-1')
        return tb.tod_crc(bytes(buf))
    p = ck(['%s:%s' % (n, t) for n, t in pt['properties']])
    pb = ck(['%s:%s' % (n, t) for n, t in pt['properties'][:pt['builtin']]])
    c = ck(ct['commands'])
    rep = dict(block=entry, header_propCk='%08X' % b.prop_ck, header_cmdCk='%08X' % b.cmd_ck,
               table_properties=len(pt['properties']), builtin_properties=pt['builtin'],
               computed_propCk='%08X' % p, computed_builtin_propCk='%08X' % pb,
               table_commands=len(ct['commands']), builtin_commands=ct['builtin'], computed_cmdCk='%08X' % c,
               match=(p == b.prop_ck and c == b.cmd_ck))
    print(json.dumps(rep, indent=1))
    return 0 if rep['match'] else 1


def cmd_list(a):
    raw, entry, ext = load_block_bytes(a.block)
    b = tb.Block(raw, ext)
    frs, _ = fragments(b)
    T = types()
    print('%s: %d records, %d fragments, propCk %08X cmdCk %08X' % (entry, b.n, len(frs), b.prop_ck, b.cmd_ck))
    for k, nm in frs:
        fr = tf.Fragment(b.payloads[k], T)
        recs = fr.records
        print('  rec %4d  %8d B  %3d chunks  %6d creates  %7d sets  %s' % (
            k, len(b.payloads[k]), len(fr.chunks), sum(r.op == tf.CREATE for r in recs),
            sum(r.op == tf.SET for r in recs), nm))


def cmd_dump(a):
    raw, entry, ext = load_block_bytes(a.block)
    b = tb.Block(raw, ext)
    frs, _ = fragments(b)
    k, nm = pick_fragment(frs, a.fragment)
    doc = fe.FragDoc(b.payloads[k], types(), nm)
    owner, odesc = owner_chain(b, frs, k, types())
    if a.entity:
        sel = json.loads(a.entity) if a.entity.startswith('{') else a.entity
        nodes = doc.subtree(doc.find(sel)) if a.deep else [doc.find(sel)]
    else:
        nodes = doc.created_ids()
    out = dict(block=entry, fragment=nm, record=k, payload_bytes=len(b.payloads[k]), chunks=len(doc.frag.chunks),
               owner_chain=odesc, entities=[entity_json(doc, n, owner) for n in nodes])
    text = json.dumps(out, indent=1)
    if a.o:
        open(a.o, 'w', encoding='utf-8').write(text)
        print('wrote %s (%d entities)' % (a.o, len(nodes)))
    else:
        print(text)


def roundtrip_block(raw, ext, T):
    b = tb.Block(raw, ext)
    frs, _ = fragments(b)
    nb = tb.Block(raw, ext)
    res = dict(fragments=len(frs), records=0, fail=[])
    for k, nm in frs:
        p = b.payloads[k]
        try:
            fr = tf.Fragment(p, T)
        except Exception as e:
            res['fail'].append([nm, str(e)]); continue
        s = fr.serialize()
        fr2 = tf.Fragment(p, T); fr2.rechunk()
        if s != p or fr2.serialize() != p:
            res['fail'].append([nm, 'serialize/rechunk differs']); continue
        for r in fr.records:
            res['records'] += 1
            if r.op == tf.SET:
                t = T.parse(T.ptype(r.prop))
                if tf.encode_value(tf.decode_value(r.raw, 0, t), t) != tf.mask_string_padding(r.raw, 0, t):
                    res['fail'].append([nm, 're-encode %s' % T.name(r.prop)]); break
        nb.payloads[k] = s
    res['block_identical'] = nb.serialize() == raw
    return res


def cmd_roundtrip(a):
    T = types()
    names = a.blocks
    if a.all:
        names = sorted(n for n in archive().names if os.path.splitext(n)[1].lower() in tb.BLOCK_EXTS)
    tot = dict(blocks=0, fragments=0, records=0, failures=0, blocks_not_identical=0)
    for nmb in names:
        raw, entry, ext = load_block_bytes(nmb)
        r = roundtrip_block(raw, ext, T)
        tot['blocks'] += 1; tot['fragments'] += r['fragments']; tot['records'] += r['records']
        tot['failures'] += len(r['fail']); tot['blocks_not_identical'] += (not r['block_identical'])
        if not a.all or r['fail'] or not r['block_identical']:
            print(entry, json.dumps(r))
    print(json.dumps(tot))
    return 0 if tot['failures'] == 0 and tot['blocks_not_identical'] == 0 else 1


# ------------------------------------------------------------------ edits
def value_in(doc, prop, v):
    """Edit-JSON value -> codec value (Entity selectors resolved in the fragment)."""
    T = doc.T
    pidx = T.index(prop)
    t = T.parse(T.ptype(pidx))
    def conv(v, t):
        if t[0] == 'entity':
            if v is None or v == 0:
                return v
            return doc.find(v)
        if t[0] == 'list':
            return [conv(x, t[1]) for x in v]
        if t[0] == 'simple' and t[1] == 'number':
            return float(v)
        return v
    return conv(v, t)


def apply_ops(raw, ext, ops, T):
    b = tb.Block(raw, ext)
    frs, _ = fragments(b)
    docs = {}
    block_ids = set()                                  # one shared set: new ids stay unique in the whole block
    for k, nm in frs:
        for r in tf.Fragment(b.payloads[k], T).records:
            block_ids.add(r.node)
    def doc_for(sub):
        k, nm = pick_fragment(frs, sub)
        if k not in docs:
            d = fe.FragDoc(b.payloads[k], T, nm)
            d.used_ids = block_ids
            d.owner, d.owner_desc = owner_chain(b, frs, k, T)
            docs[k] = d
        return k, docs[k]
    intended = []
    for op in ops:
        k, d = doc_for(op['fragment'])
        kind = op['op']
        if kind == 'set':
            n = d.find(op['entity'])
            d.set_prop(n, op['prop'], value_in(d, op['prop'], op['value']))
            intended.append(dict(k=k, kind='set', node=n, prop=T.index(op['prop'])))
        elif kind == 'move':
            n = d.find(op['entity'])
            d.move(n, op.get('pos'), op.get('orient'), op.get('space', 'local'), d.owner)
            for p in (('pos', fe.P_POS), ('orient', fe.P_ORIENT)):
                if op.get(p[0]) is not None or op.get('space') == 'world':
                    intended.append(dict(k=k, kind='set', node=n, prop=p[1]))
        elif kind in ('clone', 'add_animslot', 'add_collisionbox'):
            src = d.find(op['entity'] if 'entity' in op else op['source'])
            parent = d.find(op['parent']) if op.get('parent') not in (None, 0) else op.get('parent')
            root, m = d.clone(src, deep=op.get('deep', kind == 'clone'), parent=parent, name=op.get('name'),
                              keep_unique_ids=op.get('keep_unique_ids', False))
            sets = dict(op.get('set', {}))
            if kind == 'add_animslot':
                if 'clip' in op:
                    sets['target1'] = op['clip']
                    info = bytes(b.info)
                    names = {tb.record_name(info, R).lower() for R in tb.record_starts(info, b.n)}
                    want = ('/data' + op['clip']).lower()
                    if want not in names and not op.get('allow_missing_clip'):
                        raise Fail('clip %s is not an asset of this block (add it first with clip_replace.py '
                                   '--allow-add and pass that block here, or set allow_missing_clip)' % want)
                if 'cps' in op: sets['speed'] = op['cps']
            if kind == 'add_collisionbox':
                if 'size' in op:
                    sets['width'], sets['height'], sets['length'] = op['size']
                if 'collisionflags' in op: sets['CollisionFlags'] = op['collisionflags']
            for p, v in sets.items():
                d.set_prop(root, p, value_in(d, p, v))
            if op.get('pos') is not None or op.get('orient') is not None:
                d.move(root, op.get('pos'), op.get('orient'), op.get('space', 'local'), d.owner)
            intended.append(dict(k=k, kind='clone', src=src, root=root, map=m, sets={T.index(p) for p in sets},
                                 snapshot=d.last_clone_snapshot, src_types=d.last_clone_types))
        elif kind == 'remove':
            n = d.find(op['entity'])
            sub = d.subtree(n) if op.get('deep', True) else [n]
            d.remove(n, op.get('deep', True))
            intended.append(dict(k=k, kind='remove', nodes=set(sub)))
        else:
            raise Fail('unknown op %r' % kind)
    nb = tb.Block(raw, ext)
    for k, d in docs.items():
        nb.payloads[k] = d.payload()
    return b, nb, docs, intended


def world_in(fr, n, owner, T):
    d = fe.FragDoc(fr.serialize(), T)
    return d.world(n, owner)


def entity_map(fr, T):
    """nodeId -> (type, {prop: raw value bytes of the last set}), plus the create order."""
    ents, order = {}, []
    for r in fr.records:
        if r.op == tf.CREATE:
            if r.node not in ents:
                order.append(r.node)
            ents.setdefault(r.node, [tf.decode_value(r.raw, 0, ('string',)), {}])
        elif r.op == tf.SET:
            ents.setdefault(r.node, [None, {}])[1][r.prop] = r.raw
    return ents, order


def verify_block(old, new_bytes, ext, docs, intended, T):
    rep = {}
    chk = tb.Block(new_bytes, ext)
    starts_new = tb.record_starts(bytes(chk.info), chk.n)
    rep['1_block_parses'] = dict(N=chk.n, info_bytes=len(chk.info), max_payload=chk.max_asset)
    if (chk.timestamp, chk.prop_ck, chk.cmd_ck, chk.prefix, bytes(chk.info)) != \
       (old.timestamp, old.prop_ck, old.cmd_ck, old.prefix, bytes(old.info)):
        raise Fail('block header or INFO changed')
    pt = json.load(open(os.path.join(HERE, 'property_table.json')))
    ct = json.load(open(os.path.join(HERE, 'command_table.json')))
    if int(pt['crc_loaded'], 16) != chk.prop_ck or int(ct['crc_loaded'], 16) != chk.cmd_ck:
        raise Fail('block checksums do not match the property/command tables this tool decodes with')
    rep['2_header_info_unchanged_checksums_match_tables'] = dict(propCk='%08X' % chk.prop_ck, cmdCk='%08X' % chk.cmd_ck)
    same = 0
    for k in range(old.n):
        if k in docs:
            continue
        if chk.payloads[k] != old.payloads[k]:
            raise Fail('untouched payload %d changed' % k)
        same += 1
    rep['3_untouched_payloads_identical'] = same
    frag_reps = []
    for k, d in docs.items():
        newp = chk.payloads[k]
        fr = tf.Fragment(newp, T)                         # exact parse of the result
        fr2 = tf.Fragment(newp, T); fr2.rechunk()
        if fr2.serialize() != newp:
            raise Fail('%s: chunking does not follow the editor rule' % d.name)
        oldf = tf.Fragment(old.payloads[k], T)
        oe, oorder = entity_map(oldf, T)
        ne, norder = entity_map(fr, T)
        mine = [x for x in intended if x['k'] == k]
        allowed_sets = {(x['node'], x['prop']) for x in mine if x['kind'] == 'set'}
        new_nodes, removed = set(), set()
        for x in mine:
            if x['kind'] == 'clone':
                new_nodes |= set(x['map'].values())
            if x['kind'] == 'remove':
                removed |= x['nodes']
        # a: old entities unchanged except intended sets
        changed = []
        for n, (ty, props) in oe.items():
            if n in removed:
                if n in ne: raise Fail('%s: removed entity %#x still present' % (d.name, n))
                continue
            if n not in ne: raise Fail('%s: entity %#x lost' % (d.name, n))
            nty, nprops = ne[n]
            if nty != ty: raise Fail('%s: entity %#x type changed' % (d.name, n))
            for p in set(props) | set(nprops):
                if props.get(p) != nprops.get(p):
                    if (n, p) not in allowed_sets:
                        raise Fail('%s: unintended change %#x %s' % (d.name, n, T.name(p)))
                    changed.append(('0x%08X' % n, T.name(p)))
        # b: new entities = intended clones, with the source's records except remapped/intended ones
        extra = set(ne) - set(oe)
        if extra != new_nodes:
            raise Fail('%s: new entities %s != intended %s' % (d.name, sorted(extra), sorted(new_nodes)))
        for x in mine:
            if x['kind'] != 'clone':
                continue
            inv = {v: k2 for k2, v in x['map'].items()}
            for nn, src in inv.items():
                sty, sprops = x['src_types'][src], x['snapshot'][src]     # the source as it was when cloned
                nty, nprops = ne[nn]
                if nty != sty: raise Fail('clone type differs')
                for p in set(sprops) | set(nprops):
                    a, bb = sprops.get(p), nprops.get(p)
                    if a == bb:
                        continue
                    t = T.parse(T.ptype(p))
                    if p in (fe.P_UID0, fe.P_UID1, fe.P_NAME, fe.P_ORDER, fe.P_PARENT, fe.P_POS, fe.P_ORIENT) or \
                       (nn == x['root'] and p in x['sets']):
                        continue
                    if 'entity' in repr(t) and a is not None and bb is not None:
                        av = tf.decode_value(a, 0, t); bv = tf.decode_value(bb, 0, t)
                        if json.dumps(av, default=str) != json.dumps(bv, default=str):
                            continue                       # remapped reference inside the clone
                    raise Fail('clone %#x differs from source %#x in %s' % (nn, src, T.name(p)))
        # c: every Entity value resolves inside the fragment (or 0 = owner, or none); same rule as the original
        def ents_in(v, t):
            if t[0] == 'entity': yield v
            elif t[0] == 'list':
                for y in v: yield from ents_in(y, t[1])
        dangling = 0
        created = set(ne)
        for r in fr.records:
            if r.op == tf.SET:
                if r.node not in created: dangling += 1
                t = T.parse(T.ptype(r.prop))
                if t[0] in ('entity', 'list'):
                    for v in ents_in(tf.decode_value(r.raw, 0, t), t):
                        if v not in (None, 0) and v not in created:
                            dangling += 1
        old_dangling = 0
        for r in oldf.records:
            if r.op == tf.SET:
                if r.node not in set(oe): old_dangling += 1
                t = T.parse(T.ptype(r.prop))
                if t[0] in ('entity', 'list'):
                    for v in ents_in(tf.decode_value(r.raw, 0, t), t):
                        if v not in (None, 0) and v not in oe: old_dangling += 1
        if dangling > old_dangling:
            raise Fail('%s: %d dangling references (original had %d)' % (d.name, dangling, old_dangling))
        # d: parent chains end at 0 (no cycles)
        for n in ne:
            seen, p = set(), n
            while p:
                if p in seen: raise Fail('%s: parent cycle at %#x' % (d.name, n))
                seen.add(p)
                pr = ne.get(p, [None, {}])[1].get(fe.P_PARENT)
                p = struct.unpack('<I', pr)[0] if pr else 0
                if p == 0xFFFFFFFF: p = 0
        # e: creates first, set groups contiguous in create order (shipped invariant), unless the original broke it
        recs = fr.records
        nc = sum(r.op == tf.CREATE for r in recs)
        inv_ok = all(r.op == tf.CREATE for r in recs[:nc])
        seq = []
        for r in recs[nc:]:
            if not seq or seq[-1] != r.node: seq.append(r.node)
        inv_ok = inv_ok and len(seq) == len(set(seq))
        # f: an independent reader (the project's heuristic decoder) walks the result without resyncs
        indep = None
        try:
            sys.path.insert(0, HERE)   # TODGameplay: vendored copy
            import tod_fragment_decode as tfd
            ops, st = tfd.decode_fragment(newp, {})
            indep = dict(resyncs=st['resyncs'], create=st['create'], set=st['setprop'] + st['inferred'])
        except Exception as e:
            indep = 'not run: %s' % e
        frag_reps.append(dict(fragment=d.name, record=k, payload_old=len(old.payloads[k]), payload_new=len(newp),
                              chunks_old=len(oldf.chunks), chunks_new=len(fr.chunks), records_old=len(oldf.records),
                              records_new=len(recs), changed_props=changed, new_entities=len(new_nodes),
                              removed_entities=len(removed), dangling_refs=dangling,
                              shipped_order_invariant=inv_ok, independent_reader=indep, log=d.log,
                              owner_chain=getattr(d, 'owner_desc', None),
                              world_after={'0x%08X' % n: dict(zip(('pos', 'orient'), (list(W[0]), list(fe.qconj(W[1])))))
                                           for n in sorted({x.get('node') or x.get('root') for x in mine if x['kind'] in ('set', 'clone')})
                                           for W in [world_in(fr, n, getattr(d, 'owner', fe.IDENT), T)]}))
    rep['4_edited_fragments'] = frag_reps
    return rep, chk


def write_zip(path, entries, naz_for_mtime=GOG_NAZ):
    tmp = path + '.part'
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as zf:
        for entry, data in entries:
            zi = zipfile.ZipInfo(entry, date_time=(2005, 8, 21, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED; zi.create_system = 0; zi.external_attr = 0
            zf.writestr(zi, data)
    os.replace(tmp, path)
    st = os.stat(naz_for_mtime)
    os.utime(path, (st.st_atime, st.st_mtime))
    a = tb.Archive(path)
    out = []
    for entry, data in entries:
        hit = a.find('/' + entry)
        if a.naz or hit is None or hit[2] != entry or hit[1] != len(data):
            raise Fail('zip: engine lookup failed for %s' % entry)
        with open(path, 'rb') as f:
            f.seek(hit[0])
            if f.read(hit[1]) != data:
                raise Fail('zip: bytes at the engine offset differ for %s' % entry)
        out.append(dict(entry=entry, data_offset=hit[0], bytes=len(data), sha256=sha(data), engine_lookup_ok=True))
    base = archive()
    collide = [n for c, (_, _, n) in base.table.items() if c in a.table and n != a.table[c][2]]
    if collide:
        raise Fail('zip: name hash collides with ' + collide[0])
    return dict(zip=path, zip_bytes=os.path.getsize(path), entries=out)


def game_running():
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout.lower()
    return '"tod.exe"' in out or '"totaloverdose.exe"' in out


def cmd_edit(a):
    T = types()
    spec = json.load(open(a.edit))
    os.makedirs(a.out, exist_ok=True)
    rep = dict(edit=spec, blocks=[])
    zip_entries = []
    try:
        for bs in spec['blocks']:
            raw, entry, ext = load_block_bytes(bs['block'], bs.get('entry'))
            if os.path.exists(bs['block']) and not bs.get('entry'):
                raise Fail('block given as a file needs "entry" (its archive name)')
            r0 = roundtrip_block(raw, ext, T)
            if r0['fail'] or not r0['block_identical']:
                raise Fail('base block does not round-trip: %s' % r0)
            old, nb, docs, intended = apply_ops(raw, ext, bs['ops'], T)
            out = nb.serialize()
            vr, chk = verify_block(old, out, ext, docs, intended, T)
            fn = os.path.join(a.out, os.path.basename(entry))
            open(fn, 'wb').write(out)
            rep['blocks'].append(dict(block=bs['block'], entry=entry, base_sha256=sha(raw), base_roundtrip=r0,
                                      out=fn, out_sha256=sha(out), out_bytes=len(out), checks=vr))
            zip_entries.append((entry, out))
        zp = os.path.join(a.out, ZIP_NAME)
        rep['zip'] = write_zip(zp, zip_entries)
        line = 'filesystem = ' + ', '.join([ZIP_NAME] + BASE_ARCHIVES)
        open(os.path.join(a.out, 'configpc_line.txt'), 'w').write(line + '\n')
        rep['configpc_line'] = line
        if a.deploy:
            gd = os.path.normcase(os.path.abspath(a.deploy))
            if any(gd == f or gd.startswith(f + os.sep) for f in FORBIDDEN_DEPLOY):
                raise Fail('refusing to deploy into %s' % a.deploy)
            if game_running():
                raise Fail('a game is running; not touching any game folder now')
            shutil.copy2(zp, os.path.join(a.deploy, ZIP_NAME))
            rep['deploy'] = dict(to=a.deploy, note='put %s first on the filesystem line of configpc.txt' % ZIP_NAME)
        rep['result'] = 'PASS'
    except (Fail, fe.EditError, KeyError, ValueError) as e:
        rep['result'] = 'FAIL: %s' % e
    json.dump(rep, open(os.path.join(a.out, 'report.json'), 'w'), indent=1, default=str)
    short = dict(result=rep['result'], zip=rep.get('zip'), configpc_line=rep.get('configpc_line'))
    for bl in rep['blocks']:
        for f in bl['checks']['4_edited_fragments']:
            short.setdefault('fragments', []).append({k: v for k, v in f.items() if k != 'log'})
            short.setdefault('log', []).extend(f['log'])
    print(json.dumps(short, indent=1, default=str))
    return 0 if rep['result'] == 'PASS' else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('checksums'); p.add_argument('block', nargs='?')
    p = sp.add_parser('list'); p.add_argument('block')
    p = sp.add_parser('dump'); p.add_argument('block'); p.add_argument('fragment')
    p.add_argument('--entity'); p.add_argument('--deep', action='store_true'); p.add_argument('-o')
    p = sp.add_parser('roundtrip'); p.add_argument('blocks', nargs='*'); p.add_argument('--all', action='store_true')
    p = sp.add_parser('edit'); p.add_argument('edit'); p.add_argument('--out', required=True); p.add_argument('--deploy')
    a = ap.parse_args()
    try:
        import lowprio  # noqa: F401  (below-normal priority on Windows)
    except Exception:
        pass
    return {'checksums': cmd_checksums, 'list': cmd_list, 'dump': cmd_dump, 'roundtrip': cmd_roundtrip,
            'edit': cmd_edit}[a.cmd](a) or 0


if __name__ == '__main__':
    sys.exit(main())
