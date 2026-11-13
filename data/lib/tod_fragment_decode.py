#!/usr/bin/env python3
"""
TOD (Kapow Engine) fragment decoder.

Decodes a raw .fragment data blob (as dumped by `restools <type> <path> dumpdata`)
into the entity/node graph it serializes: create-node records (entity-type names),
delete records, and set-property records (incl. transforms position/orientation).

Format (reverse-engineered):
  - On-disk pointers are SELF-RELATIVE: abs = stored_u32 + address_of_that_field.
  - FragmentInfo header (0x18 bytes): field_0 = self-rel offset to first block.
  - Block: [u32 next(self-rel)] [u32 rec_len bytes] [u32 first_rec(self-rel)] ...records...
           record stream spans [first_rec, first_rec + rec_len); next block follows.
  - Record: [u32 nodeId] [u32 packed]; hi = packed>>24:
        0xFF -> create node: payload [u32 wordcount][asciiz padded to wordcount*4]
        0xFE -> delete node: no payload
        else -> set property idx=(packed & 0xFFFFFF): payload = type-sized value
  - nodeId bitfield (Entity::EntityId): bit0 HasPosition, bit1 HasQuadTree,
        bit2 HasFragment, bits8..22 Id (15b), bits23..25 BlockId (3b).
"""
import sys, os, json, struct, argparse

# property type -> payload size in 32-bit words (fixed-size types).
# Variable types (string/list/dict/struct/engine-structs) handled separately.
FIXED_TYPE_WORDS = {
    'number': 1, 'integer': 1, 'truth': 1, 'Entity': 1, 'color': 1,
    'vector': 3, 'quaternion': 4,
}

def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]

def f32(b, off):
    return struct.unpack_from('<f', b, off)[0]

def fixup(b, off):
    """Self-relative pointer at byte offset `off` -> absolute byte offset."""
    return (u32(b, off) + off) & 0xFFFFFFFF

def align4(v):
    return v & 0xFFFFFFFC

def load_properties(dump_path, builtin_offset):
    """Return dict: global_property_index -> (name, type)."""
    table = {}
    with open(dump_path, 'r', encoding='latin-1') as f:
        for i, line in enumerate(f):
            line = line.rstrip('\r\n')
            if not line:
                continue
            # property entries are "name:type"; commands have "(...)".
            name, _, typ = line.rpartition(':')
            if not name:
                name, typ = line, ''
            table[builtin_offset + i] = (name, typ)
    return table

def decode_entity_id(nid):
    return {
        'raw': nid,
        'hasPosition': bool(nid & 1),
        'hasQuadTree': bool(nid & 2),
        'hasFragment': bool(nid & 4),
        'id': (nid >> 8) & 0x7FFF,
        'blockId': (nid >> 23) & 0x7,
    }

def read_cstr(b, off, max_bytes):
    end = b.find(b'\x00', off, off + max_bytes)
    if end < 0:
        end = off + max_bytes
    return b[off:end].decode('latin-1', 'replace')

def decode_value(b, off, typ):
    """Return (python_value, words_consumed_from_payload) for a property value."""
    w = FIXED_TYPE_WORDS.get(typ)
    if w is not None:
        if typ == 'number':
            return f32(b, off), 1
        if typ in ('integer', 'Entity', 'color'):
            return u32(b, off), 1
        if typ == 'truth':
            return bool(u32(b, off) & 1), 1
        if typ == 'vector':
            return [f32(b, off), f32(b, off + 4), f32(b, off + 8)], 3
        if typ == 'quaternion':
            return [f32(b, off), f32(b, off + 4), f32(b, off + 8), f32(b, off + 12)], 4
    if typ == 'string':
        wc = u32(b, off)
        s = read_cstr(b, off + 4, wc * 4)
        return s, 1 + wc
    # unknown / variable (list, dict, struct, engine structs) -> can't size safely
    return None, None

def is_record_start(b, pos, rec_end, known_ids):
    """A valid record begins at pos: nodeId is known and the packed word is a plausible
    set-property (small index) or a create/delete marker."""
    if pos + 8 > rec_end:
        return False
    if u32(b, pos) not in known_ids:
        return False
    hi = u32(b, pos + 4) >> 24
    if hi in (0xFE, 0xFF):
        return True
    return (u32(b, pos + 4) & 0xFFFFFF) < 6100  # global property id ceiling

def value_span_words(b, vstart, rec_end, known_ids):
    """Builtin/unknown property: delimit the value by scanning to the next record start
    (the next known nodeId followed by a plausible packed word)."""
    vp = vstart
    while vp + 4 <= rec_end:
        if is_record_start(b, vp, rec_end, known_ids):
            break
        vp += 4
    return (vp - vstart) // 4

def words_to_str(b, off, nwords):
    raw = b[off:off + nwords * 4]
    return raw.split(b'\x00', 1)[0].decode('latin-1', 'replace')

def interpret_words(b, off, words):
    """Best-effort typing of a delimited builtin value from its word count."""
    if words == 1:
        iv = u32(b, off)
        return {'i': iv if iv < 0x80000000 else iv - 0x100000000,
                'f': round(f32(b, off), 4)}
    # string property stored as [u32 wordcount][asciiz padded]: first word == payload words
    cnt = u32(b, off)
    if 0 < cnt == words - 1:
        s = words_to_str(b, off + 4, cnt)
        if all(32 <= ord(c) < 127 or c == '' for c in s):
            return {'str': s}
    if words == 3:
        return [round(f32(b, off + 4 * i), 4) for i in range(3)]   # vector
    if words == 4:
        return [round(f32(b, off + 4 * i), 4) for i in range(4)]   # quaternion
    return [u32(b, off + 4 * i) for i in range(words)]

def walk_records(b, first_rec, rec_end, props, stats, known_ids):
    """Decode the contiguous record stream in [first_rec, rec_end)."""
    ops = []
    pos = first_rec
    while pos + 8 <= rec_end:
        nid = u32(b, pos)
        packed = u32(b, pos + 4)
        hi = packed >> 24

        # resync: a record must start on a known nodeId (or a create/delete marker)
        if not is_record_start(b, pos, rec_end, known_ids):
            scan = pos + 4
            while scan + 8 <= rec_end and not is_record_start(b, scan, rec_end, known_ids):
                scan += 4
            stats['resyncs'] += 1
            stats['skipped_words'] += (scan - pos) // 4
            pos = scan
            continue

        pos += 8
        if hi == 0xFF:  # create node
            wc = u32(b, pos)
            ops.append({'op': 'create', 'nodeId': nid, 'type': read_cstr(b, pos + 4, wc * 4)})
            pos += 4 + wc * 4
            stats['create'] += 1
        elif hi == 0xFE:  # delete node
            ops.append({'op': 'delete', 'nodeId': nid})
            stats['delete'] += 1
        else:  # set property
            idx = packed & 0xFFFFFF
            name, typ = props.get(idx, (None, None))
            val, consumed = decode_value(b, pos, typ) if typ is not None else (None, None)
            if consumed is None:
                # builtin (<711) or variable type: delimit by next record start.
                words = value_span_words(b, pos, rec_end, known_ids)
                val = interpret_words(b, pos, words)
                pos += words * 4
                ops.append({'op': 'setprop', 'nodeId': nid, 'propIndex': idx,
                            'propName': name or ('builtin#%d' % idx), 'propType': typ,
                            'value': val, 'inferred': True})
                stats['inferred'] += 1
            else:
                pos += consumed * 4
                ops.append({'op': 'setprop', 'nodeId': nid, 'propIndex': idx,
                            'propName': name, 'propType': typ, 'value': val})
                stats['setprop'] += 1
    return ops

def iter_blocks(blob):
    """Yield (first_rec, rec_end) for each block in the chain."""
    size = len(blob)
    block = fixup(blob, 0)
    visited = set()
    while 0 < block and block + 12 <= size and block not in visited:
        visited.add(block)
        next_stored = u32(blob, block)
        next_block = (next_stored + block) & 0xFFFFFFFF
        rec_len = u32(blob, block + 4)
        first_rec = fixup(blob, block + 8)
        rec_end = min(first_rec + rec_len, size)
        if first_rec < rec_end:
            yield first_rec, rec_end
        if next_stored in (0, 0x80000000) or next_block <= block or next_block + 12 > size:
            break
        block = next_block

def collect_created_ids(blob):
    """Pass 1: scan create records to learn the universe of valid nodeIds."""
    ids = set()
    for first_rec, rec_end in iter_blocks(blob):
        pos = first_rec
        while pos + 8 <= rec_end:
            nid = u32(blob, pos)
            hi = u32(blob, pos + 4) >> 24
            if hi == 0xFF:
                wc = u32(blob, pos + 8)
                ids.add(nid)
                pos += 12 + wc * 4
            elif hi == 0xFE:
                pos += 8
            else:
                # property phase reached; stop scanning this block for creates
                break
    return ids

def decode_fragment(blob, props):
    stats = {'create': 0, 'delete': 0, 'setprop': 0, 'inferred': 0,
             'blocks': 0, 'resyncs': 0, 'skipped_words': 0}
    known_ids = collect_created_ids(blob)
    ops = []
    for first_rec, rec_end in iter_blocks(blob):
        stats['blocks'] += 1
        ops.extend(walk_records(blob, first_rec, rec_end, props, stats, known_ids))
    return ops, stats

# Builtin property indices recovered empirically and cross-validated against the full
# Builtin (code-registered) property names, global index -> name. Reproduced statically by replaying
# KapowEngineClass::InitEntitiesDatabase()'s registration order (tools/build_property_index.py ->
# property_index_map.json): global index = first-seen order of property names as each class's
# Register() runs. Anchor-validated: parent=7, pos=12, orient=13, name=15 (exact). NOTE: the engine
# also push_backs a duplicate GlobalPropertiesList entry on slot<0 re-registration of an inherited
# name, which our unique-name replay doesn't, so absolute indices can drift by a few in the HIGH
# builtin range (~6 by idx 520). The low range that level fragments actually use (transforms, <~150)
# is exact. Database (.dump) properties at index >= 711 are named exactly from properties.dump.
import os as _os, json as _json
def _load_builtin_names():
    p = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'property_index_map.json')
    names = {7: 'parent', 12: 'position', 13: 'orientation', 15: 'name'}  # canonical transform anchors
    try:
        for r in _json.load(open(p))['table']:
            names.setdefault(r['index'], r['name'])
        names.update({7: 'parent', 12: 'position', 13: 'orientation', 15: 'name'})
    except Exception:
        pass
    return names
BUILTIN_NAMES = _load_builtin_names()

def build_entities(ops):
    """Fold the op stream into entity records (creation order = sibling/parent order)."""
    entities = {}
    order = []
    for op in ops:
        nid = op['nodeId']
        if op['op'] == 'create':
            ent = {'nodeId': nid, 'idBits': decode_entity_id(nid),
                   'type': op['type'], 'properties': {}}
            entities[nid] = ent
            order.append(nid)
        elif op['op'] == 'delete':
            if nid in entities:
                entities[nid]['deleted'] = True
        else:  # setprop
            ent = entities.get(nid)
            if ent is None:
                ent = {'nodeId': nid, 'idBits': decode_entity_id(nid),
                       'type': None, 'properties': {}}
                entities[nid] = ent
                order.append(nid)
            key = op.get('propName') or ('#%d' % op['propIndex'])
            if op.get('inferred') and op['propIndex'] in BUILTIN_NAMES:
                key = BUILTIN_NAMES[op['propIndex']]
            ent['properties'][key] = op['value']

    # derive a clean transform + parent for UE import
    for ent in entities.values():
        pr = ent['properties']
        tf = {}
        if isinstance(pr.get('position'), list):
            tf['position'] = pr['position']
        if isinstance(pr.get('orientation'), list):
            tf['orientation'] = pr['orientation']      # quaternion (x,y,z,w order TBD)
        par = pr.get('parent')
        if isinstance(par, dict) and 'i' in par:
            pv = par['i'] & 0xFFFFFFFF
            if pv in entities:
                ent['parent'] = pv
        if tf:
            ent['transform'] = tf
    return [entities[n] for n in order]

def main():
    ap = argparse.ArgumentParser(description='Decode a TOD .fragment blob to JSON.')
    ap.add_argument('fragment', help='path to raw .fragment blob (restools dumpdata output)')
    ap.add_argument('-p', '--properties', help='path to properties.dump', default=None)
    ap.add_argument('-o', '--out', help='output JSON path (default: stdout)', default=None)
    ap.add_argument('--builtin-offset', type=int, default=711,
                    help='global index of properties.dump[0] (builtin count, default 711)')
    ap.add_argument('--ops', action='store_true', help='also include raw op stream')
    args = ap.parse_args()

    with open(args.fragment, 'rb') as f:
        blob = f.read()
    props = {}
    if args.properties:
        props = load_properties(args.properties, args.builtin_offset)
    # Name the code-registered builtins (global index < builtin_offset) from the replayed table.
    # Type left as None so values keep using the proven inferred-span decoder; only names are added.
    for idx, nm in BUILTIN_NAMES.items():
        if idx < args.builtin_offset and idx not in props:
            props[idx] = (nm, None)

    ops, stats = decode_fragment(blob, props)
    entities = build_entities(ops)
    result = {
        'fragment': os.path.basename(args.fragment),
        'blobSize': len(blob),
        'builtinOffset': args.builtin_offset,
        'stats': stats,
        'entityCount': len(entities),
        'entities': entities,
    }
    if args.ops:
        result['ops'] = ops
    text = json.dumps(result, indent=2)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text)
        print('wrote %s (%d entities, stats=%s)' % (args.out, len(entities), stats))
    else:
        print(text)

if __name__ == '__main__':
    main()
