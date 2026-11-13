r"""Backups of the game folder (full or only the files the installer owns), manifests with sha256, verify, restore.

Layout of one backup:
  <root>\Gameplay+ backup 2026-10-28 171500 (full)\
      manifest.json      kind, game_dir, created, files {rel: {sha256, size}}, absent [owned files not in the game]
      files\<rel>        the copies
      install.log        the log of the install that made it (restore.log for restores)
      eidos.reg          the game's registry key, when the install changes the display size
Writing into the game folder always goes to a temp file next to the target, then os.replace: a hard-linked file is
never written into (its link is replaced).
"""
import datetime
import hashlib
import json
import os
import shutil

from . import plan as P

CHUNK = 1 << 22
TMP_SUFFIX = '.gpi_tmp'


class BackupError(Exception):
    pass


def sha_file(path, progress=None):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(CHUNK), b''):
            h.update(c)
            if progress:
                progress(len(c))
    return h.hexdigest()


def copy_hashed(src, dst, progress=None):
    """Copy src -> dst (dst written as dst.gpi_tmp, then renamed), returns the sha256 of what was read."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    h = hashlib.sha256()
    tmp = dst + TMP_SUFFIX
    with open(src, 'rb') as fi, open(tmp, 'wb') as fo:
        for c in iter(lambda: fi.read(CHUNK), b''):
            h.update(c)
            fo.write(c)
            if progress:
                progress(len(c))
    shutil.copystat(src, tmp)
    os.replace(tmp, dst)
    return h.hexdigest()


def replace_file(src, dst, progress=None):
    """Put a copy of src at dst without ever writing into an existing dst (hard-link safe)."""
    return copy_hashed(src, dst, progress)


def write_bytes(dst, data):
    tmp = dst + TMP_SUFFIX
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, dst)


def minimap_files(game_dir):
    """The files of <game>\\TODGameplay_minimap that hud\\tools\\bake_minimap.py makes and that exist now (rel paths with
    '/'): minimap.txt, bake.json, each baked map's picture and .occ (named by bake.json and minimap.txt), and the
    tool's own .tmp leftovers. Anything else in that folder is not the installer's and is never removed."""
    d = os.path.join(game_dir, P.MINIMAP_DIR)
    if not os.path.isdir(d):
        return []
    names = {'minimap.txt', 'bake.json'}
    try:
        with open(os.path.join(d, 'bake.json'), encoding='utf-8') as f:
            for k, m in ((json.load(f) or {}).get('maps') or {}).items():
                names.add(os.path.basename(str(m.get('file') or '')))
                names.add(os.path.basename('%s.occ' % k))
    except (OSError, ValueError, AttributeError):
        pass
    try:
        with open(os.path.join(d, 'minimap.txt'), encoding='utf-8', errors='replace') as f:
            for ln in f:
                parts = ln.split()
                if len(parts) == 8 and not ln.startswith('#'):
                    names.add(os.path.basename(parts[7]))
                    names.add(os.path.basename(parts[0] + '.occ'))
    except OSError:
        pass
    names = {n for n in names if n and n not in ('.', '..')}
    names |= {n + '.tmp' for n in names}
    return sorted(P.MINIMAP_DIR + '/' + n for n in os.listdir(d)
                  if n in names and os.path.isfile(os.path.join(d, n)))


def backup_files(game_dir, kind):
    """[(rel, size)] the backup will hold. A "changed files only" backup holds the installer-owned files and every file
    of the minimap folder."""
    if kind == 'full':
        out = []
        for root, dirs, files in os.walk(game_dir):
            for f in files:
                p = os.path.join(root, f)
                if f.endswith(TMP_SUFFIX):
                    continue
                out.append((os.path.relpath(p, game_dir), os.path.getsize(p)))
        return sorted(out)
    out = [(n, os.path.getsize(os.path.join(game_dir, n))) for n in P.OWNED_FILES
           if os.path.isfile(os.path.join(game_dir, n))]
    mm = os.path.join(game_dir, P.MINIMAP_DIR)
    if os.path.isdir(mm):
        for root, dirs, files in os.walk(mm):
            for f in files:
                p = os.path.join(root, f)
                if not f.endswith(TMP_SUFFIX):
                    out.append((os.path.relpath(p, game_dir), os.path.getsize(p)))
    return out


def new_backup_dir(root, kind, now=None):
    now = now or datetime.datetime.now()
    name = 'Gameplay+ backup %s (%s)' % (now.strftime('%Y-%m-%d %H%M%S'), 'full' if kind == 'full' else 'changed files')
    d = os.path.join(root, name)
    k = 2
    while os.path.exists(d):
        d = os.path.join(root, '%s %d' % (name, k))
        k += 1
    return d


def make_backup(game_dir, root, kind, log, progress=None, extra=None):
    """Copy, hash while copying, write the manifest, then verify every copy by reading it back. Returns the folder."""
    files = backup_files(game_dir, kind)
    total = sum(s for _, s in files)
    d = new_backup_dir(root, kind)
    os.makedirs(os.path.join(d, 'files'))
    log('backup: %s, %d files, %d bytes -> %s' % (kind, len(files), total, d))
    done = [0]

    def prog(n):
        done[0] += n
        if progress:
            progress(done[0], total * 2)

    man = dict(tool='Gameplay+ installer', kind=kind, game_dir=os.path.abspath(game_dir),
               created=datetime.datetime.now().isoformat(timespec='seconds'), files={}, absent=[],
               bytes=total)
    if extra:
        man.update(extra)
    for rel, size in files:
        src = os.path.join(game_dir, rel)
        man['files'][rel.replace('\\', '/')] = dict(sha256=copy_hashed(src, os.path.join(d, 'files', rel), prog),
                                                     size=size)
    man['absent'] = [n for n in P.OWNED_FILES if not os.path.exists(os.path.join(game_dir, n))]
    with open(os.path.join(d, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(man, f, indent=1)
    bad = verify_backup(d, man, prog)
    if bad:
        raise BackupError('backup verification failed for %d file(s): %s' % (len(bad), ', '.join(bad[:5])))
    log('backup verified: %d files read back, every sha256 matches' % len(files))
    return d, man


def backup_side_file(d, path, log):
    """Copy the display side file (<Documents>\\...\\savegames\\TODGameplay_display.txt) into the backup folder d (as
    side\\TODGameplay_display.txt), read it back, and record it in the manifest. Returns the manifest entry."""
    if os.path.basename(path).lower() != P.DISPLAY_SIDE.lower():
        raise BackupError('not the display side file: %s' % path)
    dst = os.path.join(d, 'side', P.DISPLAY_SIDE)
    h = copy_hashed(path, dst)
    if sha_file(dst) != h:
        raise BackupError('the backup copy of %s does not read back' % path)
    man = load_manifest(d)
    ent = dict(name=P.DISPLAY_SIDE, path=os.path.abspath(path), sha256=h, size=os.path.getsize(dst))
    man.setdefault('side_files', []).append(ent)
    with open(os.path.join(d, 'manifest.json.tmp'), 'w', encoding='utf-8') as f:
        json.dump(man, f, indent=1)
    os.replace(os.path.join(d, 'manifest.json.tmp'), os.path.join(d, 'manifest.json'))
    log('backup: %s copied into the backup (side\\%s), verified' % (path, P.DISPLAY_SIDE))
    return ent


def load_manifest(d):
    with open(os.path.join(d, 'manifest.json'), encoding='utf-8') as f:
        return json.load(f)


def verify_backup(d, man=None, progress=None):
    """Names of files whose copy is missing or differs from the manifest ([] = good)."""
    man = man or load_manifest(d)
    bad = []
    for rel, info in man['files'].items():
        p = os.path.join(d, 'files', *rel.split('/'))
        if not os.path.isfile(p) or os.path.getsize(p) != info['size'] or sha_file(p, progress) != info['sha256']:
            bad.append(rel)
    return bad


def list_backups(root):
    out = []
    if not root or not os.path.isdir(root):
        return out
    for n in sorted(os.listdir(root), reverse=True):
        d = os.path.join(root, n)
        if not os.path.isfile(os.path.join(d, 'manifest.json')):
            continue
        try:
            m = load_manifest(d)
        except (OSError, ValueError):
            continue
        out.append(dict(path=d, name=n, kind=m.get('kind'), created=m.get('created'), game_dir=m.get('game_dir'),
                        files=len(m.get('files', {})), bytes=m.get('bytes', 0), selection=m.get('selection_before'),
                        note=m.get('note')))
    return out


def restore(d, game_dir, log, progress=None, registry_import=None, side_target=None):
    """Verify the backup, copy back every file that differs, delete installer-owned files the backup says were
    absent. Returns a result dict. Nothing in the game folder is touched when the verification fails.
    side_target: where the display side file lives on this PC (None = never touched). A side file the install
    deleted is put back only when there is none now (a mode the player kept in game since is never overwritten)."""
    man = load_manifest(d)
    total = sum(i['size'] for i in man['files'].values())
    done = [0]

    def prog(n):
        done[0] += n
        if progress:
            progress(done[0], total * 3)

    log('restore from %s (%s, %d files)' % (d, man.get('kind'), len(man['files'])))
    bad = verify_backup(d, man, prog)
    if bad:
        raise BackupError('the backup does not match its manifest (%d file(s), e.g. %s): nothing restored'
                          % (len(bad), ', '.join(bad[:3])))
    log('backup verified against its manifest')
    copied, same = [], []
    for rel, info in sorted(man['files'].items()):
        dst = os.path.join(game_dir, *rel.split('/'))
        if os.path.isfile(dst) and os.path.getsize(dst) == info['size'] and sha_file(dst, prog) == info['sha256']:
            same.append(rel)
            continue
        src = os.path.join(d, 'files', *rel.split('/'))
        replace_file(src, dst, prog)
        copied.append(rel)
        log('restored %s' % rel)
    removed = []
    kept = set(k.lower() for k in man['files'])
    absent = set(n.lower() for n in man.get('absent', []))
    for n in P.OWNED_FILES:
        p = os.path.join(game_dir, n)
        if n.lower() in kept or not os.path.isfile(p):
            continue
        if man.get('kind') == 'full' or n.lower() in absent:
            os.remove(p)
            removed.append(n)
            log('removed %s (not in the backup: the installer added it)' % n)
    # the minimap folder: both backup kinds hold every file of it, so a file the bake made since is removed
    for rel in minimap_files(game_dir):
        if rel.lower() not in kept:
            os.remove(os.path.join(game_dir, *rel.split('/')))
            removed.append(rel)
            log('removed %s (not in the backup: the installer added it)' % rel)
    remove_empty_dirs(game_dir, P.HDFX_FILES + [P.MINIMAP_DIR + '/x'])
    for root, dirs, files in os.walk(game_dir):             # leftovers of an interrupted write
        for f in files:
            if f.endswith(TMP_SUFFIX):
                os.remove(os.path.join(root, f))
    bad_after = []
    for rel, info in man['files'].items():
        dst = os.path.join(game_dir, *rel.split('/'))
        if rel in copied and sha_file(dst) != info['sha256']:
            bad_after.append(rel)
    if bad_after:
        raise BackupError('restored files differ from the backup: %s' % ', '.join(bad_after[:5]))
    reg = None
    regfile = os.path.join(d, 'eidos.reg')
    if os.path.isfile(regfile) and registry_import:
        reg = registry_import(regfile)
        log('registry: %s' % reg)
    side = None
    for ent in man.get('side_files') or []:
        src = os.path.join(d, 'side', P.DISPLAY_SIDE)
        if ent.get('name') != P.DISPLAY_SIDE or not os.path.isfile(src) or sha_file(src) != ent.get('sha256'):
            continue
        if not side_target:
            side = 'kept in the backup (test mode: the Documents folder is not touched)'
        elif os.path.basename(side_target).lower() != P.DISPLAY_SIDE.lower():
            side = 'not restored (unexpected target %s)' % side_target
        elif os.path.exists(side_target):
            side = 'left as it is (the game kept a new mode since the install)'
        else:
            os.makedirs(os.path.dirname(side_target), exist_ok=True)
            replace_file(src, side_target)
            side = 'put back'
        log('display side file %s: %s' % (side_target or P.DISPLAY_SIDE, side))
    log('restore done: %d copied back, %d already identical, %d removed' % (len(copied), len(same), len(removed)))
    return dict(copied=copied, identical=len(same), removed=removed, registry=reg, kind=man.get('kind'),
                backup=d, side_file=side)


def remove_empty_dirs(game_dir, rels):
    """Remove the (now empty) sub-folders the owned files `rels` live in, deepest first; never the game folder."""
    dirs = set()
    for r in rels:
        parts = r.replace('\\', '/').split('/')[:-1]
        for i in range(len(parts), 0, -1):
            dirs.add('/'.join(parts[:i]))
    for d in sorted(dirs, key=lambda x: -x.count('/')):
        p = os.path.join(game_dir, *d.split('/'))
        if os.path.isdir(p) and not os.listdir(p):
            os.rmdir(p)


def folder_digest(game_dir):
    """{rel: sha256} of every file (for tests and the final comparison)."""
    out = {}
    for root, dirs, files in os.walk(game_dir):
        for f in files:
            p = os.path.join(root, f)
            out[os.path.relpath(p, game_dir).replace('\\', '/')] = sha_file(p)
    return out
