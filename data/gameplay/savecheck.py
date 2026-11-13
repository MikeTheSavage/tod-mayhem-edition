"""Which edited values also live in the player's saves (read-only, works on copies or the real folder).

A save stores every property that changed since its block was loaded, addressed by (block, creation index), and the
load replays those records over the freshly loaded blocks. So:
  - create order: the builder already proves that no create record moved (node indices stay valid);
  - a value edited by the mod that a save ALSO stores (because the game wrote it at run time) is restored from the
    save on load, i.e. the edit does not show on that save until the game rewrites it.
This module finds the second case by name: save nodes are named by the save editor's node map (vendored from
tools/save_editor), matched on fragment + entity name + property name.
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.join(os.path.dirname(HERE), 'lib', 'save_editor')


def _load_savefile(game_dir):
    os.environ['TOD_GAME_DIR'] = os.path.abspath(game_dir)
    if SE not in sys.path:
        sys.path.insert(0, SE)
    import savefile
    savefile.GAME_DIR = os.path.abspath(game_dir)
    return savefile


def check(saves_dir, game_dir, changes):
    savefile = _load_savefile(game_dir)
    slots = sorted(p for p in glob.glob(os.path.join(saves_dir, 'Slot*')) if os.path.basename(p) != 'Slot11')
    want = {}
    for c in changes:
        name = c['path'].split('/')[-1]
        fr = c['fragment'].split('/')[-1].lower()
        want.setdefault((fr, name.lower(), c['prop'].lower()), set()).add('%s.%s (%s)' % (
            c['path'], c['prop'], c['fragment'].split('/')[-1]))
        if name == 'CameraPos' and c['prop'] == 'pos':
            # camera_system copies CameraPos into its own members (initialize_local 0x4CE320) and writes them every
            # frame; the rig's target is end_pos, so a saved end_pos/current_pos overrides the new data too
            rig = '/'.join(c['path'].split('/')[:-1])
            for m in ('end_pos', 'current_pos'):
                want.setdefault((fr, 'cameractrl', m), set()).add('%s.%s (%s, copy of CameraPos)' % (
                    rig, m, c['fragment'].split('/')[-1]))
    hits, decoded, failed = {}, [], []
    for p in slots:
        try:
            with open(p, 'rb') as f:
                j = savefile.decode(f.read())
        except Exception as e:  # a slot the editor cannot read is reported, never fatal
            failed.append('%s: %s' % (os.path.basename(p), e))
            continue
        decoded.append(os.path.basename(p))
        for r in j['records']:
            if 'n' not in r or not r.get('name'):
                continue
            node = j['nodes'].get(r['n'], {})
            key = ((node.get('fragment') or '').lower(), (node.get('name') or '').lower(), r['name'].lower())
            if key in want:
                for w in want[key]:
                    hits.setdefault(w, set()).add('%s (%s)' % (os.path.basename(p), r['n']))
    out = dict(saves=decoded, unreadable=failed,
               hits=[dict(what=w, saves=sorted(s)) for w, s in sorted(hits.items())])
    if hits:
        out['summary'] = ('%d edited values are also stored in saves: loading such a save restores the stored value '
                          'until the game writes it again (see the list).' % len(hits))
    else:
        out['summary'] = 'No edited value is stored in any save: every edit applies to existing saves.'
    return out
