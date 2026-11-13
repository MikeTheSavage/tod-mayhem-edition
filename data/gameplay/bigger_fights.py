"""Bigger Fights, data half (option `bigger_fights`): more enemies at the start of a fight and the heat squads' vehicles.

This option BREAKS SAVES on purpose (a deliberate design choice): it adds nodes in the middle of overdose.main and
in mission blocks, so the node indices a save stores no longer match. The DLL keeps the two worlds apart: with
[bigger_fights] enabled the game saves to BFSlotNN instead of SlotNN (see docs/mods/bigger_fights.md).

overdose.main
    The pools the resource controllers hand out grow: AICharacter10..14 in DefaultEnemyResources.fragment (9 -> 14
    character shells, each with its CharacterAIControlled.fragment instance), 8 more weapon props, 3 more wear props.
    Nothing in the code counts the shells (the lists are script lists); the DLL raises the live caps.

every mission block with hostile placements
    Extra placements, one per-enemy Folder cloned next to a donor's Folder in the same fragment:
        Folder BF<tier>_<n> { character_placeholder { Placeholders { weapon / wear placeholders } } }
    The clone has no track, no Trigger_Root, no sounds and no reference to anything outside itself. It is dormant (own
    flags bit 0) and not a kill objective, not a VIP and not bound to a track. Dormant clones do nothing at all: the
    DLL wakes the ones of the chosen difficulty (tier 1 BRUTAL and PSYCHO, tier 2 PSYCHO only) when the donor is live.
    A clone keeps the donor's position until then, which is how the DLL finds its donor (same parent, same place).
    A block with buddies or an escort gets a smaller share (tier 3) and no vehicles.

every mission and intermission block without buddies
    Three dormant vehicles for heat squads: the game's own police assault car (read from the player's Downtown block) with
    three gunmen and an 8 point track, top level of the block's root fragment. In a mission the gunmen look like its
    commonest gunman; between missions they are police (the only character model that is always loaded) in faction 9.
    The DLL writes the road route into the track points and starts the car with the game's own commands.

Verification (`apply_block` raises on any miss): every old create record keeps node, type and order in its fragment; the
other fragments and INFO are byte-identical; the block round-trips; each clone reads back dormant and harmless.
"""
import math
import os
import struct

BF_LEVELS = ['off', 'on']
ADD_CHARACTERS = 5                 # 9 -> 14 shells
ADD_WEAPONS = 8                    # 16 -> 24 props (the game hands out 15, the DLL 23)
ADD_WEAR = 3                       # 7 -> 10 props (the game hands out 6, the DLL 9)
F_ENEMY = 'DefaultEnemyResources.fragment'
F_WEAPON = 'DefaultWeaponResources.fragment'
F_WEAR = 'DefaultWearResources.fragment'

TIER1_SHARE = 0.50                 # of ALL hostile placements of the block: BRUTAL +50 %
TIER2_SHARE = 0.25                 # PSYCHO: tier 1 + this = +75 %
FRIEND_SHARE = 0.25                # a block with buddies or an escort: +25 % on both levels, tier 3, no vehicles
TIER_FRIEND = 3
SQUADS_PER_BLOCK = 3               # vehicles the DLL can send at once
TRACK_POINTS = 8                   # the DLL writes the road route into them
GUNMEN_PER_CAR = 3
NO_SQUAD_WORDS = ('_race.', 'tutorial', 'gameover')      # a race or the tutorial is no place for them
PREFAB_BLOCK = 'submap01_map02_lostoros_downtown.submap'
PREFAB_FRAGMENT = 'Police_Assault_MissionStructure_03.fragment'
PREFAB_ROOT = 'Police_Vehicle_01'
MAX_CLONES_PER_DONOR = 2
MAX_CLONES_PER_BLOCK = 120         # the DLL tracks this many
BOSS_HEALTH = 1000.0

# sub types that are not plain gunmen (gang_wars skips the same): 4 escort, 8 special, 5 / 6 / 7 / 9 civilians and
# animals; every faction 9 placement of the shipped missions is one of 0-3, 10-17
SKIP_SUBTYPES = {4, 5, 6, 7, 8, 9}
SKIP_PATH_WORDS = ('cutscene', 'cinema', 'cine_', 'movie', 'intro', 'outro', 'dummy', 'tutorial', 'passenger', 'vehicle')
KEEP_TYPES = ('character_placeholder', 'weapon_placeholder', 'wear_placeholder', 'Folder')


class BigFightsError(Exception):
    pass


class _RefMap(dict):
    """Entity values of a clone: inside the clone -> the new node, anything else -> none."""
    def get(self, k, default=None):
        return dict.get(self, k, None)


def _ty(d, n):
    return (d.type_of(n) or '').split('(')[0]


def _parent_map(d):
    return {n: (d.get(n, 'parent') or 0) for n in d.created_ids()}


class FragPlan:
    """One fragment's parent / child / type tables, built once (FragDoc's own queries are O(records) per call)."""
    def __init__(self, d, tf):
        self.d = d
        self.tf = tf
        self.types, self.sets = {}, {}
        self.create = {}
        for r in d.records:
            if r.op == tf.CREATE:
                if r.node not in self.create:
                    self.create[r.node] = r
                    self.types[r.node] = tf.decode_value(r.raw, 0, ('string',))
            elif r.op == tf.SET:
                self.sets.setdefault(r.node, []).append(r)
        self.parent = {n: (d.get(n, 'parent') or 0) for n in self.create}
        self.kids = {}
        for n, p in self.parent.items():
            self.kids.setdefault(p, []).append(n)

    def base(self, n):
        return (self.types.get(n) or '').split('(')[0]

    def subtree(self, n):
        out, stack = [], [n]
        while stack:
            x = stack.pop()
            out.append(x)
            stack.extend(reversed(self.kids.get(x, [])))
        return out

    def path(self, n):
        names, seen = [], set()
        while n and n not in seen and n in self.create:
            seen.add(n)
            names.append(self.d.get(n, 'name') or '')
            n = self.parent.get(n, 0)
        return '/'.join(reversed(names))


def is_eligible(p, n, objectives=False):
    """A plain hostile gunman in his own Folder: the only kind that may be copied. (reason or None)
    objectives: also a mission's kill targets, for the blocks where every enemy is one (the copy is never a target)."""
    d = p.d
    if d.get(n, 'FactionID', 0) != 9:
        return 'faction'
    if d.get(n, 'IsVIP') or d.get(n, 'Is_Buddy') or (d.get(n, 'MissionObjectiveKill') and not objectives):
        return 'vip, buddy or objective'
    if (d.get(n, 'OrgHealth') or 0.0) >= BOSS_HEALTH:
        return 'boss'
    if d.get(n, 'ResourceType', 1) != 1 or d.get(n, 'PopSensitiveType', 0) != 0:
        return 'resource'
    if d.get(n, 'AISubType', 0) in SKIP_SUBTYPES:
        return 'sub type'
    if (d.get(n, 'flags') or 0) & 1:
        return 'dormant'
    par = p.parent.get(n, 0)
    if not par or p.base(par) != 'Folder' or not p.parent.get(par, 0):
        return 'no folder'
    if sum(1 for x in p.subtree(par) if p.base(x) == 'character_placeholder') != 1:
        return 'shared folder'
    a = n
    while a:
        if p.base(a).startswith('vehicle'):
            return 'in a vehicle'
        a = p.parent.get(a, 0)
    low = p.path(n).lower()
    if any(w in low for w in SKIP_PATH_WORDS):
        return 'scripted path'
    return None


def choose(donors, hostile, share1=TIER1_SHARE, share2=TIER2_SHARE):
    """[(donor, tier)]: evenly spread over the block's creation order, at most MAX_CLONES_PER_DONOR per donor."""
    if not donors:
        return []
    want1 = math.ceil(hostile * share1)
    want2 = math.ceil(hostile * (share1 + share2)) - want1
    room = min(len(donors) * MAX_CLONES_PER_DONOR, MAX_CLONES_PER_BLOCK)
    want1 = min(want1, room)
    want2 = min(want2, room - want1)
    total = want1 + want2
    out = []
    count = {}
    for i in range(total):
        # even spread: i-th clone of `total` over the donors, wrapping for the second round
        k = (i * len(donors)) // max(total, 1) if total <= len(donors) else i % len(donors)
        count[k] = count.get(k, 0) + 1
        out.append((donors[k], 1 if i % 3 != 2 or want2 == 0 else 2))
    n1 = sum(1 for _, t in out if t == 1)
    # the 2-in-3 pattern gives the right shape; settle the exact counts by the end of the list
    i = len(out) - 1
    while n1 > want1 and i >= 0:
        if out[i][1] == 1:
            out[i] = (out[i][0], 2)
            n1 -= 1
        i -= 1
    i = len(out) - 1
    while n1 < want1 and i >= 0:
        if out[i][1] == 2:
            out[i] = (out[i][0], 1)
            n1 += 1
        i -= 1
    return out


def next_slot_alloc(taken, tf):
    """Fresh node ids for the block: same block bits as the source, slots above everything in the block."""
    state = dict(slot=max([tf.node_slot(n) for n in taken] + [0]) + 1)

    def alloc(like):
        hi = like & ~0xFF8FFFFF
        while True:
            n = hi | state['slot']
            state['slot'] += 1
            if n not in taken:
                taken.add(n)
                return n
    return alloc


def clone_batch(p, jobs, alloc, tf, fe, rnd_seed):
    """jobs = [(donor placeholder, name of the Folder clone, name of the placeholder clone)]. Adds the records to the
    FragDoc in one pass and returns [(root, placeholder clone)]."""
    d = p.d
    import random
    rnd = random.Random(rnd_seed)
    uids = set()
    for rs in p.sets.values():
        for r in rs:
            if r.prop in (fe.P_UID0, fe.P_UID1):
                uids.add(struct.unpack('<i', r.raw[:4])[0])
    new_creates, new_sets, out = [], [], []
    for ph, fname, pname in jobs:
        first = len(new_sets)
        folder = p.parent[ph]
        keep = [folder, ph]
        for x in p.subtree(ph):
            if x != ph and p.base(x) in KEEP_TYPES:
                keep.append(x)
        # a kept node's parent must be kept too (an odd child under a dropped node stays behind)
        ks = set(keep)
        keep = [x for x in keep if x == folder or p.parent[x] in ks]
        m = _RefMap()
        for x in keep:
            m[x] = alloc(ph)
        for x in keep:
            new_creates.append(tf.Record(m[x], tf.CREATE, raw=p.create[x].raw))
            for r in p.sets.get(x, []):
                raw = r.raw
                if r.prop == fe.P_PARENT:
                    if x != folder:
                        raw = d._encode(fe.P_PARENT, m[p.parent[x]])
                else:
                    raw = d._remap_value(r.prop, raw, m)
                if r.prop in (fe.P_UID0, fe.P_UID1):
                    v = struct.unpack('<i', raw[:4])[0]
                    if v != 0:
                        nv = 0
                        while nv == 0 or nv in uids:
                            nv = rnd.randint(-2**31, 2**31 - 1)
                        uids.add(nv)
                        raw = struct.pack('<i', nv)
                new_sets.append(tf.Record(m[x], tf.SET, r.prop, raw, r.hi))
        out.append((m[folder], m[ph]))
        want = {m[folder]: {'name': fname}, m[ph]: {
            'name': pname, 'flags': (d.get(ph, 'flags') or 0) | 1, 'MissionObjectiveKill': False, 'IsVIP': False,
            'DontHunt': False, 'Goto_Movement': 0, 'Status': 0, 'Locked': False, 'StoredDamage': 0.0,
            'Wait_until_Aggressive': 0.0}}
        for node, props in want.items():
            for prop, val in props.items():
                idx = d.T.index(prop)
                raw = d._encode(idx, val)
                hits = [r for r in new_sets[first:] if r.node == node and r.prop == idx]
                if hits:
                    hits[-1].raw = raw
                else:
                    new_sets.append(tf.Record(node, tf.SET, idx, raw))
    last_create = max(i for i, r in enumerate(d.records) if r.op == tf.CREATE)
    d.records[last_create + 1:last_create + 1] = new_creates
    d.records.extend(new_sets)
    d.modified = True
    d._gen += 1
    return out


def instances_in_block(docs, tf, fe):
    """Basename (lower) -> how many `fragment` properties name it in this block."""
    c = {}
    prop = docs[0].T.index('fragment')
    for d in docs:
        for r in d.records:
            if r.op == tf.SET and r.prop == prop:
                v = d.frag.value(r)
                if v:
                    k = os.path.basename(str(v).replace('\\', '/')).lower()
                    c[k] = c.get(k, 0) + 1
    return c


def apply_mission(raw, entry, tb, tf, fe, ft, T, prefab=None):
    """One mission block -> (new bytes, report); bytes are the input when the block has nothing to add."""
    ext = os.path.splitext(entry)[1]
    b = tb.Block(raw, ext)
    frs, _ = ft.fragments(b)
    docs = {k: fe.FragDoc(b.payloads[k], T, nm) for k, nm in frs}
    inst = instances_in_block(list(docs.values()), tf, fe)
    taken = set()
    for d in docs.values():
        for r in d.records:
            taken.add(r.node)
    alloc = next_slot_alloc(taken, tf)
    for objectives in (False, True):
        hostile = 0
        per_frag, plans, why = [], [], {}
        for k, nm in frs:
            d = docs[k]
            p = FragPlan(d, tf)
            phs = [n for n in p.create if p.base(n) == 'character_placeholder']
            here = 0
            donors = []
            for n in phs:
                if d.get(n, 'FactionID', 0) == 9:
                    here += 1
                r = is_eligible(p, n, objectives)
                if r is None and inst.get(os.path.basename(nm).lower(), 0) > 1:
                    r = 'instanced twice'
                if r:
                    why[r] = why.get(r, 0) + 1
                else:
                    donors.append(n)
            hostile += here
            plans.append((p, phs))
            if donors:
                per_frag.append((k, p, donors))
        all_donors = [(k, n) for k, p, ds in per_frag for n in ds]
        if all_donors:
            break
    friendly = any(friend_in(fp, n) for fp, phs in plans for n in phs)
    picks = choose(all_donors, hostile, FRIEND_SHARE if friendly else TIER1_SHARE, 0.0 if friendly else TIER2_SHARE)
    jobs = {}
    serial = {1: 0, 2: 0, 3: 0}
    for (k, n), tier in picks:
        tier = TIER_FRIEND if friendly else tier
        serial[tier] += 1
        nm = 'BF%d_%03d' % (tier, serial[tier])
        jobs.setdefault(k, []).append((n, nm, nm + 'e'))
    nb = tb.Block(raw, ext)
    made = []
    for k, p, _ in per_frag:
        if k not in jobs:
            continue
        for root, ph in clone_batch(p, jobs[k], alloc, tf, fe, zlib_seed(entry, k)):
            made.append((k, root, ph))
    touched = {k for k, _, _ in made}
    squads = 0
    base = os.path.basename(entry).lower()
    free_roam = base.startswith('intermission_')      # its friends stand around mission starts: no escort to protect
    if prefab is not None and (free_roam or not friendly) and not any(w in base for w in NO_SQUAD_WORDS):
        k = root_fragment(frs, inst)
        look = gunman_look(docs, all_donors)
        for i in range(SQUADS_PER_BLOCK):
            add_squad(docs[k], prefab, i, alloc, look, tf, fe)
            squads += 1
        touched.add(k)
    if not touched:
        return raw, dict(entry=entry, hostile=hostile, eligible=len(all_donors), tier1=0, tier2=0, tier_friend=0, squads=0, friendly=friendly, skipped=why)
    for k in touched:
        nb.payloads[k] = docs[k].payload()
    out = nb.serialize()
    verify_mission(raw, out, entry, made, tb, tf, fe, ft, T, touched)
    return out, dict(entry=entry, hostile=hostile, eligible=len(all_donors), tier1=serial[1], tier2=serial[2], tier_friend=serial[3], squads=squads,
                     friendly=friendly, skipped=why, bytes_added=len(out) - len(raw))


def zlib_seed(entry, k):
    import zlib
    return zlib.crc32(entry.encode('latin-1')) ^ k


def verify_mission(raw, out, entry, made, tb, tf, fe, ft, T, touched):
    ext = os.path.splitext(entry)[1]
    b0, b1 = tb.Block(raw, ext), tb.Block(out, ext)
    if bytes(b0.info) != bytes(b1.info):
        raise BigFightsError('%s: INFO changed' % entry)
    if b0.n != b1.n:
        raise BigFightsError('%s: the fragment count changed' % entry)
    changed = set(touched)
    for k in range(b0.n):
        if k not in changed and b0.payloads[k] != b1.payloads[k]:
            raise BigFightsError('%s: payload %d changed without a clone in it' % (entry, k))
    for k in changed:
        oc = [(r.node, r.raw) for r in tf.Fragment(b0.payloads[k], T).records if r.op == tf.CREATE]
        nc = [(r.node, r.raw) for r in tf.Fragment(b1.payloads[k], T).records if r.op == tf.CREATE]
        if nc[:len(oc)] != oc:
            raise BigFightsError('%s: old create records moved in payload %d' % (entry, k))
        if len({n for n, _ in nc}) != len(nc):
            raise BigFightsError('%s: a node id is used twice in payload %d' % (entry, k))
        d = fe.FragDoc(b1.payloads[k], T, 'check')
        p = FragPlan(d, tf)
        for kk, root, ph in made:
            if kk != k:
                continue
            nm = d.get(root, 'name') or ''
            if not nm.startswith('BF') or d.get(ph, 'name') != nm + 'e':
                raise BigFightsError('%s: clone %#x has the wrong name %r' % (entry, ph, nm))
            if not (d.get(ph, 'flags') or 0) & 1:
                raise BigFightsError('%s: clone %s is not dormant' % (entry, nm))
            if d.get(ph, 'MissionObjectiveKill') or d.get(ph, 'IsVIP') or d.get(ph, 'DontHunt'):
                raise BigFightsError('%s: clone %s is not harmless' % (entry, nm))
            sub = set(p.subtree(root))
            for x in sub:
                for r in p.sets.get(x, []):
                    t = T.parse(T.ptype(r.prop))
                    if r.prop == fe.P_PARENT or 'entity' not in repr(t):
                        continue
                    v = d.frag.value(r)
                    if t[0] == 'entity' and v not in (None, 0) and v not in sub:
                        raise BigFightsError('%s: clone %s still points at %#x (%s)' % (entry, nm, v, T.name(r.prop)))
    rt = ft.roundtrip_block(out, ext, T)
    if rt['fail'] or not rt['block_identical']:
        raise BigFightsError('%s: the block does not round-trip: %s' % (entry, rt['fail'][:3]))


def apply_main(raw, tb, tf, fe, ft, T):
    """overdose.main with the bigger pools -> (new bytes, report)."""
    b = tb.Block(raw, '.main')
    frs, _ = ft.fragments(b)
    block_ids = set()
    for k, nm in frs:
        for r in tf.Fragment(b.payloads[k], T).records:
            block_ids.add(r.node)
    rep = {}
    nb = tb.Block(raw, '.main')
    for suffix, add, prefix in ((F_ENEMY, ADD_CHARACTERS, 'AICharacter'), (F_WEAPON, ADD_WEAPONS, None), (F_WEAR, ADD_WEAR, None)):
        hits = [(k, nm) for k, nm in frs if nm.lower().endswith('/' + suffix.lower())]
        if len(hits) != 1:
            raise BigFightsError('overdose.main: %s found %d times' % (suffix, len(hits)))
        k, nm = hits[0]
        d = fe.FragDoc(b.payloads[k], T, nm)
        d.used_ids = block_ids
        ids = d.created_ids()
        roots = [n for n in ids if (d.get(n, 'parent') or 0) == 0]
        if len(roots) != len(ids):
            raise BigFightsError('%s: expected only top level entities' % suffix)
        src = roots[-1]
        count0 = len(roots)
        for i in range(add):
            name = '%s%d' % (prefix, count0 + 1 + i) if prefix else None
            d.clone(src, deep=True, name=name, seed=0x8F000 + i)
        if len(d.created_ids()) != count0 + add:
            raise BigFightsError('%s: the clone count is wrong' % suffix)
        nb.payloads[k] = d.payload()
        rep[suffix] = dict(before=count0, after=count0 + add)
    out = nb.serialize()
    chk = tb.Block(out, '.main')
    if bytes(chk.info) != bytes(b.info):
        raise BigFightsError('overdose.main: INFO changed')
    rt = ft.roundtrip_block(out, '.main', T)
    if rt['fail'] or not rt['block_identical']:
        raise BigFightsError('overdose.main: the block does not round-trip: %s' % (rt['fail'][:3],))
    return out, rep


def friend_in(p, n):
    """a placeholder the block must keep alive: a buddy, an escort or a friend"""
    d = p.d
    return bool(d.get(n, 'Is_Buddy')) or d.get(n, 'FactionID', 0) in (10, 18) or d.get(n, 'AISubType', 0) == 4


def root_fragment(frs, inst):
    """the block's root fragment: named by no `fragment` property of its own block"""
    roots = [k for k, nm in frs if inst.get(os.path.basename(nm).lower(), 0) == 0]
    if not roots:
        raise BigFightsError('no root fragment')
    return roots[0]


def load_prefab(raw, entry, tb, tf, fe, ft, T):
    """The game's own police assault car with its passengers and track, as records (the player's block, read only)."""
    ext = os.path.splitext(entry)[1]
    b = tb.Block(raw, ext)
    frs, _ = ft.fragments(b)
    hits = [k for k, nm in frs if nm.lower().endswith('/' + PREFAB_FRAGMENT.lower())]
    if len(hits) != 1:
        raise BigFightsError('%s not found once in %s' % (PREFAB_FRAGMENT, entry))
    d = fe.FragDoc(b.payloads[hits[0]], T, PREFAB_FRAGMENT)
    p = FragPlan(d, tf)
    roots = [n for n in p.create if d.get(n, 'name') == PREFAB_ROOT]
    if not roots:
        raise BigFightsError('%s not found' % PREFAB_ROOT)
    root = roots[0]
    nodes = p.subtree(root)
    veh = [n for n in nodes if p.base(n) == 'vehicle_placeholder']
    trk = [n for n in nodes if p.base(n) == 'Track']
    pts = [n for n in nodes if p.base(n) == 'AI_track_point']
    guns = [n for n in nodes if p.base(n) == 'character_placeholder']
    if len(veh) != 1 or len(trk) != 1 or len(pts) < 2 or len(guns) < 2:
        raise BigFightsError('the police car has another shape: %d %d %d %d' % (len(veh), len(trk), len(pts), len(guns)))
    return dict(p=p, d=d, root=root, nodes=nodes, vehicle=veh[0], track=trk[0], points=pts, gunmen=guns)


def gunman_look(docs, donors):
    """what a mission's own gunmen look like (the commonest donor); None: the police look, the only model always loaded"""
    c = {}
    for k, n in donors:
        d = docs[k]
        key = (d.get(n, 'Model', 0), d.get(n, 'Real_Active_Texture_Set', 0), d.get(n, 'OrgHealth', 0.0), d.get(n, 'Use_AISubType_Data', False), d.get(n, 'AISubType', 0))
        c[key] = c.get(key, 0) + 1
    if not c:
        return None
    return max(sorted(c), key=lambda x: c[x])


def add_squad(d, pf, i, alloc, look, tf, fe):
    """Vehicle i of the block's squads, dormant:
        Folder BFS<i> { vehicle_placeholder BFV<i> { pivot, Vehicle_Passengers { Random_Defender { gunman, weapon } x 3 } },
                        Track Vehicle_track { AI_track_point x 8 } }"""
    src, q = pf['d'], pf['p']
    like = d.created_ids()[0]
    new_creates, new_sets = [], []
    state = dict(v=0x5BF00000 + i * 0x100 + (like & 0xFFF) * 0x10000)

    def uid():
        state['v'] += 1
        return state['v']

    def copy(nodes, m, parent_of):
        for x in nodes:
            new_creates.append(tf.Record(m[x], tf.CREATE, raw=q.create[x].raw))
            for r in q.sets.get(x, []):
                if r.prop == fe.P_PARENT:
                    raw = d._encode(fe.P_PARENT, parent_of(x))
                else:
                    raw = src._remap_value(r.prop, r.raw, m)
                if r.prop in (fe.P_UID0, fe.P_UID1) and struct.unpack('<i', raw[:4])[0] != 0:
                    raw = struct.pack('<i', uid())
                new_sets.append(tf.Record(m[x], tf.SET, r.prop, raw, r.hi))

    m = _RefMap()
    for x in pf['nodes']:
        m[x] = alloc(like)
    copy(pf['nodes'], m, lambda x: 0 if x == pf['root'] else m[q.parent[x]])     # 0: a top node of the block, as the folders beside it
    gunmen = [m[g] for g in pf['gunmen']]
    # the third gunman: a copy of the last Random_Defender folder
    last = q.parent[pf['gunmen'][-1]]
    sub = q.subtree(last)
    for _ in range(GUNMEN_PER_CAR - len(pf['gunmen'])):
        m2 = _RefMap()
        for x in sub:
            m2[x] = alloc(like)
        copy(sub, m2, lambda x: m[q.parent[x]] if x == last else m2[q.parent[x]])
        gunmen.append(m2[pf['gunmen'][-1]])
    # the track: the last point copied up to TRACK_POINTS
    tail = pf['points'][-1]
    points = [m[x] for x in pf['points']]
    for _ in range(TRACK_POINTS - len(points)):
        m3 = _RefMap({tail: alloc(like)})
        copy([tail], m3, lambda x: m[pf['track']])
        points.append(m3[tail])
    at = max(j for j, r in enumerate(d.records) if r.op == tf.CREATE) + 1
    d.records[at:at] = new_creates
    d.records.extend(new_sets)
    d.modified = True
    d._gen += 1
    fold, veh, track = m[pf['root']], m[pf['vehicle']], m[pf['track']]

    def put(node, **props):
        for prop, val in props.items():
            idx = d.T.index(prop)
            raw = d._encode(idx, val)
            hits = [r for r in new_sets if r.node == node and r.prop == idx]
            if hits:
                hits[-1].raw = raw
            else:
                rec = tf.Record(node, tf.SET, idx, raw)
                new_sets.append(rec)
                d.records.append(rec)

    put(fold, name='BFS%d' % i, flags=(src.get(pf['root'], 'flags') or 0) | 1)
    put(veh, name='BFV%d' % i, pos=[0.0, -500.0, 0.0])
    put(track, pos=[0.0, 0.0, 0.0], orient=[0.0, 0.0, 0.0, 1.0])
    for j, n in enumerate(points):
        put(n, name='P%d' % j, order=j + 1, pos=[0.0, -500.0, float(j)], orient=[0.0, 0.0, 0.0, 1.0], Stay_In_Vehicle=False, Pause_point=False)
    for j, n in enumerate(gunmen):
        put(n, name='BFG%d_%d' % (i, j), FactionID=9, StoredDamage=0.0, MissionObjectiveKill=False, DontHunt=False)
        if look is not None:
            mdl, tex, hp, use_data, sub_type = look
            put(n, Model=mdl, FallbackModel=mdl, CurrentModel=mdl, Real_Active_Texture_Set=tex, OrgHealth=hp, Use_AISubType_Data=use_data, AISubType=sub_type)


def mission_entries(fs):
    """Every shared mission block of the player's archive, in a stable order."""
    names = sorted(v[2] for v in fs.naz().table.values())
    return [n for n in names if '/shared/' in n and n.endswith('.mission')]
