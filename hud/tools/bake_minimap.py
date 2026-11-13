"""bake_minimap.py - the map of every TOD level, made from the player's own game files (read only).

The [minimap] feature of TODGameplay.dll draws these maps inside the game's round radar. Game data cannot be
redistributed, so the installer runs this script on the player's machine and writes the result next to TOD.exe:

  python -I bake_minimap.py --game "C:\\GOG Games\\Total Overdose" [--out DIR] [--maps map02,map03] [--force]

  --out      default <game>\\TODGameplay_minimap (minimap.txt, one tile pack + one .occ per map, bake.json)
  --force    bake even when bake.json says the sources and the style did not change

This script reads the solid collision of every placed model and collision box of the map block and all its submaps, and
the names of the models for tree symbols and clutter (minimap_geo.py). The pixel work is done by dist\\TODMinimapBake.exe
(hud/bake/minimap_bake.cpp: all the drawing rules and the pack format are there).

minimap.txt (read by the DLL, dll/src/gp_hud_logic.cpp parse_map_line): one line per map
  <name> <x0> <z0> <x1> <z1> <width> <height> <file>

A map stays listed until its replacement is finished: when a new style's bake fails, the player keeps the old map.

Python 3 standard library only. Nothing is written outside --out and the temp folder; the game folder is only read.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import minimap_geo as mg                 # noqa: E402
from gameplay.gamefs import GameFS, DEFAULT_ARCHIVES  # noqa: E402  (minimap_geo put data/ on the path)

VERSION = 3
TEXEL = 0.25                             # metres per texel of the finest level (TEXEL0 in minimap_bake.cpp)
BAKER = 'TODMinimapBake.exe'


class MapFS(GameFS):
    """GameFS for the bake only: a minimap is worth having from whatever can be read. An archive that is broken or locked
    counts as missing instead of ending the bake, and the game's blocks are found even when a configpc.txt filesystem
    line leaves them out. (The data builder keeps the strict GameFS: there a silent gap would change what the game loads.)"""
    def __init__(self, game, skip=()):
        GameFS.__init__(self, game, skip)
        have = {n.lower() for n in self.order} | {s.lower() for s in self.skipped}
        self.order += [n for n in DEFAULT_ARCHIVES if n.lower() not in have]

    def archive(self, name):
        try:
            return GameFS.archive(self, name)
        except Exception as e:                # noqa: BLE001
            print('%s cannot be read (%s): left out' % (name, e))
            self._arch[name] = None
            return None


def baker_path():
    """the native baker: next to the packed installer's data, or the build's dist folder"""
    roots = [getattr(sys, '_MEIPASS', None), os.path.normpath(os.path.join(HERE, '..', '..'))]
    for r in roots:
        if r and os.path.isfile(os.path.join(r, 'dist', BAKER)):
            return os.path.join(r, 'dist', BAKER)
    return None


def write_input(path, geo):
    """the baker's input: "MMB1", triangles (9 floats + material), marks (x, z, radius, kind)"""
    with open(path, 'wb') as f:
        f.write(b'MMB1' + struct.pack('<II', len(geo.tris), len(geo.marks)))
        pack = struct.Struct('<10f').pack
        f.writelines(pack(*t) for t in geo.tris)
        pack = struct.Struct('<4f').pack
        f.writelines(pack(*m) for m in geo.marks)


def run_baker(exe, mmb, pack, occ):
    """-> dict of the baker's result line; raises RuntimeError with its own message"""
    p = subprocess.run([exe, mmb, pack, occ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=0x08000000,
                       text=True, encoding='utf-8', errors='replace')
    last = (p.stdout or '').strip().splitlines()
    if p.returncode or not last:
        raise RuntimeError('the baker failed (exit %s): %s' % (p.returncode, ' '.join(last)[-200:]))
    v = last[-1].split()
    return dict(x0=float(v[0]), z0=float(v[1]), x1=float(v[2]), z1=float(v[3]), w=int(v[4]), h=int(v[5]),
                tiles=int(v[6]) + int(v[7]), bytes=int(v[8]))


# ---------------------------------------------------------------- driver
def source_key(fs, entries):
    h = hashlib.sha256()
    for e in entries + [mg.MAIN[len(mg.SHARED):]]:
        try:
            n, path = fs.locate(mg.SHARED + e)
        except KeyError:
            continue
        a = fs.archive(n); off, size, name = a.find('/' + mg.SHARED + e)
        st = os.stat(path)
        h.update(('%s|%s|%d|%d|%d;' % (e, n, off, size, int(st.st_mtime))).encode())
    h.update(('v%d' % VERSION).encode())
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--game', required=True)
    ap.add_argument('--out')
    ap.add_argument('--maps', default='')
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args(argv)
    game = os.path.abspath(a.game)
    out = os.path.abspath(a.out or os.path.join(game, 'TODGameplay_minimap'))
    for bad in ('tod hd', 'todbridge'):
        if bad in out.lower() and 'todgameplay_minimap' not in out.lower():
            print('refusing to write into %s' % out); return 2
    exe = baker_path()
    if not exe:
        print('%s is missing (dist folder): build it with dll\\build.bat' % BAKER); return 3
    os.makedirs(out, exist_ok=True)
    fs = MapFS(game, skip=['todgameplay_data.zip'])
    maps = mg.list_maps(fs)
    want = [m for m in a.maps.split(',') if m]
    keys = [k for k in maps if not want or any(k.startswith(w) for w in want)]
    state_path = os.path.join(out, 'bake.json')
    try:
        with open(state_path) as f:
            state = json.load(f)
    except (OSError, ValueError):
        state = {}
    if not isinstance(state.get('maps'), dict):
        state = {'maps': {}}
    # an older bake's entries stay until each map is replaced: their source key differs, so all of them bake again
    state['version'] = VERSION
    t_all = time.time()
    for k in keys:
        entries = maps[k]
        sk = source_key(fs, entries)
        old = state['maps'].get(k)
        if old and old.get('source') == sk and os.path.exists(os.path.join(out, old['file'])) and not a.force:
            print('%s: up to date' % k); continue
        t0 = time.time()
        print('%s: reading collision' % k)
        geo = mg.MapGeo(fs, entries, log=print)
        if len(geo.tris) < 100:
            print('%s: too little collision, skipped' % k); continue
        t1 = time.time()
        fd, mmb = tempfile.mkstemp(suffix='.mmb')
        os.close(fd)
        fn = '%s.mmp' % k
        try:
            write_input(mmb, geo)
            r = run_baker(exe, mmb, os.path.join(out, fn), os.path.join(out, '%s.occ' % k))
        except (OSError, RuntimeError) as e:
            print('%s: %s, skipped' % (k, e)); continue
        finally:
            os.remove(mmb)
        if old and old.get('file') not in (None, fn):
            try:
                os.remove(os.path.join(out, old['file']))
            except OSError:
                pass
        state['maps'][k] = dict(file=fn, source=sk, x0=r['x0'], z0=r['z0'], x1=r['x1'], z1=r['z1'], w=r['w'], h=r['h'], res=TEXEL,
                                tris=len(geo.tris), tiles=r['tiles'], bytes=r['bytes'], seconds=round(time.time() - t0, 1))
        print('%s: %dx%d px at %.2f m/px, %d tiles, %.1f MB (read %.0f s, draw %.0f s)' % (
            k, r['w'], r['h'], TEXEL, r['tiles'], r['bytes'] / (1 << 20), t1 - t0, time.time() - t1))
        with open(state_path + '.tmp', 'w') as f:
            json.dump(state, f, indent=1)
        os.replace(state_path + '.tmp', state_path)
    lines = ['# TODGameplay minimap index (hud/tools/bake_minimap.py): name x0 z0 x1 z1 width height file']
    for k, m in sorted(state['maps'].items()):
        if os.path.exists(os.path.join(out, m['file'])):
            lines.append('%s %.2f %.2f %.2f %.2f %d %d %s' % (k, m['x0'], m['z0'], m['x1'], m['z1'], m['w'], m['h'], m['file']))
    with open(os.path.join(out, 'minimap.txt.tmp'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(os.path.join(out, 'minimap.txt.tmp'), os.path.join(out, 'minimap.txt'))
    print('done: %d map(s) listed in %s (%.0f s)' % (len(lines) - 1, os.path.join(out, 'minimap.txt'), time.time() - t_all))
    return 0


if __name__ == '__main__':
    sys.exit(main())
