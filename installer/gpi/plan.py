"""Selection -> install plan. Pure: no file access except reading the shipped ini text passed in.

A plan says exactly what an install does:
  data      the data options for data/build_gameplay_mod.py (every option, vanilla where no group sets it;
            difficulty is always `normal`: the DLL applies the levels at run time, the menu chooses them)
  ini       [(section, key, value, mode)] for seamless/tools/merge_ini.py; mode `set` = always written (a missing
            section is copied from the shipped ini), `if_present` = only when the player's ini already has that key
            (used to switch a feature OFF: a missing key is already off, nothing is deleted)
  files     DLLs to copy / remove, the seamless prefetch table, the difficulty marker
  display   configpc.txt fullscreen / screensize and the registry XRes / YRes
"""
import re

from . import catalog

# TODGameplay.map / TODPhysics.map: the linker maps of the same build, next to the DLLs; the hang
# watchdog names the stuck code from them (a map of another build is refused). Copied and removed with their DLL.
DLL_FILES = ['TODGameplay.dll', 'dsound.dll', 'TODGameplay.map']
PHYSICS_DLL = 'TODPhysics.dll'
PHYSICS_MAP = 'TODPhysics.map'
ZIP = 'todgameplay_data.zip'
PREFETCH = 'TODGameplay_seamless.txt'
DIFF_MARKER = 'todgameplay_difficulty.txt'
INI = 'TODGameplay.ini'
# HD effects (hdfx/README.md): the TOD HD d3d9.dll used as released, its ini, and the files hdfx_regen.py builds from the
# player's own game files. Paths relative to the game folder.
HDFX_DLL = 'd3d9.dll'
HDFX_INI = 'tod_hd_fx.ini'
HDFX_ASIDE = 'd3d9.dll.before_hdfx'        # another program's d3d9.dll, set aside while the HD effects are on
HDFX_FILES = [HDFX_DLL, HDFX_INI, 'tod_hd_light.txt'] + ['tod_hd_fx/' + f for f in (
    'light/classes.txt', 'light/emitters.txt', 'light/manifest.txt', 'light/sh.txt', 'particles/keys.txt',
    'particles/kinds.txt', 'particles/manifest.txt', 'sky/keys.txt', 'sky/manifest.txt', 'terrain/triplanar.txt',
    'volumetric/cones.txt', 'water/water.txt', 'water/water_normal.dds', 'water/water_foam.dds', 'water/SOURCES.txt')]
# sub-option -> the tod_hd_fx.ini keys it switches: (key, value when on, value when off)
HDFX_SUBS = {
    'hd_filtering': [('aniso_all', 16, 1)],
    'hd_ao': [('ssao', 1, 0)],
    'hd_post': [('post', 1, 0)],
    'hd_shafts': [('light_shafts', 1, 0), ('mist', 1, 0), ('volumetric_beams', 1, 0), ('volumetric_light', 1, 0)],
    'hd_particles': [('particle_mips', 1, 0), ('soft_particles', 1, 0), ('part_lit', 1, 0), ('heat_haze', 1, 0)],
    'hd_lighting': [('lighting_mode', 1, 0)],
    'hd_water': [('water_mode', 1, 0)],
    'hd_shadows': [('dyn_shadows', 1, 0)],
    'hd_static_shadows': [('static_shadows', 1, 0)],
    'hd_fps_cap': [('fps_cap', 60, 0)],
}
SELECTION_FILE = 'Gameplay+ install.json'
# PSYCHO's enemy counts (psycho-tune: about 25 % fewer enemies in total, damage kept): the
# same values as the shipped dll/TODGameplay.ini, written with mode `set`
PSYCHO_TUNE = {
    'bigger_fights': [('bigger_fights', 'cap_scale_psycho', '1.3'), ('bigger_fights', 'psycho_wake_share', '0.6')],
    'difficulty': [('difficulty', 'reinforce_share_psycho', '0.375')],   # strings: fmt() keeps 2 decimals
}
WINDOWED_SIZE = (1920, 1080)
# MAYHEM title: the art next to TOD.exe, from dist\ (without the first two the
# feature stays off; without the fire file the fire layer is skipped)
MAYHEM_DDS = ['TODGameplay_mayhem.dds', 'TODGameplay_mayhem_fx.dds', 'TODGameplay_mayhem_fire.dds']
# [minimap]: hud\tools\bake_minimap.py writes this folder from the player's own
# game files; the installer owns exactly the files that tool makes (minimap_files)
MINIMAP_DIR = 'TODGameplay_minimap'
# [display] mode_menu keeps the mode picked in game in <Documents>\Total Overdose\savegames\<this> (display/README.md); the only
# file outside the game folder the installer may touch: backed up, then deleted when the player picks a start mode
DISPLAY_SIDE = 'TODGameplay_display.txt'

# every file the installer may create, replace or delete in the game folder (a "changed files only" backup covers
# these and every file of MINIMAP_DIR; a restore deletes the ones the backup says were absent)
OWNED_FILES = DLL_FILES + [PHYSICS_DLL, PHYSICS_MAP, INI, ZIP, ZIP + '.manifest.json', PREFETCH, DIFF_MARKER, 'configpc.txt',
                           'configpc.txt.todgameplay.bak', SELECTION_FILE] + HDFX_FILES + [HDFX_ASIDE] + MAYHEM_DDS


def ini_has(text, section, key=None):
    """Does the ini text have [section] (and key in it)?"""
    cur = None
    for ln in (text or '').splitlines():
        m = re.match(r'\s*\[([^\]]+)\]', ln)
        if m:
            cur = m.group(1).strip().lower()
            if key is None and cur == section.lower():
                return True
            continue
        if key is not None and cur == section.lower():
            m = re.match(r'\s*([A-Za-z0-9_]+)\s*=', ln)
            if m and m.group(1).lower() == key.lower():
                return True
    return False


def retire_key(text, section, key, value):
    """An older install's line of a feature that no longer ships: `key = value` in place, every other byte kept. The
    shipped ini has no such section, so merge_ini.py cannot reach it."""
    cur = None
    lines = (text or '').split('\n')
    for i, ln in enumerate(lines):
        m = re.match(r'\s*\[([^\]]+)\]', ln)
        if m:
            cur = m.group(1).strip().lower()
            continue
        if cur == section.lower():
            m = re.match(r'(\s*' + re.escape(key) + r'\s*=[ \t]*)([^\s;#]*)(.*)$', ln, re.I | re.S)
            if m:
                lines[i] = m.group(1) + str(value) + m.group(3)
    return '\n'.join(lines)


def fmt(v):
    if isinstance(v, bool):
        return '1' if v else '0'
    if isinstance(v, float):
        s = ('%.2f' % v).rstrip('0')
        return s + '0' if s.endswith('.') else s
    return str(v)


def group_spec(gid, subs, shipped_ini='', native=None):
    """What one group does when ON and what turning it OFF writes.
    Returns dict(data={opt: level}, on=[(sec, key, val)], off=[(sec, key, val, mode)], dll=bool, files=[...],
    notes=[...])."""
    s = subs
    d = dict(data={}, on=[], off=[], dll=False, files=[], notes=[])
    if gid == 'essentials':
        d['dll'] = True
        crash = 1 if s.get('ess_crash', True) else 0
        stab = 1 if s.get('ess_stability', True) else 0
        d['on'] = [('fixes', 'vector_realloc', crash), ('fixes', 'weapon_owner', crash), ('timing', 'clock_fix', stab),
                   ('timing', 'pickup_step_fix', stab), ('controls', 'mouse_turn_fix', stab),
                   ('controls', 'diagonal_normalize', stab)]
        # the two crash fixes default to ON in the DLL when the key is missing: write 0 explicitly
        d['off'] = [('fixes', 'vector_realloc', 0, 'set'), ('fixes', 'weapon_owner', 0, 'set'),
                    ('timing', 'clock_fix', 0, 'if_present'), ('timing', 'pickup_step_fix', 0, 'if_present'),
                    ('controls', 'mouse_turn_fix', 0, 'if_present'), ('controls', 'diagonal_normalize', 0, 'if_present')]
        d['notes'] = ['Border-swap double-free fix (vanilla engine bug)', 'Weapon-reload crash fix (vanilla engine bug)',
                      'Slow motion no longer stalls timers at high frame rates', 'Pickups fall at the same speed at any fps',
                      'Mouse turning independent of the frame rate', 'Diagonal running no longer 41 % faster']
    elif gid == 'display':
        d['dll'] = True
        border = s.get('display_mode') == 'borderless'
        w, h = (native or WINDOWED_SIZE) if border else WINDOWED_SIZE
        # mode_menu: SETTINGS > SCREEN RESOLUTION lists the real modes and keeps the pick in the side file;
        # configpc / borderless / registry below are then only the first start's default
        d['on'] = [('display', 'enabled', 1), ('display', 'mode_menu', 1), ('display', 'borderless', 1 if border else 0)]
        d['off'] = [('display', 'enabled', 0, 'if_present')]
        d['display'] = dict(fullscreen=border, w=w, h=h, apply=bool(s.get('display_apply')))
        d['files'] = ['configpc.txt: fullscreen = %s, screensize = %d,%d' % ('true' if border else 'false', w, h),
                      'registry HKCU\\Software\\Eidos\\Total Overdose XRes / YRes = %d x %d' % (w, h)]
        if s.get('display_apply'):
            d['files'].append('Documents\\Total Overdose\\savegames\\%s (the mode kept in game): backed up, then '
                              'deleted, so this start mode applies' % DISPLAY_SIDE)
        d['notes'] = ['16:9 resolution list, HUD at the screen edges, menus pillarboxed instead of stretched',
                      'Change the mode in game any time: SETTINGS > SCREEN RESOLUTION (kept for the next start)']
    elif gid == 'controller':
        d['dll'] = True
        d['data'] = {'controller': 'menu'}
        pct = int(s.get('pad_rumble_pct', 100))
        d['on'] = [('pad', 'enabled', 1), ('pad', 'prompts', s.get('pad_prompts', 'auto')),
                   ('pad', 'rumble', 1 if pct > 0 else 0)]
        if pct > 0:
            d['on'].append(('pad', 'rumble_strength', round(pct / 100.0, 2)))
        d['off'] = [('pad', 'enabled', 0, 'if_present')]
        d['notes'] = ['SETTINGS > VIBRATION row shown on PC', 'Prompts follow the device used last']
    elif gid == 'driving':
        d['data'] = {'driving': 'better_feel'}
        if s.get('chase_camera'):
            d['data']['vehicle_camera'] = 'far'
        if s.get('steer_return'):
            d['on'].append(('vehicles', 'steering_return_rate', 0.25))
        if s.get('brake_parity'):
            d['on'].append(('vehicles', 's_brake_parity', 1))
        if d['on']:
            d['dll'] = True
        # the two experimental keys are switched back off whenever their checkbox (or the group) is off
        if not s.get('steer_return'):
            d['off_always'] = [('vehicles', 'steering_return_rate', 0, 'if_present')]
        if not s.get('brake_parity'):
            d.setdefault('off_always', []).append(('vehicles', 's_brake_parity', 0, 'if_present'))
        d['off'] = [('vehicles', 'steering_return_rate', 0, 'if_present'), ('vehicles', 's_brake_parity', 0, 'if_present')]
        d['notes'] = ['Steering ramp 0.12, cornering x0.9, slide control x0.5 (25 vehicles)']
    elif gid == 'seamless':
        d['dll'] = True
        d['data'] = {'seamless': 'on_nomarkers' if s.get('seamless_hide_markers', True) else 'on'}
        d['on'] = [('seamless', 'enabled', 1), ('seamless', 'prefetch', 1 if s.get('seamless_prefetch', True) else 0)]
        d['off'] = [('seamless', 'enabled', 0, 'if_present')]
        d['files'] = [PREFETCH + ' (rebuilt for the new data zip)']
        d['notes'] = ['Downtown-Harbour bridge and the Downtown-Industrial tunnel become walk borders',
                      'The farm pass stays a teleport']
    elif gid == 'suave':
        d['dll'] = True
        slot = str(s.get('suave_slot', '10'))
        song = str(s.get('suave_song') or '').strip()
        d['data'] = {'suave_anims': 'on', 'suave': 'on_slot10' if slot == '10' else 'on'}
        if not song:
            d['data']['suave_music'] = 'off'              # no stream records, no oggs: Suave without its music
        d['on'] = [('suave', 'enabled', 1), ('suave', 'slot', int(slot)), ('suave', 'music', 1 if song else 0)]
        d['off'] = [('suave', 'enabled', 0, 'if_present')]
        d['notes'] = ['22 new animation clips, HUD name "Suave"',
                      '4 music segments cut from your copy of the song (read only, on this PC)' if song else
                      'No music: no song file given (pick your own copy to add it)']
    elif gid == 'smart_ai':
        d['dll'] = True
        d['on'] = [('smart_ai', 'enabled', 1), ('smart_ai', 'squad', 1 if s.get('smart_squad', True) else 0),
                   ('smart_ai', 'morale', 1 if s.get('smart_morale', True) else 0)]
        d['off'] = [('smart_ai', 'enabled', 0, 'if_present')]
    elif gid == 'qol':
        d['dll'] = True
        d['on'] = [('bullettime', 'enabled', 1), ('bullettime', 'key', s.get('bt_key', 'G')),
                   ('restart', 'enabled', 1), ('restart', 'key', s.get('restart_key', 'F10'))]
        d['off'] = [('bullettime', 'enabled', 0, 'if_present'), ('restart', 'enabled', 0, 'if_present')]
        d['notes'] = ['Bullet time meter: 8 s, refills from kills and combos',
                      'Quick restart (hold 1 s) uses the game\'s own retry']
    elif gid == 'physics':
        d['dll'] = True
        d['on'] = [('physics', 'enabled', 1)]
        d['off'] = [('physics', 'enabled', 0, 'if_present')]
        d['files'] = [PHYSICS_DLL, PHYSICS_MAP]
        d['notes'] = ['Box3D (MIT) rigid bodies for loose props; cosmetic, the gameplay collision stays the game\'s',
                      'shell_impulse = 450 and blast_lift = 4 stay as they are']
    elif gid == 'difficulty':
        d['dll'] = True
        d['data'] = {'difficulty_menu': 'on_all_languages' if s.get('difficulty_all_languages') else 'on'}
        d['on'] = [('difficulty', 'enabled', 1)] + PSYCHO_TUNE['difficulty']
        # PSYCHO's enemy loco moves: its own section, active only while the level is PSYCHO
        psycho = bool(s.get('psycho_loco', True))
        if psycho:
            d['on'].append(('psycho', 'enemy_loco_moves', 1))
            d['on'].append(('psycho', 'elite_drops', 1 if s.get('psycho_drops', True) else 0))
        else:
            d['off_always'] = [('psycho', 'enemy_loco_moves', 0, 'if_present')]
        d['off'] = [('difficulty', 'enabled', 0, 'if_present'), ('psycho', 'enemy_loco_moves', 0, 'if_present'),
                    ('drops', 'enabled', 0, 'if_present')]
        # [drops] (docs/mods/drops.md): more pickups from dead enemies on every level, the game's own goodies
        loot = bool(s.get('loot_drops', True))
        if loot:
            d['on'] += [('drops', 'enabled', 1), ('drops', 'ammo_scale', s.get('loot_ammo', 1.75)),
                        ('drops', 'health_chance', 0.2), ('drops', 'health_hurt_bonus', 0.3), ('drops', 'powerup_chance', 0.05),
                        ('drops', 'drop_seconds', 30), ('drops', 'max_on_ground', 3)]
        else:
            d['off_always'] = d.get('off_always', []) + [('drops', 'enabled', 0, 'if_present')]
        d['files'] = [DIFF_MARKER + ' = menu']
        d['notes'] = ['Pick the level in OPTIONS > SETTINGS > DIFFICULTY LEVEL',
                      'BRUTAL: harder hits, tighter aim, +50 % enemies',
                      'PSYCHO: BRUTAL, and elites (200+ hp) and bosses use the Tornado spin and the El Toro charge, '
                      'with a 1 s red wind-up; a killed elite drops its move as a pickup' if psycho and s.get('psycho_drops', True)
                      else 'PSYCHO: BRUTAL, and elites (200+ hp) and bosses use the Tornado spin and the El Toro charge, '
                      'with a 1 s red wind-up' if psycho else 'PSYCHO: the same as BRUTAL (loco moves off)']
        if loot:
            d['notes'].append('Every level: guns drop %g× the ammo, a kill leaves health 20-50 %% of the time (more '
                              'when you are hurt) and a power-up 5 %% of the time' % float(s.get('loot_ammo', 1.75)))
    elif gid == 'hdfx':
        d['dll'] = True                      # the dsound.dll loader points the d3d9.dll's logs out of the game folder
        sets, on = [], []
        for sid, keys in HDFX_SUBS.items():
            want = bool(s.get(sid, True if sid != 'hd_static_shadows' else False))
            if sid in ('hd_shadows', 'hd_water'):
                want = want and bool(s.get('hd_lighting', True))       # both are drawn by the per-pixel lighting
            if sid == 'hd_static_shadows':
                want = want and bool(s.get('hd_shadows', True)) and bool(s.get('hd_lighting', True))
            for k, von, voff in keys:
                sets.append('%s=%s' % (k, von if want else voff))
            if want:
                on.append(sid)
        d['hdfx'] = dict(sets=sets, subs=on)
        if s.get('hd_fps_cap', True):
            # one frame limiter: the d3d9.dll's cap replaces [timing] fps_limit
            d['on'] = [('timing', 'fps_limit', 0)]
        d['files'] = ['d3d9.dll (the TOD HD effects DLL, unchanged), tod_hd_fx.ini, tod_hd_light.txt',
                      'tod_hd_fx\\ (material, sprite and sky data built from your own game files)']
        d['notes'] = ['Uses the TOD HD effects DLL as released; the effects that need TOD HD art stay off',
                      'Logs in %LOCALAPPDATA%\\TODGameplay\\hdfx_logs; F8 flips the post pass while playing']
    elif gid == 'more_enemies':
        d['dll'] = True
        d['on'] = [('difficulty', 'max_live_enemies', int(s.get('max_live_enemies', 9)))]
        d['off'] = [('difficulty', 'max_live_enemies', 0, 'if_present')]
        d['notes'] = ['Only on BRUTAL and PSYCHO; the pool has 9 character shells (vanilla cap 6 or 8)']
    elif gid == 'hud':
        d['dll'] = True
        mm, mk = bool(s.get('hud_minimap', True)), bool(s.get('hud_markers', True))
        # the weapon wheel is gone: an older install's ini goes to enabled = 0, whatever this card says (the DLL ignores
        # the section anyway)
        d['off_always'] = [('weapon_wheel', 'enabled', 0, 'retired')]
        if mm or mk:
            d['on'] += [('minimap', 'enabled', 1), ('minimap', 'map', 1 if mm else 0),
                        ('minimap', 'objective_marker', 1 if mk else 0), ('minimap', 'world_marker', 1 if mk else 0)]
            if mm:
                d['on'].append(('minimap', 'rotate', 1 if s.get('hud_rotate', True) else 0))
        else:
            d['off_always'].append(('minimap', 'enabled', 0, 'if_present'))
        d['off'] = [('minimap', 'enabled', 0, 'if_present'), ('weapon_wheel', 'enabled', 0, 'retired')]
        d['minimap_bake'] = mm
        if mm:
            d['files'] = [MINIMAP_DIR + '\\ (minimap.txt, one tile pack per map, bake.json: about 25 MB made from your '
                          'game files by hud\\tools\\bake_minimap.py, 2-5 min the first time and after a map style '
                          'update)']
        d['notes'] = ['The map ink sits on the radar\'s own parchment; the game\'s blips stay on top'] if mm else []
    elif gid == 'hud_textures':
        # HD HUD icons: a data option only (the art goes into overdose.main inside the data zip); off = the zip
        # rebuilt without it. No ini keys, no files of its own.
        d['data'] = {'hud_textures': 'on'}
        d['notes'] = ['57 of the game\'s own HUD textures redrawn in HD: pickup coins, ammo icons and the bullet bar, the '
                      '28 weapon selector cards, the combo skull',
                      'Saves are not affected; the data zip is about 28 MB bigger']
    elif gid == 'ragdoll':
        d['dll'] = True
        d['on'] = [('ragdoll', 'enabled', 1), ('ragdoll', 'impulse_scale', float(s.get('ragdoll_impulse', 1.0)))]
        d['off'] = [('ragdoll', 'enabled', 0, 'if_present')]
        d['notes'] = ['Our own active ragdolls in the Physics Box3D world (needs TODPhysics.dll)',
                      'Dead enemies stay 4 s before the game takes the body (vanilla 0.5 s)']
    elif gid == 'car_physics':
        d['dll'] = True
        d['on'] = [('car_physics', 'enabled', 1), ('car_physics', 'impulse_scale', float(s.get('car_impulse', 1.0)))]
        d['off'] = [('car_physics', 'enabled', 0, 'if_present')]
        d['notes'] = ['Cars as Box3D boxes in the Physics world; no damage is added (needs TODPhysics.dll)']
    elif gid == 'chain_reactions':
        d['dll'] = True
        d['on'] = [('mayhem', 'chain_reactions', 1), ('mayhem', 'chain_cars', 1 if s.get('chain_cars', False) else 0)]
        d['off'] = [('mayhem', 'chain_reactions', 0, 'if_present')]
        d['notes'] = ['Through the game\'s own damage: its own explosions, fire and sound follow']
    elif gid == 'breakable_cover':
        d['dll'] = True
        d['on'] = [('mayhem', 'breakable_cover', 1)]
        d['off'] = [('mayhem', 'breakable_cover', 0, 'if_present')]
        d['notes'] = ['Cover props break after about a third of the hits; Box3D throws the pieces']
    elif gid == 'bigger_fights':
        d['dll'] = True
        d['data'] = {'bigger_fights': 'on'}
        # psycho-tune: PSYCHO's counts a quarter lower; written every install, so an ini from before
        # (cap_scale_psycho = 1.85, no wake share) gets them; BRUTAL's keys stay as the player has them
        d['on'] = [('bigger_fights', 'enabled', 1)] + PSYCHO_TUNE['bigger_fights']
        d['off'] = [('bigger_fights', 'enabled', 0, 'if_present')]
        d['notes'] = ['Old saves do not load with this on: start a NEW GAME. New saves are kept apart as BFSlot00.., your '
                      'old Slot00.. are not touched and come back when this is switched off',
                      'More enemies only on BRUTAL and PSYCHO; lower levels play as before',
                      'Bigger data zip: about 80 MB more']
    elif gid == 'mayhem_heat':
        d['dll'] = True
        esc = bool(s.get('heat_escalation', True))
        d['on'] = [('mayhem_heat', 'enabled', 1), ('mayhem_heat', 'escalation', 1 if esc else 0),
                   ('mayhem_heat', 'decay_seconds', int(s.get('heat_decay', 10))),
                   ('mayhem_heat', 'tint', 1.0 if s.get('heat_tint', True) else 0.0),
                   ('mayhem_heat', 'icons', 1 if s.get('heat_icons', True) else 0),
                   ('mayhem_heat', 'score_multiplier', 1 if s.get('heat_score', True) else 0),
                   ('mayhem_heat', 'score_label', 0)]   # heat-chili: the xN label is gone; an older ini's 1 is turned off
        if esc:
            d['on'].append(('mayhem_heat', 'escalation_scale', float(s.get('heat_escalation_scale', 1.0))))
        d['off'] = [('mayhem_heat', 'enabled', 0, 'if_present'), ('mayhem_heat', 'score_label', 0, 'if_present')]
        d['notes'] = ['Scaled by the difficulty: easy 0.4, normal 0.7, hard 0.85, brutal 1, psycho 1.25',
                      'Score multiplier: no heat and level 1 x1, then x1.25 / x1.5 / x1.75 / x2, on top of the combo',
                      'Reinforcements keep every mission rule of the Difficulty engine (runs without it too)']
    elif gid == 'gang_wars':
        d['dll'] = True
        d['on'] = [('gang_wars', 'enabled', 1), ('gang_wars', 'provoke', 1 if s.get('gw_provoke', True) else 0),
                   ('gang_wars', 'spark', 1 if s.get('gw_spark', False) else 0)]
        d['off'] = [('gang_wars', 'enabled', 0, 'if_present')]
        d['notes'] = ['Mission scripts untouched: buddies, bosses and scripted characters never switch sides']
    return d


def mayhem_title_available(shipped_ini):
    return ini_has(shipped_ini, 'mayhem_title', 'enabled')


def make_plan(selection, shipped_ini='', native=None, registry=True):
    sel = catalog.normalize(selection)
    groups, subs = sel['groups'], sel['subs']
    data = {}
    ini = []
    dll = False
    display = None
    effects = {}
    for g in catalog.GROUPS:
        gid = g['id']
        spec = group_spec(gid, subs, shipped_ini, native)
        effects[gid] = spec_effects(spec)
        if groups[gid]:
            data.update(spec['data'])
            ini += [(a, b, fmt(c), 'set') for a, b, c in spec['on']]
            ini += [(a, b, fmt(c), m) for a, b, c, m in spec.get('off_always', [])]
            dll = dll or spec['dll']
            if spec.get('display'):
                display = spec['display']
        else:
            ini += [(a, b, fmt(c), m) for a, b, c, m in spec['off']]
    mayhem_on = any(groups[m] for m in catalog.MAYHEM)
    title = mayhem_title_available(shipped_ini)
    if title:
        ini.append(('mayhem_title', 'enabled', '1' if mayhem_on else '0', 'set' if mayhem_on else 'if_present'))
        effects['mayhem_title'] = [dict(kind='ini', text='[mayhem_title] enabled = 1 (MAYHEM burns through the title '
                                                          'screen and into the main menu)'),
                                   dict(kind='file', text=', '.join(MAYHEM_DDS))]
    hud = group_spec('hud', subs, shipped_ini, native) if groups.get('hud') else None
    # data options: every option of the catalogue, vanilla unless a group sets it; static difficulty stays normal
    opts = None
    try:
        from . import repo
        opts = repo.options()
    except Exception:
        pass
    full = opts.defaults() if opts else {}
    full.update(data)
    full['difficulty'] = 'normal'
    data_needed = any(v != (opts.BY_KEY[k]['levels'][0] if opts else None) for k, v in full.items()) if opts \
        else bool(data)
    # dedupe ini keys: the last write of a (section, key) wins, a `set` beats an `if_present`
    seen = {}
    for item in ini:
        k = (item[0].lower(), item[1].lower())
        if k in seen and seen[k][3] == 'set' and item[3] == 'if_present':
            continue
        seen[k] = item
    ini = list(seen.values())
    return dict(
        selection=sel,
        data=full,
        data_needed=data_needed,
        ini=ini,
        dll_needed=dll,
        physics_dll=groups['physics'],
        prefetch=groups['seamless'],
        hdfx=group_spec('hdfx', subs, shipped_ini, native)['hdfx'] if groups.get('hdfx') else None,
        display=display,
        registry=(display['w'], display['h']) if (display and registry) else None,
        song=str(subs.get('suave_song') or '').strip() if groups['suave'] else '',
        difficulty_marker=groups['difficulty'],
        mayhem_on=mayhem_on,
        mayhem_dds=bool(title and mayhem_on),
        minimap=bool(hud and hud.get('minimap_bake')),
        display_apply=bool(display and display.get('apply')),
        effects=effects,
    )


def saved_selection(selection):
    """The selection as `Gameplay+ install.json` keeps it: a start-mode pick applies once, so it is never pre-ticked
    next time."""
    sel = catalog.normalize(selection)
    sel['subs']['display_apply'] = False
    return sel


def spec_effects(spec):
    out = []
    for k, v in sorted(spec['data'].items()):
        out.append(dict(kind='data', text='data option %s = %s' % (k, v)))
    for a, b, c in spec['on']:
        out.append(dict(kind='ini', text='[%s] %s = %s' % (a, b, fmt(c))))
    for f in spec['files']:
        out.append(dict(kind='file', text=f))
    if spec['dll']:
        out.append(dict(kind='file', text='TODGameplay.dll + dsound.dll (loader)'))
    for n in spec['notes']:
        out.append(dict(kind='note', text=n))
    return out


def summary(plan):
    """Short human lines for the UI / log."""
    g = plan['selection']['groups']
    on = [catalog.BY_ID[k]['title'] for k, v in g.items() if v]
    off = [catalog.BY_ID[k]['title'] for k, v in g.items() if not v]
    return dict(on=on, off=off)
