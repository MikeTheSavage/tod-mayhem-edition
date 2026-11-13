"""make_prefetch_table.py - TODGameplay_seamless.txt for the DLL's [seamless] prefetch (read only on the game).

Every walk / teleport loader volume of the connected groups (city, jungle, farm; the 15 submap blocks and the Map01 map
block that hold loaders) with its centre (x, z), a radius (half the horizontal diagonal of the box, or the sphere
radius) and the shared blocks it loads; for each block the archive the game reads it from (the configpc `filesystem =`
order with ZIP first, i.e. as installed), its byte offset and size, and each archive's file size (the DLL skips an
archive whose size changed, e.g. after a rebuild of the zip with other options).

  python -I make_prefetch_table.py GAME_DIR ZIP OUT.txt
The zip is the todgameplay_data.zip that will be installed (its seamless boxes are the ones the game will use).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import catalog_rerun as cr                 # noqa: E402  (Reader, LOADER_BLOCKS, data/ on sys.path)
import todblock as tb                      # noqa: E402
import fragtool as ft                      # noqa: E402
import fragedit as fe                      # noqa: E402
from gameplay import seamless as sm        # noqa: E402

SHARED = sm.SHARED


def target_entry(folder):
    f = folder.lower()
    if f.startswith('mainmap_'):
        return SHARED + f + '.map'
    if f.startswith('submap'):
        return SHARED + f + '.submap'
    return None


def main(game_dir, zip_path, out):
    zname = os.path.basename(zip_path)
    archives = [tb.Archive(zip_path), tb.Archive(os.path.join(game_dir, 'blocks.naz'))]
    names = [zname, 'blocks.naz']
    sizes = [os.path.getsize(zip_path), os.path.getsize(os.path.join(game_dir, 'blocks.naz'))]
    T = ft.types()
    blocks, loaders = {}, []

    def locate(entry):
        for k, a in enumerate(archives):
            hit = a.find('/' + entry)
            if hit is not None and hit[2].lower() == entry.lower():
                return k, hit[0], hit[1]
        return None

    def read(entry):
        k, off, size = locate(entry)
        with open(archives[k].path, 'rb') as f:
            f.seek(off)
            return f.read(size)

    for blk in cr.LOADER_BLOCKS:
        if blk.endswith('.mission'):
            continue
        e = SHARED + blk
        b = tb.Block(read(e), os.path.splitext(e)[1])
        frs, _ = ft.fragments(b)
        for v in sm.scan_doc_volumes(e, b, frs, T, fe, ft):
            ents = []
            for a in v['actions']:
                te = target_entry(a['target'])
                if not te:
                    continue
                if te not in blocks:
                    loc = locate(te)
                    if loc is None:
                        continue
                    blocks[te] = loc
                if te not in ents:
                    ents.append(te)
            if not ents:
                continue
            if v.get('radius'):
                r = float(v['radius'])
            else:
                w, _, l = [float(x or 0) for x in v['size']]
                r = 0.5 * (w * w + l * l) ** 0.5
            loaders.append((v['pos'][0], v['pos'][2], r, ents[:4], blk, v['name']))
    used = sorted({k for k, _, _ in blocks.values()})
    lines = ['# TODGameplay_seamless.txt - prefetch table for TODGameplay.dll [seamless] (seamless/tools/make_prefetch_table.py)',
             '# made for %s with %s first; %d loader volumes, %d blocks' % (game_dir, zname, len(loaders), len(blocks))]
    for k in used:
        lines.append('archive %s %d' % (names[k], sizes[k]))
    for e, (k, off, size) in sorted(blocks.items()):
        lines.append('block %s %s %d %d' % (e, names[k], off, size))
    for x, z, r, ents, blk, name in loaders:
        lines.append('loader %.2f %.2f %.1f %s' % (x, z, r, '|'.join(ents)))
    with open(out, 'w', encoding='ascii', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d loader volumes, %d blocks (%s)' % (out, len(loaders), len(blocks),
                                                     ', '.join('%s %d' % (names[k], sum(1 for v in blocks.values() if v[0] == k)) for k in used)))
    return 0


if __name__ == '__main__':
    a = sys.argv[1:]
    sys.exit(main(a[0], a[1], a[2]))
