"""Game folder detection, TOD.exe identity, what is installed now, disk space, monitor size."""
import hashlib
import json
import os
import re
import shutil
import sys

from . import catalog, plan as P

GOG_DEFAULT = r'C:\GOG Games\Total Overdose'
GOG_ID = '1207659179'
EXE_SIZE = 6860800
EXE_MD5 = 'd9beb351b3c78ee9f14a766a368f3ff9'     # dll/src/gp_game.cpp GOG_MD5 / GOG_SIZE


def candidates():
    """Folders that look like the GOG install (TOD.exe present), best first."""
    found = []
    if sys.platform == 'win32':
        try:
            import winreg
            for root, sub in ((winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\WOW6432Node\GOG.com\Games\%s' % GOG_ID),
                              (winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\GOG.com\Games\%s' % GOG_ID),
                              (winreg.HKEY_CURRENT_USER, r'SOFTWARE\GOG.com\Games\%s' % GOG_ID)):
                try:
                    with winreg.OpenKey(root, sub) as k:
                        p, _ = winreg.QueryValueEx(k, 'path')
                        found.append((os.path.normpath(p), 'GOG registry'))
                except OSError:
                    pass
        except ImportError:
            pass
    found.append((GOG_DEFAULT, 'GOG default folder'))
    out, seen = [], set()
    for p, how in found:
        k = os.path.normcase(p)
        if k in seen or not os.path.isfile(os.path.join(p, 'TOD.exe')):
            continue
        seen.add(k)
        out.append(dict(path=p, how=how))
    return out


DOCS_ENV = 'GPI_DOCUMENTS'      # tests / sandboxed runs: a fake Documents folder (never the player's real one)


def documents_dir(override=None):
    """The Documents folder TODGameplay.dll uses (SHGetFolderPath CSIDL_PERSONAL, as gp_display_modes.cpp does).
    `override` (the --documents argument) or the GPI_DOCUMENTS environment variable win; '' = none."""
    if override:
        return os.path.abspath(override)
    env = os.environ.get(DOCS_ENV)
    if env:
        return os.path.abspath(env)
    if sys.platform != 'win32':
        return ''
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(520)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf) == 0 and buf.value:
            return buf.value
    except Exception:
        pass
    return ''


def display_side_path(docs):
    """<Documents>\\Total Overdose\\savegames\\TODGameplay_display.txt, or None without a Documents folder."""
    return os.path.join(docs, 'Total Overdose', 'savegames', P.DISPLAY_SIDE) if docs else None


def display_side_info(path):
    """The mode the game kept (the DLL's side file: mode / width / height / monitor), or None."""
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, 'rb') as f:
            t = f.read(4096).decode('latin-1')
    except OSError:
        return None
    d = {}
    for ln in t.splitlines():
        if '=' in ln:
            k, v = ln.split('=', 1)
            d[k.strip().lower()] = v.strip()
    try:
        return dict(mode=d.get('mode', '?'), w=int(d.get('width', 0)), h=int(d.get('height', 0)), path=path)
    except ValueError:
        return dict(mode=d.get('mode', '?'), w=0, h=0, path=path)


def exe_check(game_dir):
    p = os.path.join(game_dir, 'TOD.exe')
    if not os.path.isfile(p):
        return dict(present=False, match=False, size=0, md5=None,
                    text='No TOD.exe in this folder.')
    size = os.path.getsize(p)
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    md5 = h.hexdigest()
    ok = size == EXE_SIZE and md5 == EXE_MD5
    return dict(present=True, match=ok, size=size, md5=md5,
                text='GOG TOD.exe (6,860,800 bytes, md5 matches): every DLL feature can run.' if ok else
                'This TOD.exe is not the GOG build Gameplay+ knows (%d bytes, md5 %s). The data options still apply, '
                'but the DLL features stay inactive (the DLL refuses to patch an unknown exe). TOD.exe is never '
                'modified.' % (size, md5))


def read_text(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('latin-1')
    except OSError:
        return ''


def ini_value(text, section, key):
    cur = None
    for ln in text.splitlines():
        m = re.match(r'\s*\[([^\]]+)\]', ln)
        if m:
            cur = m.group(1).strip().lower()
            continue
        if cur == section:
            m = re.match(r'\s*([A-Za-z0-9_]+)\s*=\s*([^;\r\n]*?)\s*(;.*)?$', ln)
            if m and m.group(1).lower() == key:
                return m.group(2).strip()
    return None


def installed_state(game_dir):
    """What Gameplay+ pieces are in the game folder now, and a selection guessed from them."""
    j = lambda n: os.path.join(game_dir, n)          # noqa: E731
    ini = read_text(j(P.INI))
    man = None
    try:
        with open(j(P.ZIP + '.manifest.json'), encoding='utf-8') as f:
            man = json.load(f)
    except (OSError, ValueError):
        pass
    saved = None
    try:
        with open(j(P.SELECTION_FILE), encoding='utf-8') as f:
            saved = json.load(f)
    except (OSError, ValueError):
        pass
    on = lambda s, k='enabled': (ini_value(ini, s, k) or '0').strip() not in ('0', '')   # noqa: E731
    data = (man or {}).get('selection') or {}
    files = {n: os.path.exists(j(n)) for n in P.OWNED_FILES}
    g = {
        'essentials': bool(ini) and (on('timing', 'clock_fix') or (ini_value(ini, 'fixes', 'vector_realloc') or '1') != '0'),
        'display': on('display'),
        'controller': on('pad') or data.get('controller') == 'menu',
        'driving': data.get('driving') == 'better_feel',
        'seamless': on('seamless') or data.get('seamless', 'off') != 'off',
        'suave': on('suave') or data.get('suave_anims') == 'on',
        'smart_ai': on('smart_ai'),
        'qol': on('bullettime') or on('restart'),
        'physics': on('physics') and files.get(P.PHYSICS_DLL),
        'difficulty': on('difficulty') and data.get('difficulty_menu', 'off') != 'off',
        'more_enemies': on('difficulty') and (ini_value(ini, 'difficulty', 'max_live_enemies') or '0') not in ('0', ''),
        'hud': on('minimap'),
        'hud_textures': data.get('hud_textures', 'off') != 'off',
        'ragdoll': on('ragdoll') and files.get(P.PHYSICS_DLL),
        'car_physics': on('car_physics') and files.get(P.PHYSICS_DLL),
        'chain_reactions': on('mayhem', 'chain_reactions') and files.get(P.PHYSICS_DLL),
        'breakable_cover': on('mayhem', 'breakable_cover') and files.get(P.PHYSICS_DLL),
        'mayhem_heat': on('mayhem_heat'),
        'bigger_fights': on('bigger_fights') and data.get('bigger_fights', 'off') != 'off',
        'gang_wars': on('gang_wars'),
    }
    subs = {}
    bt = ini_value(ini, 'bullettime', 'key')
    if bt:
        subs['bt_key'] = bt.upper()
    slot = ini_value(ini, 'suave', 'slot')
    if slot in ('10', '11'):
        subs['suave_slot'] = slot
    rk = ini_value(ini, 'restart', 'key')
    if rk:
        subs['restart_key'] = rk.upper()
    v = ini_value(ini, 'pad', 'prompts')
    if v:
        subs['pad_prompts'] = v
    try:
        if ini_value(ini, 'pad', 'rumble') == '0':
            subs['pad_rumble_pct'] = 0
        elif ini_value(ini, 'pad', 'rumble_strength'):
            subs['pad_rumble_pct'] = int(round(float(ini_value(ini, 'pad', 'rumble_strength')) * 10) * 10)
    except ValueError:
        pass
    for k, sk in (('squad', 'smart_squad'), ('morale', 'smart_morale')):
        if ini_value(ini, 'smart_ai', k) in ('0', '1'):
            subs[sk] = ini_value(ini, 'smart_ai', k) == '1'
    if ini_value(ini, 'fixes', 'vector_realloc') == '0':
        subs['ess_crash'] = False
    if ini and not on('timing', 'clock_fix'):
        subs['ess_stability'] = False
    if ini_value(ini, 'display', 'borderless') == '1':
        subs['display_mode'] = 'borderless'
    mle = ini_value(ini, 'difficulty', 'max_live_enemies')
    if mle and mle != '0':
        subs['max_live_enemies'] = mle
    if data.get('seamless') == 'on':
        subs['seamless_hide_markers'] = False
    if data.get('vehicle_camera') == 'far':
        subs['chase_camera'] = True
    if ini_value(ini, 'psycho', 'enemy_loco_moves') in ('0', '1'):
        subs['psycho_loco'] = ini_value(ini, 'psycho', 'enemy_loco_moves') == '1'
    if ini_value(ini, 'psycho', 'elite_drops') in ('0', '1'):
        subs['psycho_drops'] = ini_value(ini, 'psycho', 'elite_drops') == '1'
    if ini_value(ini, 'drops', 'enabled') in ('0', '1'):
        subs['loot_drops'] = ini_value(ini, 'drops', 'enabled') == '1'
    if ini_value(ini, 'drops', 'ammo_scale'):
        try:
            subs['loot_ammo'] = float(ini_value(ini, 'drops', 'ammo_scale'))
        except ValueError:
            pass
    if on('minimap'):                                 # the group is installed: which parts (else the defaults)
        subs['hud_minimap'] = ini_value(ini, 'minimap', 'map') != '0'
        subs['hud_markers'] = ini_value(ini, 'minimap', 'world_marker') != '0'
        subs['hud_rotate'] = ini_value(ini, 'minimap', 'rotate') != '0'
    for sec, key, sk in (('ragdoll', 'impulse_scale', 'ragdoll_impulse'), ('car_physics', 'impulse_scale', 'car_impulse'),
                         ('mayhem_heat', 'escalation_scale', 'heat_escalation_scale'),
                         ('mayhem_heat', 'decay_seconds', 'heat_decay')):
        v = ini_value(ini, sec, key)
        if v:
            subs[sk] = v
    for sec, key, sk in (('mayhem', 'chain_cars', 'chain_cars'), ('mayhem_heat', 'escalation', 'heat_escalation'),
                         ('mayhem_heat', 'icons', 'heat_icons'), ('mayhem_heat', 'score_multiplier', 'heat_score'),
                         ('gang_wars', 'provoke', 'gw_provoke'),
                         ('gang_wars', 'spark', 'gw_spark')):
        if ini_value(ini, sec, key) in ('0', '1'):
            subs[sk] = ini_value(ini, sec, key) == '1'
    if ini_value(ini, 'mayhem_heat', 'tint') is not None:
        try:
            subs['heat_tint'] = float(ini_value(ini, 'mayhem_heat', 'tint')) > 0
        except ValueError:
            pass
    detected = catalog.normalize(dict(groups=g, subs=subs))
    any_mod = bool(ini) or files.get(P.ZIP) or files.get('TODGameplay.dll')
    return dict(files=files, data_selection=data or None, ini_present=bool(ini), detected=detected if any_mod else None,
                saved=catalog.normalize(saved.get('selection')) if saved and saved.get('selection') else None,
                saved_info=dict(installed_at=saved.get('installed_at'), backup=saved.get('backup')) if saved else None,
                static_difficulty=(data.get('difficulty') if data.get('difficulty', 'normal') != 'normal' else None))


def initial_selection(game_dir):
    st = installed_state(game_dir) if game_dir and os.path.isdir(game_dir) else {}
    if st.get('saved'):
        return st['saved'], 'saved'
    if st.get('detected'):
        return st['detected'], 'detected'
    return catalog.default_selection(), 'defaults'


def folder_files(game_dir):
    out = []
    for root, dirs, files in os.walk(game_dir):
        for f in files:
            p = os.path.join(root, f)
            out.append((os.path.relpath(p, game_dir), os.path.getsize(p)))
    return out


def drive_of(path):
    d = os.path.splitdrive(os.path.abspath(path))[0]
    return d.upper() if d else os.path.abspath(path)


def free_bytes(path):
    p = os.path.abspath(path)
    while p and not os.path.exists(p):
        np = os.path.dirname(p)
        if np == p:
            break
        p = np
    try:
        return shutil.disk_usage(p).free
    except OSError:
        return 0


def native_size():
    if sys.platform != 'win32':
        return None
    try:
        import ctypes
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
        w, h = ctypes.windll.user32.GetSystemMetrics(0), ctypes.windll.user32.GetSystemMetrics(1)
        return (int(w), int(h)) if w and h else None
    except Exception:
        return None


def default_backup_root(game_dir):
    return os.path.join(os.path.dirname(os.path.abspath(game_dir)), 'Gameplay+ Backups')


def human(n):
    n = float(n)
    for u in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024 or u == 'TB':
            return ('%.0f %s' % (n, u)) if u in ('B', 'KB') else ('%.2f %s' % (n, u))
        n /= 1024
