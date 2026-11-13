"""The hud_textures data option: HD HUD icons written into overdose.main (pickup coins, ammo icons and the bullet tick,
the 28 weapon selector cards, the combo timer strip).

The art is made ahead of time by hud_textures/make_hud_textures.py (Pillow, developer side) into hud_textures/art/:
manifest.json plus one zlib-compressed A8R8G8B8 mip chain per texture. Here (stdlib only, on the player's machine)
each enabled manifest row replaces the texture of the same resource name in overdose.main: the record's size / level
fields are patched in place and its payload swapped (data/lib/todtexture.py, the TOD HD texture writer). Every other
record and payload stays byte-identical, nothing is added or moved, so saves are not affected.

Rules checked per texture before anything is written: payload size = the engine's own size for (RGBA32, w, h, full
chain), sha256 as in the manifest, at most 1,351,384 bytes, a power-of-two size with the record's aspect, the record is
not DXT1. A texture that breaks a rule (e.g. a TOD HD base whose record is already something else) is skipped and
reported; the others still go in.
"""
import hashlib
import json
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), 'lib')
if LIB not in sys.path:
    sys.path.insert(0, LIB)
import todblock as tb      # noqa: E402
import todtexture as tt    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
ART = os.path.join(ROOT, 'hud_textures', 'art')
MANIFEST = os.path.join(ART, 'manifest.json')
ENTRY = 'data_pc/overdose_the_game/overdose/shared/overdose.main'
FORMAT = 1


class HudTexturesError(Exception):
    pass


def load_manifest(path=MANIFEST):
    if not os.path.exists(path):
        raise HudTexturesError('no HUD art at %s: run hud_textures/make_hud_textures.py first' % path)
    with open(path, encoding='utf-8') as f:
        man = json.load(f)
    if man.get('format') != FORMAT or man.get('block') != ENTRY:
        raise HudTexturesError('%s: format %r / block %r not understood' % (path, man.get('format'), man.get('block')))
    names = [r['name'] for r in man['textures']]
    if len(set(names)) != len(names) or len({r['resource'].lower() for r in man['textures']}) != len(names):
        raise HudTexturesError('%s: a texture is listed twice' % path)
    return man


def read_payload(row, art=ART):
    with open(os.path.join(art, row['file']), 'rb') as f:
        data = zlib.decompress(f.read())
    if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
        raise HudTexturesError('%s: payload does not match the manifest (size or sha256)' % row['name'])
    return data


def apply(raw, man=None, art=ART, only=None):
    """overdose.main bytes -> (new bytes, report). only: optional set of texture names (tests)."""
    man = man or load_manifest()
    blk = tb.Block(raw, '.main')
    try:
        recs = tt.textures(blk)
    except tt.TextureError as e:
        raise HudTexturesError('overdose.main: %s' % e)
    reps, skipped, todo = [], [], {}
    for row in man['textures']:
        if not row.get('enabled', True) or (only is not None and row['name'] not in only):
            continue
        rec = recs.get(row['resource'].lower())
        if rec is None:
            skipped.append(dict(name=row['name'], why='no texture %s in this overdose.main' % row['resource']))
            continue
        if row['fmt'] != 'RGBA32':
            raise HudTexturesError('%s: only RGBA32 art is shipped' % row['name'])
        try:
            payload = read_payload(row, art)
            tt.check_replacement(rec, row['fmt'], row['w'], row['h'], row['levels'], payload)
        except (tt.TextureError, HudTexturesError, OSError, zlib.error) as e:
            skipped.append(dict(name=row['name'], why=str(e)))
            continue
        todo[rec['index']] = (row['fmt'], row['w'], row['h'], row['levels'], payload)
        reps.append(dict(name=row['name'], group=row['group'], resource=rec['name'], index=rec['index'],
                         before='%s %dx%d %d levels %d B' % (rec['fmt_name'], rec['w'], rec['h'], rec['levels'],
                                                             rec['engine_size']),
                         after='%s %dx%d %d levels %d B' % (row['fmt'], row['w'], row['h'], row['levels'], row['bytes']),
                         sha256=row['sha256']))
    if not todo:
        return raw, dict(textures=reps, skipped=skipped, bytes_before=len(raw), bytes_after=len(raw))
    try:
        out, r = tt.rewrite_block(raw, '.main', todo)
    except tt.TextureError as e:
        raise HudTexturesError('overdose.main: %s' % e)
    return out, dict(r, textures=reps, skipped=skipped)
