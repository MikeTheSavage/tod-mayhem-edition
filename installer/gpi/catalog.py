"""The feature groups shown by the installer: one card per group, each a group of data options, ini keys and files.

Every mapping here was checked against the repo (2026-10-28):
  data options  data/gameplay/options.py OPTIONS (keys and levels)
  ini keys      dist/TODGameplay.ini and the DLL's reads in dll/src/*.cpp (defaults noted where they are not 0)
  files         dist/TODGameplay.dll, dist/dsound.dll (loader), dist/TODPhysics.dll ([physics] only),
                the HD effects (hdfx/: the TOD HD d3d9.dll, tod_hd_fx.ini, tod_hd_light.txt, tod_hd_fx/),
                TODGameplay_seamless.txt (seamless/tools/make_prefetch_table.py), todgameplay_difficulty.txt
The plan (plan.py) turns a selection {groups: {id: bool}, subs: {id: value}} into exact steps.
"""

# sub-option types: select (options = [[value, label], ...]), range (min, max, step), check (bool),
# key (a TODGameplay.dll key name, see KEY_NAMES), file (a path to a file the player owns; filetypes = the picker's filter)
GROUPS = [
    dict(id='essentials', section='main', title='Essentials / Fixes', default=True,
         tagline='Crash fixes and frame-rate fixes. Keep this on.',
         warn_off='Game may crash without these.',
         subs=[dict(id='ess_crash', type='check', label='Crash fixes', default=True),
               dict(id='ess_stability', type='check', label='Stability fixes', default=True)]),
    dict(id='display', section='main', title='Display', default=True,
         tagline='16:9: widescreen HUD and menus with correct proportions.',
         note='The start mode is only the first start\'s default. Change it in game any time: SETTINGS > SCREEN '
              'RESOLUTION.',
         subs=[dict(id='display_mode', type='select', label='Start in', default='windowed',
                    options=[['windowed', 'Windowed 1920\u00d71080'], ['borderless', 'Borderless native']]),
               # ticked by the page when the player picks a start mode; never saved (engine: saved selection)
               dict(id='display_apply', type='check', default=False,
                    label='Use it at the next start (replaces the mode you kept in game)')]),
    dict(id='controller', section='main', title='Controller', default=True,
         tagline='Xbox pads and DualShock / DualSense, button prompts, rumble.',
         subs=[dict(id='pad_prompts', type='select', label='Button prompts', default='auto',
                    options=[['auto', 'Auto'], ['keyboard', 'Keyboard'], ['xbox', 'Xbox'],
                             ['playstation', 'PlayStation']]),
               dict(id='pad_rumble_pct', type='range', label='Rumble', default=100, min=0, max=200, step=10,
                    unit='%')]),
    dict(id='driving', section='main', title='Better Driving', default=True,
         tagline='Smoother steering and grippier cars.',
         subs=[dict(id='chase_camera', type='check', label='Chase camera: every vehicle camera 1.5 m back',
                    default=False),
               dict(id='steer_return', type='check', label='Wheels return to straight gradually (experimental)',
                    default=False),
               dict(id='brake_parity', type='check', label='S brakes as hard as V (experimental)', default=False)]),
    dict(id='seamless', section='main', title='Seamless World', default=True, experimental=True,
         tagline='Walk across map borders without load screens.',
         subs=[dict(id='seamless_hide_markers', type='check', label='Hide the border beams and area signs',
                    default=True),
               dict(id='seamless_prefetch', type='check', label='Read the next area ahead of time', default=True)]),
    dict(id='suave', section='main', title='Suave / Pi\u00f1ata Loco', default=True, experimental=True,
         tagline='A salsa with the nearest enemy, a ring of clapping thugs and a dynamite finale.',
         note='The music is not included: it is a commercial recording. Pick your own copy of "Suavemente" by Elvis '
              'Crespo (the studio version: mp3, wav, flac or ogg) and the installer cuts four short clips from it on '
              'this PC; the file is only read and never copied or uploaded. Without a file Suave installs and works '
              'without music.',
         subs=[dict(id='suave_slot', type='select', label='Power-up slot', default='10',
                    options=[['10', 'Slot 10: replaces the Pi\u00f1ata (tested)'], ['11', 'Slot 11: Tequila']]),
               dict(id='suave_song', type='file', label='Your copy of Suavemente', default='',
                    filetypes='Audio files|*.mp3;*.wav;*.flac;*.ogg;*.oga;*.aif;*.aiff;*.w64;*.caf|All files|*.*')]),
    dict(id='smart_ai', section='main', title='Smart AI', default=True,
         tagline='Enemies with roles, squad turns and morale.',
         subs=[dict(id='smart_squad', type='check', label='Squad tactics: two close in, the rest hold and shoot',
                    default=True),
               dict(id='smart_morale', type='check', label='Morale: enemies may flee when allies drop', default=True)]),
    dict(id='qol', section='main', title='Bullet Time & Quick Restart', default=True,
         tagline='Bullet time on a key; hold the restart key 1 s to retry the mission.',
         subs=[dict(id='bt_key', type='key', label='Bullet time', default='G'),
               dict(id='restart_key', type='key', label='Quick restart', default='F10')]),
    dict(id='hud', section='main', title='Minimap', default=False,
         tagline='The round radar becomes a hand-inked map of the level, with objective markers at the radar rim and '
                 'in the world.',
         subs=[dict(id='hud_minimap', type='check', default=True,
                    label='Map in the radar (made from your game files: 2-5 minutes, once)'),
               dict(id='hud_rotate', type='check', label='Map turns with the view (off: north up)', default=True,
                    requires='hud_minimap'),
               dict(id='hud_markers', type='check', default=True,
                    label='Objective markers: arrows at the radar rim, diamonds in the world')]),
    dict(id='hud_textures', section='main', title='HD HUD icons', default=True,
         tagline='The pickup coins, ammo icons, weapon cards and the combo skull redrawn in high resolution.',
         subs=[]),
    dict(id='hdfx', section='main', title='HD Effects', default=False,
         tagline='Sharper textures, ambient occlusion, anti-aliasing, sun shafts, real-time shadows. No new art: '
                 'everything is computed from your own game.',
         subs=[dict(id='hd_filtering', type='check', label='Texture filtering (16x anisotropic)', default=True),
               dict(id='hd_ao', type='check', label='Ambient occlusion (contact shadows)', default=True),
               dict(id='hd_post', type='check', label='Post: sharpen, anti-aliasing (FXAA), colour grade', default=True),
               dict(id='hd_shafts', type='check', label='Light shafts, mist and volumetric beams', default=True),
               dict(id='hd_particles', type='check',
                    label='Particles: soft particles, smoke mipmaps, lit smoke, heat haze', default=True),
               dict(id='hd_lighting', type='check',
                    label='Lighting: per-pixel sun, material highlights, reflections, triplanar slopes', default=True),
               dict(id='hd_water', type='check', label='Water: refraction, reflections, waves and foam',
                    default=True, requires='hd_lighting'),
               dict(id='hd_shadows', type='check', label='Real-time shadows (characters, weapons, vehicles)',
                    default=True, requires='hd_lighting'),
               dict(id='hd_static_shadows', type='check',
                    label='Static world shadows too (heavier)', default=False, requires='hd_shadows',
                    experimental=True),
               dict(id='hd_fps_cap', type='check', label='Frame cap 60 fps', default=True)]),
    # ---- MAYHEM (a grid of 3: Physics and the four groups built on its Box3D world first)
    dict(id='physics', section='mayhem', title='Physics', default=False,
         tagline='Box3D: crates, chairs and barrels fly when things blow up.',
         subs=[]),
    dict(id='ragdoll', section='mayhem', title='Ragdoll', default=False, requires=['physics'], experimental=True,
         tagline='Bodies fall the way they were hit: a hit knocks that body part, the dead go limp, blasts throw them.',
         subs=[dict(id='ragdoll_impulse', type='range', label='Drama (push strength)', default=1.0, min=0.5, max=3.0,
                    step=0.25, unit='\u00d7')]),
    dict(id='car_physics', section='mayhem', title='Car Physics', default=False, requires=['physics'],
         experimental=True,
         tagline='Cars shove the props, blasts throw and spin them, wrecks tumble and settle.',
         subs=[dict(id='car_impulse', type='range', label='Drama (push strength)', default=1.0, min=0.5, max=3.0,
                    step=0.25, unit='\u00d7')]),
    dict(id='chain_reactions', section='mayhem', title='Chain Reactions', default=False, requires=['physics'],
         experimental=True,
         tagline='One barrel sets off the next: a blast sets off the explosives around it, nearer ones first.',
         subs=[dict(id='chain_cars', type='check', label='Cars in reach burn and blow up too', default=False)]),
    dict(id='breakable_cover', section='mayhem', title='Breakable Cover', default=False, requires=['physics'],
         experimental=True,
         tagline='Crates, tables, stalls and carts fall apart under fire; enemies behind them stand up.',
         subs=[]),
    dict(id='mayhem_heat', section='mayhem', title='Mayhem Heat', default=False,
         tagline='Chaos builds heat in five levels: the radar glows orange to red, chili peppers catch fire, more enemies '
                 'come.',
         subs=[dict(id='heat_escalation', type='check', label='More enemies with more heat', default=True),
               dict(id='heat_escalation_scale', type='range', label='Reinforcements', default=1.0, min=0.25, max=2.0,
                    step=0.25, unit='\u00d7', requires='heat_escalation'),
               dict(id='heat_decay', type='range', label='Cooling: seconds per level', default=10, min=5, max=30,
                    step=1, unit=' s'),
               dict(id='heat_tint', type='check', label='Radar glows with the heat', default=True),
               dict(id='heat_icons', type='check', label='Chili peppers next to the radar', default=True),
               dict(id='heat_score', type='check', default=True,
                    label='Heat raises your score multiplier (x1.25 at level 2 up to x2 at level 5)')]),
    dict(id='difficulty', section='mayhem', title='Difficulty', default=False,
         tagline="HARD, BRUTAL and PSYCHO in the game's own SETTINGS menu.",
         subs=[dict(id='psycho_loco', type='check', default=True,
                    label='PSYCHO: elite enemies and bosses use loco moves on you (Tornado, El Toro)'),
               dict(id='psycho_drops', type='check', default=True, requires='psycho_loco',
                    label='PSYCHO: a killed elite drops its loco move as a pickup'),
               dict(id='loot_drops', type='check', default=True,
                    label='Every level: enemies drop more ammo, health more often and now and then a power-up'),
               dict(id='loot_ammo', type='range', label='Dropped ammo', default=1.75, min=1.0, max=3.0, step=0.25,
                    unit='×', requires='loot_drops'),
               dict(id='difficulty_all_languages', type='check',
                    label='Also the German / French / Spanish / Italian menu texts', default=False)]),
    dict(id='more_enemies', section='mayhem', title='More Enemies', default=False, requires=['difficulty'],
         tagline='More enemies alive at once on BRUTAL and PSYCHO.',
         subs=[dict(id='max_live_enemies', type='range', label='Enemies at once', default=9, min=6, max=9,
                    step=1)]),
    dict(id='bigger_fights', section='mayhem', title='Bigger Fights', default=False, requires=['difficulty'],
         experimental=True,
         tagline='BRUTAL and PSYCHO: more enemies in every fight from the start, and at heat 3+ enemy cars bring gunmen '
                 '(needs Mayhem Heat). OLD SAVES WILL NOT LOAD: you must start a new game. They stay on disk and '
                 'come back when this is switched off.',
         subs=[]),
    dict(id='gang_wars', section='mayhem', title='Gang Wars', default=False,
         tagline='Rival gangs fight each other when they meet; their kills count for you.',
         subs=[dict(id='gw_provoke', type='check', default=True,
                    label='Gangs at war ignore you until you shoot one of them'),
               dict(id='gw_spark', type='check', default=False,
                    label='Fights start only when you shoot a gang member near a rival')]),
]

# TODGameplay.dll key names (dll/src/gp_logic.cpp kKeys) a key field may write; ESC, MOUSE1 / MOUSE2 and the pad
# buttons are left out (menus, shooting, aiming; pads have their own layout)
KEY_NAMES = (['%d' % d for d in range(10)] + [chr(c) for c in range(ord('A'), ord('Z') + 1)] +
             ['F%d' % n for n in range(1, 13)] +
             ['MINUS', 'EQUALS', 'BACKSPACE', 'TAB', 'LBRACKET', 'RBRACKET', 'RETURN', 'LCONTROL', 'SEMICOLON',
              'APOSTROPHE', 'GRAVE', 'LSHIFT', 'BACKSLASH', 'COMMA', 'PERIOD', 'SLASH', 'RSHIFT', 'MULTIPLY', 'LALT',
              'SPACE', 'CAPSLOCK', 'NUMLOCK', 'SCROLL', 'SUBTRACT', 'ADD', 'DECIMAL', 'NUMPADENTER', 'RCONTROL',
              'DIVIDE', 'RALT', 'PAUSE', 'HOME', 'UP', 'PGUP', 'LEFT', 'RIGHT', 'END', 'DOWN', 'PGDN', 'INSERT',
              'DELETE', 'MOUSE3', 'MOUSE4', 'MOUSE5'] +
             ['NUMPAD%d' % d for d in range(10)])

COMING_SOON = []          # every chip of the first version is a real group now (2026-10-29)


BY_ID = {g['id']: g for g in GROUPS}
MAYHEM = [g['id'] for g in GROUPS if g['section'] == 'mayhem']


def sub_defaults():
    return {s['id']: s['default'] for g in GROUPS for s in g['subs']}


def default_selection():
    return dict(groups={g['id']: g['default'] for g in GROUPS}, subs=sub_defaults())


def normalize(selection):
    """Fill missing groups / subs with defaults, coerce types, enforce dependencies. Returns a new dict."""
    selection = selection or {}
    groups = {g['id']: bool((selection.get('groups') or {}).get(g['id'], g['default'])) for g in GROUPS}
    subs = sub_defaults()
    given = selection.get('subs') or {}
    for g in GROUPS:
        for s in g['subs']:
            if s['id'] not in given:
                continue
            v = given[s['id']]
            if s['type'] == 'check':
                subs[s['id']] = bool(v)
            elif s['type'] == 'file':                  # a path the player picked: kept as typed, checked at install
                if isinstance(v, str):
                    subs[s['id']] = v.strip()[:1024]
            elif s['type'] == 'key':
                v = str(v).strip().upper()
                if v in KEY_NAMES:
                    subs[s['id']] = v
            elif s['type'] == 'range':
                try:
                    v = float(v)
                except (TypeError, ValueError):
                    continue
                v = min(max(v, s['min']), s['max'])
                if isinstance(s['step'], int) and float(s['step']).is_integer():
                    v = int(round(v))
                else:
                    v = round(v, 2)
                subs[s['id']] = v
            else:
                vals = [o[0] for o in s['options']]
                if str(v) in vals:
                    subs[s['id']] = str(v)
    # selections saved by the first version: rumble on/off + strength became one percentage; the vehicle camera
    # moved from the camera tuning card (gone) to Better Driving
    if 'pad_rumble_pct' not in given and ('pad_rumble' in given or 'pad_rumble_strength' in given):
        try:
            pct = 0 if given.get('pad_rumble') is False else round(float(given.get('pad_rumble_strength', 1.0)) * 10) * 10
            subs['pad_rumble_pct'] = int(min(200, max(0, pct)))
        except (TypeError, ValueError):
            pass
    if 'chase_camera' not in given and given.get('vehicle_camera') == 'far' and \
            (selection.get('groups') or {}).get('tuning'):
        subs['chase_camera'] = True
    changed = True
    while changed:                       # a group whose requirement is off is off
        changed = False
        for g in GROUPS:
            if groups[g['id']] and any(not groups[r] for r in g.get('requires', [])):
                groups[g['id']] = False
                changed = True
    return dict(groups=groups, subs=subs)


def public():
    """The catalogue for the UI (no functions)."""
    return dict(groups=GROUPS, coming_soon=COMING_SOON, key_names=KEY_NAMES)
