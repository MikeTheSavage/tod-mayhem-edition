"""suave_anims: the Suave loco move's animations as a data option (clips built in suave/anim/).

Level `on` changes overdose.main in two save-safe steps, each verified before the next:
  A. clips: the Suave clips (22; the catalogue clips.json drives the count) (suave/anim/clips/game/*.json, the game's clip format) are APPENDED as new ANIMATION
     records after the last record (N + count); every existing record stays byte-identical, only the INFO size and N grow;
     header timestamp / propCk / cmdCk unchanged; each new record re-decodes to exactly its encoded bytes and its
     size pass ends on the next record; no clip name already exists in the block.
  B. AnimDict: one Folder `PlayerAnims/specials/Suave` (non-deep clone of `Sombrero_Move`) and one slot per clip
     (non-deep clones of `Sombrero_Move/SombreroMove_throw`, a database_animslot_actioninfo_for_motions slot) with
     target1 = the clip, speed = CPS, loopmode1, crossblend values. New entities get new node ids and their create
     records are appended after the fragment's last create, so every existing node keeps its creation index (saves
     address nodes by creation order). Checked with the fragment writer's verify_block (INFO unchanged, untouched
     payloads identical, only the intended new entities, no dangling references, no cycles) plus: the old create
     list is an exact prefix of the new one, the edited block round-trips through the fragment codec, and every new
     slot reads back with its clip / CPS / loop mode, and its clip resolves to a record of the block.
Other options run first (their set ops are verified by the builder); this step starts from their result.
"""
import json
import os
import struct

from . import options as opt   # noqa: F401  (same package conventions)

HERE = os.path.dirname(os.path.abspath(__file__))
CLIPS = os.path.normpath(os.path.join(HERE, '..', '..', 'suave', 'anim', 'clips', 'game'))
ENTRY = 'data_pc/overdose_the_game/overdose/shared/overdose.main'
FRAG = '/AnimDict.fragment'
FOLDER_SRC = 'PlayerAnims/specials/Sombrero_Move'
SLOT_SRC = 'PlayerAnims/specials/Sombrero_Move/SombreroMove_throw'
FOLDER = 'PlayerAnims/specials/Suave'


class SuaveError(Exception):
    pass


def catalogue():
    cat = json.load(open(os.path.join(CLIPS, 'clips.json')))
    order = sorted(cat, key=lambda k: (cat[k]['pairing'] or 'zz', k))
    return [(k, cat[k]) for k in order]


def slot_values(meta):
    """AnimDict slot values for one clip (labels as in the proven property table)"""
    loop = meta['loop_mode'] == 0
    return {'target1': meta['clip'][len('/data'):], 'speed': float(meta['cps']), 'loopmode1': 0 if loop else 1,
            'crossblendspeed': 5.0 if loop else 25.0, 'crossblendframes': 5 if loop else 1,
            'stallfirstframe': not loop, 'anim_flags': 0}


def append_clips(raw, tb, ac, cj):
    """step A; returns new block bytes and a report"""
    old = tb.Block(raw, '.main')
    starts = tb.record_starts(bytes(old.info), old.n)
    ends = starts[1:] + [len(old.info)]
    names = {tb.record_name(bytes(old.info), R).lower() for R in starts}
    template = next(ac.decode_record(bytes(old.info), R) for R in starts if tb.u32(old.info, R) == 9)
    new_info = bytearray(old.info)
    nb = tb.Block(raw, '.main')
    added = []
    for name, meta in catalogue():
        clip = json.load(open(os.path.join(CLIPS, name + '.json')))
        if clip['name'].lower() in names:
            raise SuaveError('clip %s already exists in the block' % clip['name'])
        rec = ac.encode_record(cj.clip_to_record(clip, template))
        if len(rec) % 16:
            raise SuaveError('record of %s is not 16-aligned' % name)
        added.append((name, clip['name'], len(new_info), rec))
        new_info += rec
        nb.payloads.append(b'')
    nb.info = new_info
    out = nb.serialize()
    chk = tb.Block(out, '.main')
    s2 = tb.record_starts(bytes(chk.info), chk.n)
    if chk.n != old.n + len(added) or s2[:old.n] != starts or s2[old.n:] != [a for _, _, a, _ in added]:
        raise SuaveError('record layout after the append is not the planned one')
    if (chk.timestamp, chk.prop_ck, chk.cmd_ck, chk.prefix) != (old.timestamp, old.prop_ck, old.cmd_ck, old.prefix):
        raise SuaveError('block header changed')
    if bytes(chk.info[:len(old.info)]) != bytes(old.info):
        raise SuaveError('existing records changed')
    for k in range(old.n):
        if chk.payloads[k] != old.payloads[k]:
            raise SuaveError('payload %d changed' % k)
    e2 = s2[1:] + [len(chk.info)]
    rb = []
    for (name, cname, a, rec), R, E in zip(added, s2[old.n:], e2[old.n:]):
        h = ac.decode_record(bytes(chk.info), R)
        if ac.encode_record(h) != rec or bytes(chk.info[R:R + len(rec)]) != rec or tb.a16(len(rec)) != E - R:
            raise SuaveError('%s does not read back' % name)
        if h['name'] != cname:
            raise SuaveError('%s: name reads back as %s' % (name, h['name']))
        rb.append(dict(clip=cname, record=old.n + len(rb), bytes=len(rec), frames=struct.unpack('<i', struct.pack('<I', h['frames']))[0]))
    return out, dict(records_before=old.n, records_after=chk.n, info_before=len(old.info), info_after=len(chk.info),
                     existing_records_identical=old.n, payloads_identical=old.n, header_unchanged=True, added=rb)


def add_slots(raw, tb, tf, fe, ft, T):
    """step B; returns new block bytes and a report"""
    ops = [{'op': 'clone', 'fragment': FRAG, 'entity': FOLDER_SRC, 'deep': False, 'name': 'Suave',
            'parent': 'PlayerAnims/specials'}]
    cat = catalogue()
    for name, meta in cat:
        v = slot_values(meta)
        ops.append({'op': 'add_animslot', 'fragment': FRAG, 'source': SLOT_SRC, 'deep': False, 'name': name,
                    'parent': FOLDER, 'clip': v['target1'], 'cps': v['speed'],
                    'set': {k: v[k] for k in ('loopmode1', 'crossblendspeed', 'crossblendframes', 'stallfirstframe',
                                              'anim_flags')}})
    b, nb, docs, intended = ft.apply_ops(raw, '.main', ops, T)
    out = nb.serialize()
    vrep, chk = ft.verify_block(b, out, '.main', docs, intended, T)
    # save rule: old creates are an exact prefix of the new creates; everything new is appended
    k = next(iter(docs))
    oc = [(r.node, r.raw) for r in tf.Fragment(b.payloads[k], T).records if r.op == tf.CREATE]
    nc = [(r.node, r.raw) for r in tf.Fragment(chk.payloads[k], T).records if r.op == tf.CREATE]
    if nc[:len(oc)] != oc:
        raise SuaveError('AnimDict create records are not an appended superset of the original')
    appended = len(nc) - len(oc)
    if appended != 1 + len(cat):
        raise SuaveError('AnimDict: %d creates appended, planned %d' % (appended, 1 + len(cat)))
    rt = ft.roundtrip_block(out, '.main', T)
    if rt['fail'] or not rt['block_identical']:
        raise SuaveError('edited block does not round-trip: %s' % rt['fail'][:3])
    # read-back of every slot through a fresh parse
    d = fe.FragDoc(chk.payloads[k], T, 'AnimDict')
    folder = d.find(FOLDER)
    info = bytes(chk.info)
    recnames = {tb.record_name(info, R).lower() for R in tb.record_starts(info, chk.n)}
    slots = []
    for name, meta in cat:
        n = d.find(FOLDER + '/' + name)
        v = slot_values(meta)
        got = {p: d.get(n, p) for p in v}
        for p, want in v.items():
            g = got[p]
            ok = abs(g - tf.f32(want)) < 1e-9 if isinstance(want, float) else g == want
            if not ok:
                raise SuaveError('slot %s: %s reads %r, planned %r' % (name, p, g, want))
        if ('/data' + v['target1']).lower() not in recnames:
            raise SuaveError('slot %s: clip %s is not a record of the block' % (name, v['target1']))
        if d.get(n, 'parent') != folder:
            raise SuaveError('slot %s is not under %s' % (name, FOLDER))
        slots.append(dict(slot=FOLDER + '/' + name, node='0x%08X' % n, type=d.type_of(n), **got))
    for f in vrep.get('4_edited_fragments', []):
        f.pop('world_after', None)
    return out, dict(fragment_checks=vrep, creates_before=len(oc), creates_after=len(nc), creates_appended=appended,
                     old_creates_prefix_identical=True, roundtrip=dict(fragments=rt['fragments'], records=rt['records']),
                     folder='0x%08X' % folder, slots=slots)


# ------------------------------------------------------------------ step C (save-safe position, a load crash fix)
# Saves address overdose.main nodes by RUNTIME creation index (save_editor/nodemap.py: the scene, then every
# instanced fragment's creates in order). AnimDict.fragment is instanced early (its nodes start at index ~0x9AE), so
# creates appended there shift every later node (5,831 nodes by 23): a save's recorded changes then hit nodes of
# another type and the TransactionBuffer replay (FUN_008AA220 -> FUN_0086CCB0, VA 0x86CD45) reads a property slot past
# the type's table -> access violation at load (a crash seen when loading a save). Fix: the same new entities are
# created in GameCamera.fragment, the LAST fragment instanced in the block, after its last create, with the Suave
# folder at its top level (parent 0 = a child of the GameCamera node that instances it). Every old node keeps its
# runtime index; the DLL finds the slots under the GameCamera node ([suave] clip_root = @camera:Suave).
LAST_FRAG = '/data/scripts/fragments/GameCamera.fragment'
SCENE = '/data/Overdose_THE_GAME/Overdose.scene'


def runtime_map(raw, tb):
    import sys as _s
    import os as _o
    lib = _o.path.join(_o.path.dirname(HERE), 'lib', 'save_editor')
    if lib not in _s.path:
        _s.path.insert(0, lib)
    import nodemap
    b = tb.Block(raw, '.main')
    info = bytes(b.info)
    fr = {}
    for i, R in enumerate(tb.record_starts(info, b.n)):
        nm = tb.record_name(info, R)
        if nm.lower().endswith('.fragment') or nm.lower().endswith('.scene'):
            fr[nm] = b.payloads[i]
    return [(n[0], n[2], n[3]) for n in nodemap.build(fr, SCENE, 5)]


def relocate(base, edited, tb, tf, fe, ft, T):
    """base = overdose.main before step B, edited = after step B (new entities in AnimDict). Returns the block with
    AnimDict untouched and the new entities appended to GameCamera.fragment instead, and a report."""
    b0, b1 = tb.Block(base, '.main'), tb.Block(edited, '.main')
    frs, _ = ft.fragments(b0)
    kA = [k for k, nm in frs if nm.lower().endswith('/animdict.fragment')]
    kG = [k for k, nm in frs if nm.lower() == LAST_FRAG.lower()]
    if len(kA) != 1 or len(kG) != 1:
        raise SuaveError('AnimDict / GameCamera fragment not found exactly once')
    kA, kG = kA[0], kG[0]
    old_ids = {r.node for r in tf.Fragment(b0.payloads[kA], T).records if r.op == tf.CREATE}
    new_recs = [r for r in tf.Fragment(b1.payloads[kA], T).records if r.node not in old_ids]
    new_creates = [r for r in new_recs if r.op == tf.CREATE]
    new_sets = [r for r in new_recs if r.op == tf.SET]
    dA = fe.FragDoc(b1.payloads[kA], T, 'AnimDict')
    folder = dA.find(FOLDER)
    d = fe.FragDoc(b0.payloads[kG], T, 'GameCamera')
    last = max(i for i, r in enumerate(d.records) if r.op == tf.CREATE)
    d.records[last + 1:last + 1] = new_creates
    d.records.extend(new_sets)
    d.modified = True
    d._gen += 1
    d.set_prop(folder, 'parent', 0, note=False)          # top level of GameCamera.fragment = under the GameCamera node
    nb = tb.Block(base, '.main')
    nb.payloads[kG] = d.payload()
    out = nb.serialize()
    # checks: only GameCamera changed; its old creates are a prefix; every old node keeps its runtime index
    chk = tb.Block(out, '.main')
    for k in range(b0.n):
        if k != kG and chk.payloads[k] != b0.payloads[k]:
            raise SuaveError('payload %d changed by the relocation' % k)
    if bytes(chk.info) != bytes(b0.info):
        raise SuaveError('INFO changed by the relocation')
    oc = [(r.node, r.raw) for r in tf.Fragment(b0.payloads[kG], T).records if r.op == tf.CREATE]
    nc = [(r.node, r.raw) for r in tf.Fragment(chk.payloads[kG], T).records if r.op == tf.CREATE]
    if nc[:len(oc)] != oc or len(nc) - len(oc) != len(new_creates):
        raise SuaveError('GameCamera creates are not an appended superset')
    rm0, rm1 = runtime_map(base, tb), runtime_map(out, tb)       # every node instance in creation order
    if rm1[:len(rm0)] != rm0:
        bad = next(i for i, (x, y) in enumerate(zip(rm0, rm1)) if x != y)
        raise SuaveError('existing node instances change their runtime index (save rule), first at %s' % (rm0[bad],))
    added = [x[0] for x in rm1[len(rm0):]]
    if len(added) != len(new_creates) or (added and added[0] <= rm0[-1][0]):
        raise SuaveError('the new nodes are not created exactly once, after every existing node')
    rt = ft.roundtrip_block(out, '.main', T)
    if rt['fail'] or not rt['block_identical']:
        raise SuaveError('relocated block does not round-trip: %s' % rt['fail'][:3])
    g = fe.FragDoc(chk.payloads[kG], T, 'GameCamera')
    if g.get(folder, 'parent') not in (0, None) or g.get(folder, 'name') != 'Suave':
        raise SuaveError('Suave folder is not at the top level of GameCamera.fragment')
    slots = []
    for name, meta in catalogue():
        hits = [n for n in g.created_ids() if g.get(n, 'name') == name and g.get(n, 'parent') == folder]
        if len(hits) != 1:
            raise SuaveError('slot %s not found once under the folder' % name)
        v = slot_values(meta)
        for p, want in v.items():
            got = g.get(hits[0], p)
            ok = abs(got - tf.f32(want)) < 1e-9 if isinstance(want, float) else got == want
            if not ok:
                raise SuaveError('relocated slot %s: %s reads %r, planned %r' % (name, p, got, want))
        slots.append(name)
    return out, dict(fragment=LAST_FRAG, creates_appended=len(new_creates), sets=len(new_sets),
                     animdict_unchanged=True, old_nodes_runtime_index_unchanged=len(rm0),
                     new_runtime_indices=[hex(added[0]), hex(added[-1])] if added else [], slots=len(slots),
                     folder='0x%08X' % folder, dll_clip_root='@camera:Suave')


def apply(raw, tb, ac, cj, tf, fe, ft, T):
    """overdose.main bytes -> (new bytes, report)"""
    a, rep_a = append_clips(raw, tb, ac, cj)
    b, rep_b = add_slots(a, tb, tf, fe, ft, T)
    c, rep_c = relocate(a, b, tb, tf, fe, ft, T)      # save-safe position (see step C)
    return c, dict(step_A_clips=rep_a, step_B_animdict=rep_b, step_C_relocated=rep_c)
