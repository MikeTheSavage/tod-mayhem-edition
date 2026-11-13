"""Build one STORED override zip from the selected options and verify every block it contains.

Pipeline (nothing is written until every check passed):
  1. base blocks: the block the game would load without this mod (GameFS: configpc filesystem order, so TOD HD's
     rebuilt block when hd_override*.zip carries it); each must round-trip exactly through the fragment codec.
  2. ops: every option's generator, in the fixed OPTIONS order, against the untouched base (values computed from the
     player's data). Ops are merged per (block, fragment, node, property); two options that want the same property are
     an error (no silent last-wins), different properties of one entity or block merge into one rebuilt block.
  3. apply: set records changed in place (or added to the entity's set group when the data had none); re-chunk with
     the editor rule; untouched payloads stay byte-identical.
  4. verify per block: fragment writer's verify_block (header, INFO, checksums vs the proven tables, untouched
     payloads, only intended properties, references, parent chains, ordering invariant, independent reader), plus:
     exact re-parse of every edited fragment, header propCk/cmdCk recomputed from the tables, create records
     (node id + type, in order) identical in every fragment (save rule: saves address nodes by creation order),
     read-back of every planned value, and an independent diff base->output attributed to the options.
  5. zip: STORED, entries read back through the engine's OpenZip/FindFile port, no name-hash collision with
     blocks.naz.
"""
import hashlib
import json
import os
import struct
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), 'lib')
if LIB not in sys.path:
    sys.path.insert(0, LIB)
sys.dont_write_bytecode = True
import todblock as tb   # noqa: E402
import todfrag as tf    # noqa: E402
import fragedit as fe   # noqa: E402
import fragtool as ft   # noqa: E402

from . import options as opt    # noqa: E402
from .gamefs import GameFS      # noqa: E402

ZIP_NAME = 'todgameplay_data.zip'
TOOL_VERSION = '1.0'
MAIN_ENTRY = opt.MAIN


class BuildError(Exception):
    pass


def sha(b):
    return hashlib.sha256(b).hexdigest()


def table_checksums():
    pt = json.load(open(os.path.join(LIB, 'property_table.json')))
    ct = json.load(open(os.path.join(LIB, 'command_table.json')))

    def ck(strings):
        buf = bytearray()
        for i, s in enumerate(strings):
            buf += struct.pack('<I', i) + s.encode('latin-1')
        return tb.tod_crc(bytes(buf))
    return ck(['%s:%s' % (n, t) for n, t in pt['properties']]), ck(ct['commands'])


class Context:
    """Read-only view of the base blocks; caches parsed blocks and fragment docs."""

    def __init__(self, fs):
        self.fs = fs
        self.T = ft.types()
        self.blocks = {}       # entry -> dict(raw, block, frs, src)
        self.docs = {}
        self.suave_manifest = None     # the music manifest of the suave option (None = suave/audio/build)

    def block(self, entry):
        if entry not in self.blocks:
            raw, src = self.fs.read(entry)
            b = tb.Block(raw, os.path.splitext(entry)[1])
            frs, _ = ft.fragments(b)
            self.blocks[entry] = dict(raw=raw, block=b, frs=frs, src=src)
        return self.blocks[entry]

    def fragment_names(self, entry):
        return [nm for _, nm in self.block(entry)['frs']]

    def frag_index(self, entry, frag):
        frs = self.block(entry)['frs']
        hits = [k for k, nm in frs if nm == frag]
        if len(hits) != 1:
            raise BuildError('%s: fragment %s found %d times' % (entry, frag, len(hits)))
        return hits[0]

    def doc(self, entry, frag):
        key = (entry, frag)
        if key not in self.docs:
            k = self.frag_index(entry, frag)
            self.docs[key] = fe.FragDoc(self.block(entry)['block'].payloads[k], self.T, frag)
        return self.docs[key]


def parse_selection(sel):
    """dict option -> level, validated; missing options take their vanilla default."""
    out = opt.defaults()
    for k, v in (sel or {}).items():
        k = k.strip().lower()
        if k not in opt.BY_KEY:
            raise BuildError('unknown option %r (known: %s)' % (k, ', '.join(opt.BY_KEY)))
        v = str(v).strip().lower()
        if v not in opt.BY_KEY[k]['levels']:
            raise BuildError('option %s: unknown level %r (levels: %s)' % (k, v, ', '.join(opt.BY_KEY[k]['levels'])))
        out[k] = v
    return out


def read_options_ini(path):
    import configparser
    cp = configparser.ConfigParser(inline_comment_prefixes=('#', ';'))
    cp.optionxform = str
    with open(path, encoding='utf-8') as f:
        cp.read_file(f)
    if not cp.has_section('gameplay'):
        raise BuildError('%s: no [gameplay] section' % path)
    return parse_selection(dict(cp.items('gameplay')))


def plan(ctx, selection):
    """All ops in apply order, merged; raises on a conflict."""
    merged, order = {}, []
    for o in opt.OPTIONS:
        level = selection[o['key']]
        try:
            ops = o['gen'](ctx, level)
        except opt.OptionError as e:
            raise BuildError('option %s=%s: %s' % (o['key'], level, e))
        for op in ops:
            op.option, op.level = o['key'], level
            k = op.key()
            if k in merged:
                prev = merged[k]
                raise BuildError('options %s and %s both change %s %s %s' % (
                    prev.option, op.option, op.fragment.split('/')[-1], op.path, op.prop))
            merged[k] = op
            order.append(op)
    return order


def apply_block(ctx, entry, ops):
    """Rebuild one block with its ops (sets only). Returns (old Block, new bytes, docs, intended)."""
    info = ctx.block(entry)
    raw = info['raw']
    ext = os.path.splitext(entry)[1]
    b = tb.Block(raw, ext)
    T = ctx.T
    docs, intended = {}, []
    for op in ops:
        k = ctx.frag_index(entry, op.fragment)
        if k not in docs:
            docs[k] = fe.FragDoc(b.payloads[k], T, op.fragment)
        d = docs[k]
        if op.node not in d.created_ids():
            raise BuildError('%s: node %#x not created' % (op.fragment, op.node))
        d.set_prop(op.node, op.prop, op.value)
        intended.append(dict(k=k, kind='set', node=op.node, prop=T.index(op.prop)))
    nb = tb.Block(raw, ext)
    for k, d in docs.items():
        nb.payloads[k] = d.payload()
    docs = {k: d for k, d in docs.items() if d.modified}
    return b, nb.serialize(), docs, [x for x in intended if x['k'] in docs]


def creates_of(payload, T):
    fr = tf.Fragment(payload, T)
    return [(r.node, r.raw) for r in fr.records if r.op == tf.CREATE]


def diff_fragment(old_payload, new_payload, T):
    """{(node, prop index): (old raw, new raw)} for every set value that differs (last record wins)."""
    oe, _ = ft.entity_map(tf.Fragment(old_payload, T), T)
    ne, _ = ft.entity_map(tf.Fragment(new_payload, T), T)
    out = {}
    for n in set(oe) | set(ne):
        op_ = oe.get(n, [None, {}])[1]
        np_ = ne.get(n, [None, {}])[1]
        for p in set(op_) | set(np_):
            if op_.get(p) != np_.get(p):
                out[(n, p)] = (op_.get(p), np_.get(p))
    return out


def verify(ctx, entry, old, new_bytes, docs, intended, ops):
    T = ctx.T
    ext = os.path.splitext(entry)[1]
    try:
        rep, chk = ft.verify_block(old, new_bytes, ext, docs, intended, T)
    except ft.Fail as e:
        raise BuildError('%s: fragment writer verification failed: %s' % (entry, e))
    # exact re-parse of the whole output block (every fragment, not only the edited ones)
    rt = ft.roundtrip_block(new_bytes, ext, T)
    if rt['fail'] or not rt['block_identical']:
        raise BuildError('%s: output block does not round-trip: %s' % (entry, rt['fail'][:3]))
    # header checksums recomputed from the proven tables
    pck, cck = table_checksums()
    if (chk.prop_ck, chk.cmd_ck) != (pck, cck):
        raise BuildError('%s: header checksums %08X/%08X != tables %08X/%08X' % (entry, chk.prop_ck, chk.cmd_ck, pck, cck))
    # save rule: create records identical (same nodes, same types, same order) in every fragment
    creates_same = 0
    for k in range(old.n):
        if old.payloads[k] is chk.payloads[k] or old.payloads[k] == chk.payloads[k]:
            continue
        if creates_of(old.payloads[k], T) != creates_of(chk.payloads[k], T):
            raise BuildError('%s: create records changed in payload %d (would break saves)' % (entry, k))
        creates_same += 1
    # independent diff, attributed to the options; and read-back of each planned value
    want = {}
    for op in ops:
        k = ctx.frag_index(entry, op.fragment)
        want[(k, op.node, T.index(op.prop))] = op
    diffs, seen = [], set()
    for k in docs:
        for (n, p), (a, bb) in diff_fragment(old.payloads[k], chk.payloads[k], T).items():
            op = want.get((k, n, p))
            if op is None:
                raise BuildError('%s: unattributed change %s %#x %s' % (entry, docs[k].name, n, T.name(p)))
            t = T.parse(T.ptype(p))
            got = tf.decode_value(bb, 0, t)
            exp = op.value
            if isinstance(exp, float):
                ok = got == tf.f32(exp)
            elif isinstance(exp, list):
                ok = list(got) == [tf.f32(x) for x in exp]
            else:
                ok = got == exp
            if not ok:
                raise BuildError('%s: read-back %s %s = %r, planned %r' % (entry, op.path, op.prop, got, exp))
            seen.add((k, n, p))
            d = op.as_dict()
            d['old_bytes'] = None if a is None else a.hex()
            d['new_bytes'] = bb.hex()
            diffs.append(d)
    missing = [w for w in want if w not in seen]
    if missing:
        raise BuildError('%s: %d planned values did not change the block' % (entry, len(missing)))
    rep['5_output_roundtrip'] = dict(fragments=rt['fragments'], records=rt['records'])
    rep['6_header_checksums_recomputed'] = dict(propCk='%08X' % pck, cmdCk='%08X' % cck, header_match=True)
    rep['7_create_records_identical'] = dict(edited_payloads=creates_same)
    rep['8_readback_and_diff'] = dict(changes=len(diffs))
    for f in rep.get('4_edited_fragments', []):
        f.pop('world_after', None)
    return rep, diffs


def write_zip(path, entries, naz):
    tmp = path + '.part'
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as zf:
        for entry, data in entries:
            zi = zipfile.ZipInfo(entry, date_time=(2005, 8, 21, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            zi.create_system = 0
            zi.external_attr = 0
            zf.writestr(zi, data)
    a = tb.Archive(tmp)
    out = []
    for entry, data in entries:
        hit = a.find('/' + entry)
        if a.naz or hit is None or hit[2] != entry or hit[1] != len(data):
            raise BuildError('zip: engine lookup failed for %s' % entry)
        with open(tmp, 'rb') as f:
            f.seek(hit[0])
            if f.read(hit[1]) != data:
                raise BuildError('zip: bytes at the engine offset differ for %s' % entry)
        out.append(dict(entry=entry, data_offset=hit[0], bytes=len(data), sha256=sha(data), engine_lookup_ok=True))
    collide = [n for c, (_, _, n) in naz.table.items() if c in a.table and n != a.table[c][2]]
    if collide:
        raise BuildError('zip: name hash collides with ' + collide[0])
    os.replace(tmp, path)
    return out


def guard_out(path, game_dir):
    """The zip is written to --out only; never into the game folder (the installer copies it)."""
    p = os.path.normcase(os.path.abspath(path))
    bad = [game_dir, r'C:\GOG Games\Total Overdose']
    for b in bad:
        b = os.path.normcase(os.path.abspath(b))
        if p == b or p.startswith(b + os.sep):
            raise BuildError('refusing to write into %s (build to a work folder, then install)' % b)


def build(game_dir, selection, out_zip, report_dir=None, saves_dir=None, log=print, ctx=None, suave_manifest=None):
    t0 = time.time()
    selection = parse_selection(selection)
    guard_out(out_zip, game_dir)
    if report_dir:
        guard_out(report_dir, game_dir)
    if ctx is None:
        ctx = Context(GameFS(game_dir, skip=[ZIP_NAME, os.path.basename(out_zip)]))
    if suave_manifest:
        ctx.suave_manifest = suave_manifest
    fs = ctx.fs
    report, zip_entries = build_blocks(ctx, selection, log)
    report.update(game_dir=os.path.abspath(game_dir))
    if saves_dir:
        from . import savecheck
        report['saves'] = savecheck.check(saves_dir, game_dir, report['changes'])
    report['seconds'] = round(time.time() - t0, 1)
    if not zip_entries:
        report['zip'] = None
        report['result'] = 'NOTHING TO BUILD (every option is at its vanilla level)'
    else:
        os.makedirs(os.path.dirname(os.path.abspath(out_zip)), exist_ok=True)
        report['zip'] = dict(path=os.path.abspath(out_zip), entries=write_zip(out_zip, zip_entries, fs.naz()))
        report['zip']['bytes'] = os.path.getsize(out_zip)
        with open(out_zip, 'rb') as f:
            report['zip']['sha256'] = hashlib.sha256(f.read()).hexdigest()
        report['result'] = 'PASS'
    if report_dir:
        write_reports(report, report_dir)
    return report


def build_blocks(ctx, selection, log=print):
    """Plan, apply and verify; returns (report without zip/saves, [(entry, block bytes)]). Raises BuildError."""
    selection = parse_selection(selection)
    ops = plan(ctx, selection)
    by_entry = {}
    for op in ops:
        by_entry.setdefault(op.entry, []).append(op)
    report = dict(tool='TOD Gameplay+ data builder', version=TOOL_VERSION, filesystem_order=ctx.fs.order,
                  selection=selection, blocks=[], changes=[])
    zip_entries = []
    for entry in sorted(by_entry):
        eops = by_entry[entry]
        info = ctx.block(entry)
        ext = os.path.splitext(entry)[1]
        r0 = ft.roundtrip_block(info['raw'], ext, ctx.T)
        if r0['fail'] or not r0['block_identical']:
            raise BuildError('%s: base block does not round-trip (%s): refusing to edit it' % (entry, r0['fail'][:3]))
        old, new_bytes, docs, intended = apply_block(ctx, entry, eops)
        if new_bytes == info['raw']:
            continue
        vr, diffs = verify(ctx, entry, old, new_bytes, docs, intended, eops)
        report['blocks'].append(dict(entry=entry, base=info['src'], base_sha256=sha(info['raw']),
                                     base_bytes=len(info['raw']), out_sha256=sha(new_bytes), out_bytes=len(new_bytes),
                                     base_roundtrip=dict(fragments=r0['fragments'], records=r0['records']),
                                     options=sorted({op.option for op in eops}), checks=vr))
        report['changes'] += diffs
        zip_entries.append((entry, new_bytes))
        log('  %-55s %3d values  (base: %s)  verified' % (entry.split('/')[-1], len(diffs), info['src']['archive']))
    if selection.get('suave_anims') == 'on':
        build_suave(ctx, report, zip_entries, log)
    if selection.get('suave', 'off') != 'off':
        build_suave_data(ctx, report, zip_entries, log, selection['suave'], music=selection.get('suave_music', 'on') != 'off')
    if selection.get('seamless', 'off') != 'off':
        build_seamless_check(ctx, report, zip_entries, log)
    build_menu_texts(ctx, report, zip_entries, selection, log)
    if selection.get('bigger_fights', 'off') != 'off':
        build_bigger_fights(ctx, report, zip_entries, log)
    if selection.get('hud_textures', 'off') != 'off':
        build_hud_textures(ctx, report, zip_entries, log)
    return report, zip_entries


def build_menu_texts(ctx, report, zip_entries, selection, log=print):
    """Menu text replacements of the selected options (gameplay/menus.py TEXT_EDITS), per language block; starts from
    the block another option already put into the zip (suave edits overdose_uk.main) so both edits survive."""
    from . import menus as mn
    try:
        per_lang = mn.text_edits_for(selection)
    except mn.MenuError as e:
        raise BuildError('menu texts: %s' % e)
    if not per_lang:
        return
    if selection.get('difficulty', 'normal') != 'normal' and selection.get('difficulty_menu', 'off') != 'off':
        raise BuildError('difficulty_menu needs difficulty = normal: the DLL applies the hard / brutal factors at run time '
                         '(a baked level would be applied twice; the DLL would refuse its tuning)')
    reps = []
    for lang in mn.LANGS:
        if lang not in per_lang:
            continue
        entry = mn.lang_block(lang)
        k = next((i for i, (e, _) in enumerate(zip_entries) if e == entry), None)
        info = ctx.block(entry)
        start = zip_entries[k][1] if k is not None else info['raw']
        if k is None:
            r0 = ft.roundtrip_block(start, '.main', ctx.T)
            if r0['fail'] or not r0['block_identical']:
                raise BuildError('%s: base block does not round-trip: refusing to edit it' % entry)
        try:
            out, rep = mn.apply_texts(start, lang, per_lang[lang])
        except mn.MenuError as e:
            raise BuildError('menu texts: %s' % e)
        if out == start:
            continue
        rt = ft.roundtrip_block(out, '.main', ctx.T)
        if rt['fail'] or not rt['block_identical']:
            raise BuildError('menu texts: the new %s does not round-trip: %s' % (entry, rt['fail'][:3]))
        if k is None:
            zip_entries.append((entry, out))
            report['blocks'].append(dict(entry=entry, base=info['src'], base_sha256=sha(info['raw']), base_bytes=len(info['raw']),
                                         out_sha256=sha(out), out_bytes=len(out), options=['menu_texts'], checks={}))
        else:
            zip_entries[k] = (entry, out)
            b = next(x for x in report['blocks'] if x['entry'] == entry)
            b['options'] = sorted(set(b['options']) | {'menu_texts'})
            b['out_sha256'], b['out_bytes'] = sha(out), len(out)
        rep['started_from'] = 'output of another option' if k is not None else 'base block'
        reps.append(rep)
        log('  %-55s %s  verified' % (entry.split('/')[-1], ', '.join('Menu #%d %r -> %r' % (t['ordinal'], t['old'], t['new']) for t in rep['texts'])))
    report['menu_texts'] = reps


def build_bigger_fights(ctx, report, zip_entries, log=print):
    """The bigger_fights option: bigger pools in overdose.main, dormant extra placements in every mission block.
    Runs last and starts from the block another option already put into the zip. It adds nodes in the middle of blocks on
    purpose, so verify() (which refuses that) does not apply; gameplay/bigger_fights.py has its own checks and the DLL
    keeps separate save slots."""
    from . import bigger_fights as bf

    def start(entry):
        k = next((i for i, (e, _) in enumerate(zip_entries) if e == entry), None)
        info = ctx.block(entry)
        raw = zip_entries[k][1] if k is not None else info['raw']
        if k is None:
            r0 = ft.roundtrip_block(raw, os.path.splitext(entry)[1], ctx.T)
            if r0['fail'] or not r0['block_identical']:
                raise BuildError('%s: base block does not round-trip: refusing to edit it' % entry)
        return k, info, raw

    def put(entry, k, info, out):
        if k is None:
            zip_entries.append((entry, out))
            report['blocks'].append(dict(entry=entry, base=info['src'], base_sha256=sha(info['raw']), base_bytes=len(info['raw']),
                                         out_sha256=sha(out), out_bytes=len(out), options=['bigger_fights'], checks={}))
        else:
            zip_entries[k] = (entry, out)
            b = next((x for x in report['blocks'] if x['entry'] == entry), None)
            if b is not None:
                b['options'] = sorted(set(b['options']) | {'bigger_fights'})
                b['out_sha256'], b['out_bytes'] = sha(out), len(out)

    rep = dict(missions=[])
    try:
        k, info, raw = start(MAIN_ENTRY)
        out, r = bf.apply_main(raw, tb, tf, fe, ft, ctx.T)
        put(MAIN_ENTRY, k, info, out)
        rep['pools'] = r
        log('  bigger fights: pools %s  verified' % ', '.join('%s %d -> %d' % (n.split('.')[0], x['before'], x['after']) for n, x in r.items()))
        pe = opt.SHARED + bf.PREFAB_BLOCK
        prefab = bf.load_prefab(ctx.block(pe)['raw'], pe, tb, tf, fe, ft, ctx.T)
        for entry in bf.mission_entries(ctx.fs):
            k, info, raw = start(entry)
            out, r = bf.apply_mission(raw, entry, tb, tf, fe, ft, ctx.T, prefab)
            rep['missions'].append(r)
            if out != raw:
                put(entry, k, info, out)
                log('  %-55s %3d hostile, +%d / +%d / friend +%d, %d vehicles  verified' % (entry.split('/')[-1], r['hostile'], r['tier1'], r['tier2'], r['tier_friend'], r['squads']))
    except bf.BigFightsError as e:
        raise BuildError('bigger fights: %s' % e)
    report['bigger_fights'] = rep


def build_hud_textures(ctx, report, zip_entries, log=print):
    """The hud_textures option: swaps HUD texture records + payloads in overdose.main. Runs LAST and
    starts from the block another option already put into the zip (fragments are untouched, so every earlier edit
    survives byte for byte); checks in gameplay/hud_textures.py and lib/todtexture.py. A texture that breaks a rule is
    skipped and reported, the rest still go in."""
    from . import hud_textures as ht
    entry = ht.ENTRY
    k = next((i for i, (e, _) in enumerate(zip_entries) if e == entry), None)
    info = ctx.block(entry)
    start = zip_entries[k][1] if k is not None else info['raw']
    try:
        out, rep = ht.apply(start)
    except ht.HudTexturesError as e:
        raise BuildError('hud_textures: %s' % e)
    rt = ft.roundtrip_block(out, '.main', ctx.T)
    if rt['fail'] or not rt['block_identical']:
        raise BuildError('hud_textures: the new overdose.main does not round-trip: %s' % (rt['fail'][:3],))
    old_b, new_b = tb.Block(start, '.main'), tb.Block(out, '.main')
    frags = [i for i, _ in ft.fragments(old_b)[0]]
    if any(old_b.payloads[i] != new_b.payloads[i] for i in frags):
        raise BuildError('hud_textures: a fragment payload changed')
    rep['started_from'] = 'output of another option' if k is not None else 'base block'
    rep['fragments_identical'] = len(frags)
    report['hud_textures'] = rep
    for s in rep['skipped']:
        log('  hud_textures: SKIPPED %s (%s)' % (s['name'], s['why']))
    if out == start:
        return
    if k is None:
        zip_entries.append((entry, out))
        report['blocks'].append(dict(entry=entry, base=info['src'], base_sha256=sha(info['raw']), base_bytes=len(info['raw']),
                                     out_sha256=sha(out), out_bytes=len(out), options=['hud_textures'], checks={}))
    else:
        zip_entries[k] = (entry, out)
        b = next((x for x in report['blocks'] if x['entry'] == entry), None)
        if b is not None:
            b['options'] = sorted(set(b['options']) | {'hud_textures'})
            b['out_sha256'], b['out_bytes'] = sha(out), len(out)
    log('  %-55s %d HUD textures, %d -> %d bytes  verified' % (entry.split('/')[-1], len(rep['textures']),
                                                               rep['bytes_before'], rep['bytes_after']))


def build_seamless_check(ctx, report, zip_entries, log=print):
    """The seamless option's extra check: the built blocks re-scanned (gameplay/seamless.py check):
    teleports empty, moved box where planned, box order walk with >= 20 m, the other boxes untouched."""
    from . import seamless as sm
    rep, fails = sm.check(ctx, zip_entries, tb, ft, fe)
    report['seamless'] = rep
    if fails:
        raise BuildError('seamless: ' + '; '.join(fails[:3]))
    for c in rep['corridors']:
        log('  seamless %-7s %s moved %.1f m, box order %s -> %s (gap %.1f m), %d teleports cleared  verified' % (
            c['key'], c['moved']['name'], c['moved']['by_m'], c['order_before']['state'], c['order_after']['state'],
            c['order_after']['gap'], c['teleports_cleared']))


def build_suave_data(ctx, report, zip_entries, log=print, level='on', music=True):
    """The suave option's block step: uk HUD texts + appended music stream records in overdose_uk.main,
    the music oggs as raw zip entries. Checks in gameplay/suave_data.py; the set ops went through verify() above.
    music=False (option suave_music = off): no stream records and no oggs: the move is installed without its music
    (the player gave no copy of the song; the installer then also sets [suave] music = 0)."""
    from . import suave_data as sd
    try:
        segs = sd.load_manifest(getattr(ctx, 'suave_manifest', None)) if music else []
        info = ctx.block(sd.UK)
        raw = info['raw']
        r0 = ft.roundtrip_block(raw, '.main', ctx.T)
        if r0['fail'] or not r0['block_identical']:
            raise BuildError('%s: base block does not round-trip: refusing to edit it' % sd.UK)
        if any(e == sd.UK for e, _ in zip_entries):
            raise BuildError('%s is already edited by another option' % sd.UK)
        out, rep = sd.apply_uk(raw, segs, sd.TEXTS_SLOT10 if level == 'on_slot10' else sd.TEXTS)
        rt = ft.roundtrip_block(out, '.main', ctx.T)
        if rt['fail'] or not rt['block_identical']:
            raise BuildError('suave: the new overdose_uk.main does not round-trip: %s' % (rt['fail'][:3],))
        oggs = [(s['entry'], s['data']) for s in segs]
        clash = sd.sound_collisions(ctx.fs, [e for e, _ in oggs])
        if clash:
            raise BuildError('suave: ' + clash[0])
    except sd.SuaveDataError as e:
        raise BuildError('suave: %s' % e)
    zip_entries.append((sd.UK, out))
    zip_entries.extend(oggs)
    report['blocks'].append(dict(entry=sd.UK, base=info['src'], base_sha256=sha(raw), base_bytes=len(raw),
                                 out_sha256=sha(out), out_bytes=len(out), options=['suave'],
                                 checks=dict(base_roundtrip=dict(fragments=r0['fragments'], records=r0['records']),
                                             output_roundtrip=dict(fragments=rt['fragments'], records=rt['records']))))
    report['suave'] = dict(rep, oggs=[dict(entry=s['entry'], bytes=len(s['data']), sha256=sha(s['data']),
                                           samples=s['samples'], seconds=s['seconds']) for s in segs])
    log('  %-55s +%d stream records, 2 HUD texts  verified' % (sd.UK.split('/')[-1], len(segs)))
    if segs:
        log('  %-55s %d music segments (ogg, mono 44.1 kHz)  verified' % ('data/sounds/streamedpowermoves/', len(segs)))
    else:
        log('  suave: no music segments (suave_music = off): the move plays without music')

def build_suave(ctx, report, zip_entries, log=print):
    """The suave_anims option: appends clip records and AnimDict slots to overdose.main, starting from the
    output of the set options (already verified above) or the base block; checks in gameplay/suave_anims.py."""
    from . import suave_anims as sa
    import animcodec as ac
    import clipjson as cj
    entry = sa.ENTRY
    info = ctx.block(entry)
    base = info['raw']
    k = next((i for i, (e, _) in enumerate(zip_entries) if e == entry), None)
    start = zip_entries[k][1] if k is not None else base
    if k is None:
        r0 = ft.roundtrip_block(base, '.main', ctx.T)
        if r0['fail'] or not r0['block_identical']:
            raise BuildError('%s: base block does not round-trip: refusing to edit it' % entry)
    try:
        out, rep = sa.apply(start, tb, ac, cj, tf, fe, ft, ctx.T)
    except (sa.SuaveError, ft.Fail, fe.EditError) as e:
        raise BuildError('suave_anims: %s' % e)
    pck, cck = table_checksums()
    chk = tb.Block(out, '.main')
    if (chk.prop_ck, chk.cmd_ck) != (pck, cck):
        raise BuildError('suave_anims: header checksums do not match the proven tables')
    if k is None:
        zip_entries.append((entry, out))
        report['blocks'].append(dict(entry=entry, base=info['src'], base_sha256=sha(base), base_bytes=len(base),
                                     out_sha256=sha(out), out_bytes=len(out), options=['suave_anims'], checks={}))
    else:
        zip_entries[k] = (entry, out)
        b = next(x for x in report['blocks'] if x['entry'] == entry)
        b['options'] = sorted(set(b['options']) | {'suave_anims'})
        b['out_sha256'], b['out_bytes'] = sha(out), len(out)
    report['suave_anims'] = dict(entry=entry, started_from='output of the set options' if k is not None else 'base block',
                                 in_sha256=sha(start), out_sha256=sha(out), **rep)
    log('  %-55s +%d clips, +%d AnimDict entities  verified' % (
        entry.split('/')[-1], len(rep['step_A_clips']['added']), rep['step_B_animdict']['creates_appended']))


def fmt(v):
    if isinstance(v, float):
        return '%.6g' % v
    if isinstance(v, list):
        return '(' + ', '.join('%.6g' % x for x in v) + ')'
    return str(v)


def write_reports(report, d):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'report.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1, default=str)
    by_opt = {}
    for c in report['changes']:
        by_opt.setdefault(c['option'], []).append(c)
    lines = ['# TOD Gameplay+ data build: per-option diff', '',
             'Result: %s. Game folder: %s. Read order: %s.' % (report['result'], report['game_dir'],
                                                                ', '.join(report['filesystem_order'])), '',
             'Selection: ' + ', '.join('%s=%s' % kv for kv in report['selection'].items()), '']
    for b in report['blocks']:
        lines.append('- %s: base %s (%s), %d -> %d bytes, sha256 %s...' % (
            b['entry'].split('/')[-1], b['base']['archive'], b['base_sha256'][:16], b['base_bytes'], b['out_bytes'],
            b['out_sha256'][:16]))
    for o, cs in by_opt.items():
        lines += ['', '## %s = %s (%d values)' % (o, cs[0]['level'], len(cs)), '',
                  '| block | fragment | entity | property | old | new | note |', '|---|---|---|---|---|---|---|']
        for c in cs:
            lines.append('| %s | %s | %s | %s | %s | %s | %s |' % (
                c['block'], c['fragment'].split('/')[-1], c['path'], c['prop'], fmt(c['old']), fmt(c['new']), c['note']))
    if report.get('suave_anims'):
        s = report['suave_anims']
        a, b = s['step_A_clips'], s['step_B_animdict']
        lines += ['', '## suave_anims = on', '',
                  'overdose.main records %d -> %d (INFO %d -> %d bytes), AnimDict creates %d -> %d (appended).' % (
                      a['records_before'], a['records_after'], a['info_before'], a['info_after'],
                      b['creates_before'], b['creates_after']), '',
                  '| slot | clip | CPS | loop mode |', '|---|---|---|---|']
        for sl in b['slots']:
            lines.append('| %s | %s | %.6g | %s |' % (sl['slot'], sl['target1'], sl['speed'], sl['loopmode1']))
    if report.get('suave'):
        s = report['suave']
        lines += ['', '## suave (block step)', '',
                  'overdose_uk.main records %d -> %d (INFO %d -> %d bytes); old records byte-identical except the HUD text '
                  'record.' % (s['records_before'], s['records_after'], s['info_before'], s['info_after']), '']
        for t in s['texts']:
            lines.append('- uk_HUD.txt #%d %s: %r -> %r' % (t['ordinal'], t['key'], t['old'], t['new']))
        for r in s['stream_records']:
            lines.append('- record %d %s -> %s (%d samples, %.3f s)' % (r['record'], r['name'], r['file'], r['samples'], r['seconds']))
        for o in s['oggs']:
            lines.append('- zip %s (%d bytes, sha256 %s...)' % (o['entry'], o['bytes'], o['sha256'][:16]))
    if report.get('menu_texts'):
        lines += ['', '## menu texts (gameplay/menus.py)', '']
        for r in report['menu_texts']:
            for t in r['texts']:
                lines.append('- %s Menu #%d %s: %r -> %r (%s; every other record and entry byte-identical)' % (
                    r['entry'].split('/')[-1], t['ordinal'], t['key'], t['old'], t['new'], r['started_from']))
    if report.get('seamless'):
        s = report['seamless']
        lines += ['', '## seamless (box order re-check: %s)' % s['result'], '']
        for c in s['corridors']:
            lines.append('- %s: %s in %s moved %.2f m from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f); order along the '
                         'corridor %s (gap %.1f m) -> %s (gap %.1f m); %d Teleport_to cleared' % (
                             c['label'], c['moved']['name'], c['moved']['block'], c['moved']['by_m'],
                             *c['moved']['old'], *c['moved']['new'], c['order_before']['state'],
                             c['order_before']['gap'], c['order_after']['state'], c['order_after']['gap'],
                             c['teleports_cleared']))
        for st, text in s['checks']:
            lines.append('  - %s %s' % (st, text))
    if report.get('hud_textures'):
        h = report['hud_textures']
        lines += ['', '## hud_textures = on (%d textures in overdose.main, %d skipped; started from the %s; %d fragments '
                  'byte-identical)' % (len(h['textures']), len(h['skipped']), h['started_from'], h['fragments_identical']),
                  '', '| texture | group | before | after |', '|---|---|---|---|']
        for t in h['textures']:
            lines.append('| %s | %s | %s | %s |' % (t['name'], t['group'], t['before'], t['after']))
        for t in h['skipped']:
            lines.append('- SKIPPED %s: %s' % (t['name'], t['why']))
    if report.get('saves'):
        s = report['saves']
        lines += ['', '## Save check (%d saves)' % len(s.get('saves', [])), '', s.get('summary', '')]
        for h in s.get('hits', []):
            lines.append('- %s: %s' % (h['what'], ', '.join(h['saves'])))
    with open(os.path.join(d, 'diff.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
