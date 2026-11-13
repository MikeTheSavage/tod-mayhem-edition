r"""Install / uninstall the TOD Gameplay+ data zip by editing a GIVEN configpc.txt (backup first, idempotent).

  python install_gameplay_mod.py install   --config "<game>\configpc.txt" [--zip <built todgameplay_data.zip>]
  python install_gameplay_mod.py uninstall --config "<game>\configpc.txt"
  python install_gameplay_mod.py status    --config "<game>\configpc.txt"

install:   puts todgameplay_data.zip FIRST on the `filesystem =` line (it must come before hd_override*.zip, whose
           overdose.main it replaces with a copy built FROM that block). Without a filesystem line it adds
           `filesystem = todgameplay_data.zip, blocks.naz, sounds.naz, videos00.naz, videos01.naz` plus a marker
           comment. With --zip the zip is copied next to configpc.txt (with its .manifest.json).
uninstall: removes the zip from the line; a line this tool added is removed again (the file then equals the
           backup), a line that existed before keeps every other archive in its order. The zip copied by install is
           deleted.
Both keep the file's line ends and refuse to touch a folder whose TOD.exe is running. The first install saves
configpc.txt.todgameplay.bak (never overwritten); running install twice changes nothing the second time.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gameplay.gamefs import DEFAULT_ARCHIVES, parse_filesystem, read_config_lines  # noqa: E402

ZIP = 'todgameplay_data.zip'
MARK = '# TOD Gameplay+ data options (todgameplay_data.zip); remove with install_gameplay_mod.py uninstall'
BAK = '.todgameplay.bak'


class InstallError(Exception):
    pass


def game_running():
    try:
        out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout.lower()
    except Exception:
        return False
    return '"tod.exe"' in out or '"totaloverdose.exe"' in out


def guard(folder):
    if os.path.exists(os.path.join(folder, 'TOD.exe')) and game_running():
        raise InstallError('TOD is running: not touching %s now' % folder)


def write_lines(path, lines, nl, final):
    text = nl.join(lines) + (nl if final else '')
    data = text.encode('latin-1')
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)


def install(config, zip_path=None, log=print):
    config = os.path.abspath(config)
    folder = os.path.dirname(config)
    guard(folder)
    if not os.path.exists(config):
        raise InstallError('%s does not exist' % config)
    bak = config + BAK
    if not os.path.exists(bak):
        shutil.copy2(config, bak)
        log('backup: %s' % bak)
    lines, nl, final = read_config_lines(config)
    idx, names = parse_filesystem(lines)
    if idx is None:
        new = lines + [MARK, 'filesystem = ' + ', '.join([ZIP] + DEFAULT_ARCHIVES)]
    else:
        rest = [n for n in names if n.lower() != ZIP]
        new = list(lines)
        new[idx] = 'filesystem = ' + ', '.join([ZIP] + rest)
    changed = new != lines
    if changed:
        write_lines(config, new, nl, True if idx is None else final)
        log('configpc.txt: %s first on the filesystem line' % ZIP)
    else:
        log('configpc.txt: already installed, unchanged')
    if zip_path:
        dst = os.path.join(folder, ZIP)
        src = os.path.abspath(zip_path)
        if os.path.normcase(src) != os.path.normcase(dst):
            if not os.path.exists(dst) or sha_file(dst) != sha_file(src):
                shutil.copyfile(src, dst + '.part')
                os.replace(dst + '.part', dst)
                log('copied %s -> %s' % (src, dst))
            man = dict(zip=ZIP, sha256=sha_file(dst), bytes=os.path.getsize(dst), source=src)
            rep = os.path.join(os.path.splitext(src)[0] + '.report', 'report.json')
            if os.path.exists(rep):
                with open(rep, encoding='utf-8') as f:
                    r = json.load(f)
                man.update(selection=r.get('selection'), blocks=[dict(entry=b['entry'], base=b['base'],
                           base_sha256=b['base_sha256']) for b in r.get('blocks', [])])
            with open(os.path.join(folder, ZIP + '.manifest.json'), 'w', encoding='utf-8') as f:
                json.dump(man, f, indent=1)
    return changed


def uninstall(config, log=print):
    config = os.path.abspath(config)
    folder = os.path.dirname(config)
    guard(folder)
    lines, nl, final = read_config_lines(config)
    idx, names = parse_filesystem(lines)
    new = list(lines)
    if idx is not None and any(n.lower() == ZIP for n in names):
        rest = [n for n in names if n.lower() != ZIP]
        added_by_us = idx > 0 and lines[idx - 1] == MARK and rest == DEFAULT_ARCHIVES
        if added_by_us:
            del new[idx]
            del new[idx - 1]
        else:
            new[idx] = 'filesystem = ' + ', '.join(rest)
    elif MARK in new:
        new.remove(MARK)
    changed = new != lines
    if changed:
        bak = config + BAK
        if os.path.exists(bak) and read_config_lines(bak)[0] == new:
            shutil.copyfile(bak, config + '.part')       # the pre-install file, byte for byte
            os.replace(config + '.part', config)
        else:
            write_lines(config, new, nl, final)
        log('configpc.txt: %s removed from the filesystem line' % ZIP)
    else:
        log('configpc.txt: not installed, unchanged')
    for f in (ZIP, ZIP + '.manifest.json'):
        p = os.path.join(folder, f)
        if os.path.exists(p):
            os.remove(p)
            log('removed %s' % p)
    return changed


def status(config, log=print):
    lines, _, _ = read_config_lines(config)
    idx, names = parse_filesystem(lines)
    on = bool(names) and names[0].lower() == ZIP
    present = os.path.exists(os.path.join(os.path.dirname(os.path.abspath(config)), ZIP))
    log('filesystem line: %s' % (lines[idx] if idx is not None else '(none: vanilla archives)'))
    log('installed: %s, zip present: %s' % (on, present))
    return on


def sha_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', choices=['install', 'uninstall', 'status'])
    ap.add_argument('--config', required=True, help='the configpc.txt to edit')
    ap.add_argument('--zip', help='install: built zip to copy next to configpc.txt')
    a = ap.parse_args(argv)
    try:
        if a.action == 'install':
            install(a.config, a.zip)
        elif a.action == 'uninstall':
            uninstall(a.config)
        else:
            status(a.config)
    except InstallError as e:
        print('REFUSED: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
