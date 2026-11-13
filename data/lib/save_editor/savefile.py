"""Total Overdose (PC/GOG, Kapow engine build 1925) save slot <-> JSON.

File layout (every SlotNN under %USERPROFILE%/Documents/Total Overdose/savegames/, 0x50000 bytes):
  0x00 u32 version            = 9
  0x04 u32 property-list CRC  = 0x65C37710 (Kapow CRC over [i32 id][name:type] of all 5994 global properties)
  0x08 u32 engine build       = 1925
  0x0C u32 header CRC         = Kapow CRC over bytes [0x14, 0x14 + header_size)
  0x10 u32 header_size        = bytes covered by the header CRC (0x48 with the usual 28-byte summary)
  0x14 u32 buffers            = 6
  0x18 u32 field_18           (uninitialised stack word in the game; preserved verbatim)
  0x1C u32 summary_size       (bytes)
  0x20 ...  summary           = [count] + count x ([size][value...]) of the MemorySummaryNode's script properties
  then  6 x i32 loaded-block folder ids (block-1 entity ids, -1 = none): main, map, submap, mission, cutscene, playerdata
        u32 saved game time (ms), u32 saved frame number
  then  u32 payload_dwords (uncompressed size / 4), u32 compressed_dwords (floor(compressed bytes / 4))
  then  zlib stream (level 6) of the payload; zero padding; u32 file CRC at 0x4FFFC = Kapow CRC over [0, 0x4FFFC).
Payload: the engine's per-node property stream (kstream.py); node indices = creation order (nodemap.py).
"""
import struct, zlib, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcrc, ktypes, kstream, kmodel, nodemap, kfrag
import todblock as tb

FILE_SIZE = 0x50000
GAME_DIR = os.environ.get('TOD_GAME_DIR', r'C:\GOG Games\Total Overdose')
MAIN_BLOCK = 'data_pc/overdose_the_game/overdose/shared/overdose.main'
MAIN_SCENE = '/data/Overdose_THE_GAME/Overdose.scene'
BLOCK_SLOTS = ['main', 'map', 'submap', 'mission', 'cutscene', 'playerdata']


def u32(b, o): return struct.unpack_from('<I', b, o)[0]


# ---------------------------------------------------------------- container
def parse_container(b):
    if len(b) != FILE_SIZE:
        raise ValueError('not a game save slot (size %d != 0x50000)' % len(b))
    h = {}
    h['version'], h['property_crc'], h['engine_build'], h['header_crc'], h['header_size'] = struct.unpack_from('<5I', b, 0)
    if h['version'] != 9:
        raise ValueError('version %d' % h['version'])
    o = 0x14
    h['buffers'], h['field_18'], h['summary_size'] = struct.unpack_from('<3I', b, o); o += 12
    summ = b[o:o + h['summary_size']]; o += h['summary_size']
    h['folders'] = list(struct.unpack_from('<6i', b, o)); o += 24
    h['game_time_ms'], h['frame'] = struct.unpack_from('<2I', b, o); o += 8
    if o != 0x14 + h['header_size']:
        raise ValueError('header size mismatch')
    h['payload_dwords'], h['compressed_dwords'] = struct.unpack_from('<2I', b, o); o += 8
    d = zlib.decompressobj()
    payload = d.decompress(b[o:])
    clen = len(b) - o - len(d.unused_data)
    if not d.eof:
        raise ValueError('zlib stream truncated')
    if len(payload) != h['payload_dwords'] * 4:
        raise ValueError('payload size mismatch')
    tail = b[o + clen:FILE_SIZE - 4]
    h['file_crc'] = u32(b, FILE_SIZE - 4)
    checks = {
        'header_crc_ok': kcrc.crc(b[0x14:0x14 + h['header_size']]) == h['header_crc'],
        'file_crc_ok': kcrc.crc(b[:FILE_SIZE - 4]) == h['file_crc'],
        'compressed_dwords_ok': clen // 4 == h['compressed_dwords'],
        'zero_padding_ok': not tail.strip(b'\0'),
    }
    sw = struct.unpack('<%dI' % (len(summ) // 4), summ)
    summary = {'count': sw[0], 'items': []}
    k = 1
    for _ in range(sw[0]):
        n = sw[k]; summary['items'].append(list(sw[k + 1:k + 1 + n])); k += 1 + n
    if k != len(sw):
        raise ValueError('summary not consumed')
    return h, summary, payload, checks


def build_container(h, summary, payload):
    sw = [summary['count']]
    for it in summary['items']:
        sw += [len(it)] + [int(x) & 0xFFFFFFFF for x in it]
    summ = struct.pack('<%dI' % len(sw), *sw)
    body = struct.pack('<3I', h['buffers'], h['field_18'], len(summ)) + summ
    body += struct.pack('<6i', *h['folders']) + struct.pack('<2I', h['game_time_ms'], h['frame'])
    c = zlib.compressobj(6, zlib.DEFLATED, 15, 8)
    comp = c.compress(payload) + c.flush()
    hdr = struct.pack('<3I', 9, h['property_crc'], h['engine_build'])
    hdr += struct.pack('<2I', kcrc.crc(body), len(body))
    out = bytearray(hdr + body + struct.pack('<2I', len(payload) // 4, len(comp) // 4) + comp)
    if len(out) > FILE_SIZE - 4:
        raise ValueError('save too large: %d bytes' % len(out))
    out += b'\0' * (FILE_SIZE - 4 - len(out))
    out += struct.pack('<I', kcrc.crc(bytes(out)))
    return bytes(out)


# ---------------------------------------------------------------- node maps
_ARCH = None
_BLOCKS = {}


def arch():
    global _ARCH
    if _ARCH is None:
        _ARCH = tb.open_naz(GAME_DIR)
    return _ARCH


def block_frags(name):
    if name not in _BLOCKS:
        b = tb.load_block(arch(), name)
        st = tb.record_starts(b.info, b.n)
        fr = {}
        for i, R in enumerate(st):
            nm = tb.record_name(b.info, R)
            if nm.lower().endswith('.fragment') or nm.lower().endswith('.scene'):
                fr[nm] = b.payloads[i]
        _BLOCKS[name] = fr
    return _BLOCKS[name]


def block_file_for(fragpath):
    base = fragpath.replace('\\', '/').split('/')[-1].lower()
    base = base[:-9] if base.endswith('.fragment') else base
    if base.startswith('mainmap'):
        ext = '.map'
    elif base.startswith('submap'):
        ext = '.submap'
    elif base.startswith('playerdata'):
        ext = '.playerdata'
    else:
        ext = '.mission'
    return 'data_pc/overdose_the_game/overdose/shared/' + base + ext


class World:
    """Node index -> (NodeType, info) for the blocks a save has loaded."""
    _main = None

    def __init__(self, folders):
        if World._main is None:
            mf = block_frags(MAIN_BLOCK)
            nodes = nodemap.build(mf, MAIN_SCENE, 5)
            World._main = (mf, {n[0]: n for n in nodes}, {})
        self.main_frags, self.by1, self._dec = World._main
        self.maps = {1: self.by1}
        self.blocks = {1: MAIN_BLOCK}
        self.types = {}
        for k, fid in enumerate(folders):
            if fid is None or fid <= 0 or k == 0:
                continue
            fp = self.folder_fragment(fid & 0xFFFFF)
            if not fp:
                continue
            bn = block_file_for(fp)
            try:
                bf = block_frags(bn)
            except KeyError:
                continue
            want = fp.replace('\\', '/').split('/')[-1].lower()
            roots = [x for x in bf if x.split('/')[-1].lower() == want]
            if not roots:
                continue
            nn = nodemap.build(bf, roots[0], 1, fallback=[self.main_frags])
            self.maps[k + 1] = {n[0]: n for n in nn}
            self.blocks[k + 1] = bn

    def folder_fragment(self, idx):
        n = self.by1.get(idx)
        if not n:
            return None
        nm = n[2]
        if nm not in self._dec:
            self._dec[nm] = kfrag.decode(self.main_frags[nm])
        for o in self._dec[nm]:
            if o[0] == 'set' and o[1] == n[3] and o[3] == 'fragment':
                return o[4] if isinstance(o[4], str) else (o[4] or {}).get('s')
        return None

    def folder_name(self, idx):
        n = self.by1.get(idx & 0xFFFFF)
        return n[4] if n else None

    def nodetype(self, blk, idx):
        key = (blk, idx)
        if key not in self.types:
            n = self.maps.get(blk, {}).get(idx)
            if n is not None:
                t = kmodel.NodeType(n[1])
            elif blk == 1 and idx == 3:
                t = kmodel.NodeType('Scene')   # engine-created scene root (prop 10 = timemultiplier)
            else:
                t = kmodel.NodeType('Node')    # runtime-created / engine-internal node: Node class props only
            self.types[key] = t
        return self.types[key]

    def info(self, blk, idx):
        n = self.maps.get(blk, {}).get(idx)
        if n is None:
            return {'type': 'Scene' if (blk, idx) == (1, 3) else '?', 'name': None}
        return {'type': n[1], 'name': n[4], 'fragment': n[2].split('/')[-1], 'path': n[5]}


# ---------------------------------------------------------------- decode / encode
def node_key(blk, idx):
    return '%d:%x' % (blk, idx)


def parse_key(k):
    a, b = k.split(':')
    return int(a), int(b, 16)


def decode(b, world=None, annotate=True):
    h, summary, payload, checks = parse_container(b)
    w = struct.unpack('<%dI' % (len(payload) // 4), payload)
    world = world or World(h['folders'])
    recs, probs = kstream.parse(w, world.nodetype)
    nodes, out = {}, []
    for r in recs:
        if r.kind == 'junk':
            out.append({'junk': ['%08x' % x for x in r.raw]})
            continue
        k = node_key(r.blk, r.idx)
        if annotate and k not in nodes:
            nodes[k] = world.info(r.blk, r.idx)
        if r.kind == 'thread':
            e = {'n': k, 'p': 0, 'thread': r.value}
            if r.value:
                e['raw'] = ['%08x' % x for x in r.raw]
        elif r.kind == 'raw':
            e = {'n': k, 'p': r.prop, 'raw': ['%08x' % x for x in r.raw]}
        else:
            e = {'n': k, 'p': r.prop, 'name': r.name, 't': r.type, 'v': r.value}
        out.append(e)
    folders = []
    for k, f in enumerate(h['folders']):
        folders.append({'slot': BLOCK_SLOTS[k], 'id': f, 'folder': world.folder_name(f) if f > 0 else None,
                        'block': world.blocks.get(k + 1)})
    hdr = dict(h)
    hdr['folders'] = folders
    for k in ('property_crc', 'header_crc', 'file_crc', 'field_18'):
        hdr[k] = '%08x' % hdr[k]
    return {'format': 'tod-save/1', 'header': hdr, 'checks': checks,
            'parse_problems': [list(p) for p in probs], 'summary': summary, 'nodes': nodes, 'records': out}


def encode_payload(records):
    words = []
    for e in records:
        if 'junk' in e:
            words += [int(x, 16) for x in e['junk']]
            continue
        blk, idx = parse_key(e['n'])
        p = int(e['p'])
        words.append((idx << 16) | ((blk - 1) << 12) | (p & 0xFFF))
        if 'thread' in e:
            words.append(int(e['thread']))
            if e['thread']:
                words += [int(x, 16) for x in e['raw']]
        elif 'raw' in e:
            words += [int(x, 16) for x in e['raw']]
        else:
            words += ktypes.write(ktypes.parse_type(e['t']), e['v'])
    return struct.pack('<%dI' % len(words), *words)


def encode(j):
    h = dict(j['header'])
    for k in ('property_crc', 'field_18'):
        h[k] = int(h[k], 16)
    h['folders'] = [f['id'] if isinstance(f, dict) else f for f in h['folders']]
    return build_container(h, j['summary'], encode_payload(j['records']))


def load(path):
    return decode(open(path, 'rb').read())
