"""check_suave.py - static checks of the Suave game glue against the class registry and the exe (read only).

1. every MSG row of suave_msgs.h: the negative id resolves, in the receiver class's flat method table, to that command,
   and it is the FIRST entry of the command's group (DispatchMessage 0x48A7E0 reads the handler count there);
2. every raw property id used (0x2A8 FactionID, 0x69D torso_cs) names that property in property_table.json;
3. every member offset the glue reads or writes equals the class registry's offset (and touch slot = member index);
4. the calling conventions of the called functions (callconv.py);
5. the class handlers the glue compares with (+60h of the class object) equal the registry's handler.
Registry: classes.json from the author's reverse-engineering notes (not included; the 399 baked class registrations),
path in env TOD_LOCO_CLASSES.
Exit code 0 = all OK. usage: python -I check_suave.py
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CODE = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(CODE))
CLASSES = os.environ.get('TOD_LOCO_CLASSES', 'classes.json')
PROPS = os.path.join(ROOT, 'data', 'lib', 'property_table.json')

# (class, member name, offset used by suave_game.cpp, touch slot used or None)
MEMBERS = [
    ('character_root', 'Sub_Type', 0x14, None), ('character_root', 'movectrl', 0x48, None),
    ('character_root', 'ActionCtrl', 0x4C, None), ('character_root', 'Animation', 0x50, None),
    ('character_root', 'AICtrl', 0x58, None), ('character_root', 'PhysicCtrl', 0x68, None),
    ('character_root', 'WeaponCtrl', 0x6C, None), ('character_root', 'GoodieCtrl', 0x88, None),
    ('character_root', 'PowerupCtrl', 0x8C, None), ('character_root', 'FaceCtrl', 0x9C, None),
    ('character_sentry_ai', 'Health', 0x2C, None), ('character_sentry_ai', 'DamageTaken', 0x30, None),
    ('character_sentry_ai', 'Current_state', 0x1D4, None), ('character_sentry_ai', 'KnockedOutTimer', 0x264, None),
    ('character_player_ai', 'Health', 0x1C, None), ('character_player_ai', 'DamageTaken', 0x20, None),
    ('character_animation_ctrl', 'CachedActiveCtrl', 0x30, None), ('character_animation_ctrl', 'MoveState', 0x6C, None),
    ('character_animation_ctrl', 'auto_move_to_active', 0xB8, 28), ('character_animation_ctrl', 'auto_move_to_node', 0xBC, 29),
    ('character_animation_ctrl', 'auto_move_to_worldpos', 0xC0, 30),
    ('character_powerup_ctrl', 'pGoodieCtrl', 0x08, None), ('character_powerup_ctrl', 'EffectsCtrl', 0x10, None),
    ('character_powerup_ctrl', 'PowerUpInventory', 0x14, None), ('character_powerup_ctrl', 'CurrentPowerUp', 0x1C, None),
    ('character_goodie_ctrl', 'CurrentGodTime', 0x10, 3),
    ('weapon_shell', 'Ready', 0x1C, None), ('weapon_shell', 'Owner', 0x24, None),
    ('weapon_shell', 'DynamiteBlastRadius', 0x38, None), ('weapon_shell', 'InAim', 0x28, None),
    ('weapon_shell', 'IgnitionEffect', 0x80, None),
    ('camera_sound_ctrl', 'Stereo_track_playing', 0xC4, None),
    ('camera_basic_ai', 'CurrentLogicTarget', 0x08, None),
    ('character_physic_ctrl', 'turn_speed', 0x24, 0), ('character_physic_ctrl', 'TimeStep', 0x84, None),
    ('character_animation_ctrl', 'target_lock_worldpos_active', 0xA4, 25), ('character_animation_ctrl', 'target_lock', 0xA8, 26),
]
HANDLERS = {'character_sentry_ai': 0x5C90F0, 'character_root': 0x5C3060, 'weapon_shell': 0x843D10, 'default_weapon': 0x665630,
            'character_face_ctrl': 0x541A50, 'character_player_ai': 0x5ACE10}
PROPS_USED = {0x2A8: 'FactionID', 0x69D: 'torso_cs'}


def main():
    bad = 0
    C = {c['name']: c for c in json.load(open(CLASSES))}
    txt = open(os.path.join(CODE, 'suave_msgs.h'), encoding='utf-8').read()
    rows = re.findall(r'^MSG\((\w+),\s*"(\w+)",\s*(-?\d+),\s*"([^"]+)"\)', txt, re.M)
    print('== messages (%d)' % len(rows))
    for name, cls, mid, cmd in rows:
        mid = int(mid)
        c = C.get(cls)
        if not c:
            print('BAD  %-22s class %s not in the registry' % (name, cls)); bad += 1; continue
        flat = [m for _, m in sorted(enumerate(c['methods']), key=lambda t: (t[1]['id'], t[0]))]
        i = -2 - mid
        ok = 0 <= i < len(flat) and flat[i]['command'] == cmd
        first = ok and (i == 0 or flat[i - 1]['command'] != cmd)
        handlers = [m['method'] for m in flat if m['command'] == cmd]
        if not (ok and first):
            bad += 1
        print('%s  %-22s %-26s %4d -> %-60s %s' % ('OK ' if ok and first else 'BAD', name, cls, mid,
              flat[i]['command'] if 0 <= i < len(flat) else '?', ('first of %d: %s' % (len(handlers), ', '.join(handlers))) if ok else 'expected ' + cmd))
    print('== members')
    for cls, mem, off, slot in MEMBERS:
        ms = C[cls]['members']
        idx = [k for k, m in enumerate(ms) if m['name'] == mem]
        ok = bool(idx) and ms[idx[0]]['off'] == off and (slot is None or idx[0] == slot)
        bad += not ok
        print('%s  %-26s %-24s +0x%-4X%s' % ('OK ' if ok else 'BAD', cls, mem, off,
              '' if slot is None else ' (touch slot %d)' % slot) + ('' if ok else '  registry: %s' % (
                  [(hex(ms[k]['off']), k) for k in idx] or 'missing')))
    print('== class handlers')
    for cls, h in HANDLERS.items():
        reg = C[cls].get('handler') or ''
        ok = reg.lower().endswith('%08x' % h)
        bad += not ok
        print('%s  %-26s 0x%08X (registry %s)' % ('OK ' if ok else 'BAD', cls, h, reg))
    print('== property ids')
    P = json.load(open(PROPS))['properties']
    for pid, nm in PROPS_USED.items():
        ok = P[pid][0] == nm
        bad += not ok
        print('%s  0x%X = %s' % ('OK ' if ok else 'BAD', pid, P[pid]))
    print('== calling conventions')
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'callconv.py')], capture_output=True, text=True)
    print(r.stdout.rstrip())
    bad += r.returncode != 0
    print('check_suave: %s' % ('ALL OK' if not bad else '%d PROBLEM(S)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
