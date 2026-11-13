"""The TOD Gameplay+ data options: every option is a list of property sets generated from the player's own blocks.

Each option has levels; the first-listed default level is vanilla and produces no change at all. A generator gets the
build context (read-only access to the base blocks the game would load) and returns `Op`s: (block entry, fragment,
node, property, new value, note). Values are computed from the values found in the player's data (x factor, + delta,
or moved along a camera's own line of sight), never copied from a shipped file, so the mod carries no game data.

Sources for every value (the author's reverse-engineering notes, not included):
  driving       subsystem_vehicles.md section 11 (package P1)            VehicleDict rows, VehicleSedan MoveCtrl
  difficulty    subsystem_weapons.md 4.1/4.3/4.6, ai_lab TUNING.md 2.2-2.4 PlayerStatusCtrl, WeaponDict, CharacterDict,
                                                                         BulletDrop
  camera        subsystem_camera.md sections 3, 8, 10                      CharacterPlayerControlled CameraPos
  vehicle cam   subsystem_camera.md section 10 (C5)                        VehicleDict row pos, VehicleSedan rig
  slow motion   subsystem_loco_slowmo.md 7.2, 10.1                         PlayerStatusCtrl stamina values
  cutscenes     subsystem_cutscenes.md 3.4, 4.1                            Trigger_Activate_Camera/_Cutscene Unskipable
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), 'lib')
if LIB not in sys.path:
    sys.path.insert(0, LIB)
import fragedit as fe  # noqa: E402
import todfrag as tf   # noqa: E402

SHARED = 'data_pc/overdose_the_game/overdose/shared/'
MAIN = SHARED + 'overdose.main'
F_VDICT = '/data/Scripts/fragments/VehicleDict.fragment'
F_SEDAN = '/data/Scripts/fragments/VehicleSedan.fragment'
F_ESS = '/data/Scripts/fragments/GameplayEssentials.fragment'
F_WEAP = '/data/Scripts/fragments/WeaponDict.fragment'
F_CHAR = '/data/Scripts/fragments/CharacterDict.fragment'
F_PLAYER = '/data/Scripts/fragments/CharacterPlayerControlled.fragment'


class Op:
    __slots__ = ('entry', 'fragment', 'node', 'prop', 'value', 'old', 'note', 'option', 'level', 'path')

    def __init__(self, entry, fragment, node, prop, value, old, note, path=''):
        self.entry, self.fragment, self.node, self.prop = entry, fragment, node, prop
        self.value, self.old, self.note, self.path = value, old, note, path
        self.option = self.level = None

    def key(self):
        return (self.entry, self.fragment, self.node, self.prop)

    def as_dict(self):
        return dict(option=self.option, level=self.level, block=self.entry.split('/')[-1], fragment=self.fragment,
                    node='0x%08X' % self.node, path=self.path, prop=self.prop, old=self.old, new=self.value,
                    note=self.note)


class OptionError(Exception):
    pass


# ------------------------------------------------------------------ value helpers
def f32(x):
    return tf.f32(x) + 0.0


def rows_of_type(doc, type_prefix):
    return [n for n in doc.created_ids() if (doc.type_of(n) or '').startswith(type_prefix)]


def set_scaled(ops, ctx, entry, frag, node, prop, k, note, add=0.0, minimum=None):
    """Op for value*k+add of an existing number property (skipped when the result equals the value)."""
    if k == 1.0 and add == 0.0 and minimum is None:
        return
    doc = ctx.doc(entry, frag)
    old = doc.get(node, prop)
    if old is None:
        raise OptionError('%s %s has no %s in the game data' % (frag.split('/')[-1], doc.path_of(node), prop))
    if not isinstance(old, float):
        raise OptionError('%s.%s is not a number' % (doc.path_of(node), prop))
    new = f32(float('%.7g' % old) * k + add)   # scale the authored decimal (1.3 x 0.9 = 1.17), not its f32 noise
    if minimum is not None:
        new = max(new, f32(minimum))
    if new != f32(old):
        ops.append(Op(entry, frag, node, prop, new, old, note, doc.path_of(node)))


# ------------------------------------------------------------------ 1. driving feel (vehicles P1)
P1_TURN_RATE = 0.12          # rad per 0.04 s step; shell registration default 0.174 (code), not set in data
SHELL_TURN_RATE_DEFAULT = 0.174
P1_SKIP_TYPES = {'Dirtbike', 'Chopper', 'AITest', 'Door_Left_Front'}


def gen_driving(ctx, level):
    ops = []
    if level == 'vanilla':
        return ops
    sedan = ctx.doc(MAIN, F_SEDAN)
    mc = sedan.find('MoveCtrl')
    if not (sedan.type_of(mc) or '').startswith('car_move_ctrl'):
        raise OptionError('VehicleSedan MoveCtrl is not a car_move_ctrl')
    cur = sedan.get(mc, 'turn_rate')
    base = SHELL_TURN_RATE_DEFAULT if cur is None else cur
    new = f32(base * (P1_TURN_RATE / SHELL_TURN_RATE_DEFAULT))
    if cur is None or new != f32(cur):
        ops.append(Op(MAIN, F_SEDAN, mc, 'turn_rate', new, cur if cur is not None else 'default 0.174 (code)',
                      'steering ramp per step: full lock in 0.28 s instead of 0.20 s', sedan.path_of(mc)))
    vd = ctx.doc(MAIN, F_VDICT)
    for n in rows_of_type(vd, 'database_vehicle_type'):
        name = vd.get(n, 'name')
        if name in P1_SKIP_TYPES:
            continue
        set_scaled(ops, ctx, MAIN, F_VDICT, n, 'cornering_stiffness', 0.9,
                   'x0.9: steering lasts to higher speed (Sedan 90 -> 100 km/h)')
        set_scaled(ops, ctx, MAIN, F_VDICT, n, 'control_speed', 0.5,
                   'x0.5: a held power slide spins out after ~1.4 s instead of ~0.8 s')
    return ops


# ------------------------------------------------------------------ 2. difficulty presets
# factors per preset. dmg_taken: PlayerStatus DamageEasy/Normal/Hard (x, so the in-game Easy/Normal/Hard choice keeps
# its ratio); dmg_dealt: PlayerDamageMod (damage of the player's shots on NPCs = 1 / enemy health); spread: enemy
# SpreadEnemy_m of the ten firearms (('add', deg) or ('mul', k)); react: AI Wait_time_from/to; pause: enemy pause
# between bursts Min/MaxPauseTime; ammo: dropped-gun ammo bands (BulletDrop *DropFrom/*DropTo, percent of a clip).
DIFFICULTY = {
    'easy':   dict(dmg_taken=0.6, dmg_dealt=1.5, spread=('add', 1.5), react=1.5, pause=1.5, ammo=1.5),
    'normal': dict(dmg_taken=1.0, dmg_dealt=1.0, spread=('mul', 1.0), react=1.0, pause=1.0, ammo=1.0),
    'hard':   dict(dmg_taken=1.4, dmg_dealt=0.8, spread=('mul', 0.6), react=0.7, pause=0.75, ammo=0.75),
    'brutal': dict(dmg_taken=2.0, dmg_dealt=0.6, spread=('mul', 0.25), react=0.5, pause=0.5, ammo=0.5),
}
FIREARM_IDS = set(range(10))           # Beretta Magnum Marlin Winchester Benelli Hudson Mac10 MP5 AK47 M16
PLAYER_MODEL_TYPES = {20, 10, 18}      # Ram, Ernesto, Tommy (database_model_stats Model_type)
PLAYER_CHAR_NAMES = {'Player'}         # database_character_type row of the player
DROP_BANDS = ['VeryFewDropFrom', 'VeryFewDropTo', 'FewDropFrom', 'FewDropTo', 'AverageDropFrom', 'AverageDropTo',
              'ManyDropFrom', 'ManyDropTo', 'VeryManyDropFrom', 'VeryManyDropTo']


def gen_difficulty(ctx, level):
    ops = []
    f = DIFFICULTY[level]
    ess = ctx.doc(MAIN, F_ESS)
    ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
    for p in ('DamageEasy', 'DamageNormal', 'DamageHard'):
        set_scaled(ops, ctx, MAIN, F_ESS, ps, p, f['dmg_taken'], 'damage Ram takes x%g' % f['dmg_taken'])
    set_scaled(ops, ctx, MAIN, F_ESS, ps, 'PlayerDamageMod', f['dmg_dealt'],
               'damage of Ram\'s hits on NPCs x%g (enemy health x%.3g)' % (f['dmg_dealt'], 1 / f['dmg_dealt']))
    wd = ctx.doc(MAIN, F_WEAP)
    mode, val = f['spread']
    for n in rows_of_type(wd, 'database_weapon_subtype'):
        if wd.get(n, 'Specific_ID') not in FIREARM_IDS:
            continue
        if mode == 'add':
            set_scaled(ops, ctx, MAIN, F_WEAP, n, 'SpreadEnemy_m', 1.0, 'enemy spread +%g deg' % val, add=val)
        else:
            set_scaled(ops, ctx, MAIN, F_WEAP, n, 'SpreadEnemy_m', val, 'enemy spread x%g' % val)
        for p in ('MinPauseTime', 'MaxPauseTime'):
            set_scaled(ops, ctx, MAIN, F_WEAP, n, p, f['pause'], 'enemy pause between bursts x%g' % f['pause'])
    cd = ctx.doc(MAIN, F_CHAR)
    for n in rows_of_type(cd, 'database_character_type') + rows_of_type(cd, 'database_model_stats'):
        ty = cd.type_of(n)
        if ty.startswith('database_model_stats') and cd.get(n, 'Model_type') in PLAYER_MODEL_TYPES:
            continue
        if ty.startswith('database_character_type') and cd.get(n, 'name') in PLAYER_CHAR_NAMES:
            continue
        for p in ('Wait_time_from', 'Wait_time_to'):
            if cd.get(n, p) is not None:
                set_scaled(ops, ctx, MAIN, F_CHAR, n, p, f['react'], 'AI reaction pause x%g' % f['react'])
    bd = ess.find('MasterCtrlAux/BulletDrop')
    for p in DROP_BANDS:
        set_scaled(ops, ctx, MAIN, F_ESS, bd, p, f['ammo'], 'ammo in dropped guns x%g' % f['ammo'])
    return ops


# ------------------------------------------------------------------ 3. camera distance (player) and vehicle camera
CAMERA_BACK = {'near': -0.5, 'default': 0.0, 'far': 0.75, 'farther': 1.5}
VEHICLE_CAMERA_BACK = {'default': 0.0, 'far': 1.5}


def line_of_sight(q):
    """camera view direction of a node with stored quaternion q (stored = conjugate, project convention)."""
    return fe.qrot(fe.qconj(fe.qnorm(tuple(q))), (0.0, 0.0, 1.0))


def moved_back(pos, orient, back):
    f = line_of_sight(orient)
    return [f32(pos[i] - back * f[i]) for i in range(3)]


def move_node_ops(ctx, entry, frag, node, back, note):
    doc = ctx.doc(entry, frag)
    p0 = doc.get(node, 'pos')
    q0 = doc.get(node, 'orient') or [0.0, 0.0, 0.0, 1.0]
    if p0 is None:
        raise OptionError('%s has no pos' % doc.path_of(node))
    p = moved_back(p0, q0, back)
    if p == [f32(x) for x in p0]:
        return []
    return [Op(entry, frag, node, 'pos', p, p0, note, doc.path_of(node))]


def gen_camera(ctx, level):
    back = CAMERA_BACK[level]
    if not back:
        return []
    d = ctx.doc(MAIN, F_PLAYER)
    ops = []
    for nm in ('CameraPos', 'AdjustedPivot'):
        n = d.find('Animation/CameraCtrl/' + nm)
        ops += move_node_ops(ctx, MAIN, F_PLAYER, n, back, 'player camera %+.2f m along its line of sight' % back)
    return ops


def gen_vehicle_camera(ctx, level):
    back = VEHICLE_CAMERA_BACK[level]
    if not back:
        return []
    ops = []
    vd = ctx.doc(MAIN, F_VDICT)
    for n in rows_of_type(vd, 'database_vehicle_type'):
        if vd.get(n, 'name') in ('AITest', 'Door_Left_Front'):
            continue
        ops += move_node_ops(ctx, MAIN, F_VDICT, n, back, 'vehicle camera %+.2f m along its line of sight' % back)
    sd = ctx.doc(MAIN, F_SEDAN)
    for nm in ('CameraPos', 'AdjustedPivot'):
        n = sd.find('CameraCtrl/' + nm)
        ops += move_node_ops(ctx, MAIN, F_SEDAN, n, back, 'car shell rig %+.2f m (reset copies the type value)' % back)
    return ops


# ------------------------------------------------------------------ 5. slow motion and its meter
SLOWMO_COST = {'short': 2.0, 'default': 1.0, 'long': 0.5, 'very_long': 0.25}
SLOWMO_REGEN = {'slow': 0.5, 'default': 1.0, 'fast': 2.0}


def gen_slowmo_length(ctx, level):
    k = SLOWMO_COST[level]
    if k == 1.0:
        return []
    ess = ctx.doc(MAIN, F_ESS)
    ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
    ops = []
    set_scaled(ops, ctx, MAIN, F_ESS, ps, 'StaminaCostShootdodge', k,
               'dive slow-motion drain x%g (full meter lasts %g s instead of 8 s)' % (k, 8 / k))
    return ops


def gen_slowmo_regen(ctx, level):
    k = SLOWMO_REGEN[level]
    if k == 1.0:
        return []
    ess = ctx.doc(MAIN, F_ESS)
    ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
    ops = []
    for p in ('StaminaGainStanding', 'StaminaGainRunning'):
        set_scaled(ops, ctx, MAIN, F_ESS, ps, p, k, 'slow-motion meter refill x%g' % k)
    return ops


# ------------------------------------------------------------------ 4. cutscenes: let the locked camera sequences be skipped
# Movies are skippable in vanilla (no Trigger_Activate_Cutscene has Unskipable). Camera
# sequences with Unskipable shots live in these 22 mission blocks (full scan of the 696 GOG blocks: the other hits are
# the Point_Challenge intros inside 7 submaps, left alone because a submap block under TOD HD is 100-190 MB).
UNSKIP_BLOCKS = [
    'intermission_after_jungle.mission', 'mm010_ernestocruz.mission', 'mm013_tommycruz.mission',
    'mm030_smashthestash.mission', 'mm066_brakeintosafe.mission', 'mm075_hacienda.mission',
    'mm080_highwaybridge.mission', 'mm090_getmilitaryinfo.mission', 'mm092_escapejungle.mission',
    'mm100_gettothedeahq.mission', 'mm110_deahqvault.mission', 'mm120_runawaytrain.mission',
    'subm_05_down_teqsal_kill.mission', 'subm_06_indu_chepoi_race.mission', 'subm_07_indu_mulral01_race.mission',
    'subm_08_down_7gates01_race.mission', 'subm_12_indu_mulral02_race.mission', 'subm_15_harb_pictru_racekill.mission',
    'subm_18_indu_7gates02_race.mission', 'subm_21_down_mulral03_race.mission', 'subm_30_harb_7gates03_race.mission',
    'subm_33_harb_mulral04_race.mission']
UNSKIP_TYPES = ('Trigger_Activate_Camera', 'Trigger_Activate_Cutscene')


def gen_cutscenes(ctx, level):
    if level == 'vanilla':
        return []
    ops = []
    for b in UNSKIP_BLOCKS:
        entry = SHARED + b
        for frag in ctx.fragment_names(entry):
            doc = ctx.doc(entry, frag)
            for n in doc.created_ids():
                if not (doc.type_of(n) or '').startswith(UNSKIP_TYPES):
                    continue
                v = doc.get(n, 'Unskipable')
                if v is True:
                    ops.append(Op(entry, frag, n, 'Unskipable', False, True,
                                  'sequence can be skipped with Return / Escape / Space', doc.path_of(n)))
    return ops


# ------------------------------------------------------------------ 9. the Suave loco move (gameplay/suave_data.py)
F_TEXDICT = '/data/Scripts/fragments/TextureDict.fragment'


def gen_suave(ctx, level):
    """Set ops of the `suave` option; the uk texts, stream records and oggs are added by builder.build_suave_data."""
    if level in ('off', 'on_slot10'):   # on_slot10: the Pinata keeps its own icon; only the uk texts change
        return []
    from . import suave_data as sd
    ops = []
    td = ctx.doc(MAIN, F_TEXDICT)
    n = td.find(sd.ICON_ENTITY)
    old = td.get(n, 'texture')
    if old != sd.ICON_OLD:
        raise OptionError('%s.texture is %r (expected the shipped %s): another mod changed it' % (sd.ICON_ENTITY, old, sd.ICON_OLD))
    ops.append(Op(MAIN, F_TEXDICT, n, 'texture', sd.ICON_NEW, old,
                  'slot 11 HUD icon STAND-IN (the Time Stop goodie icon; Tequila shared the small-health icon)', td.path_of(n)))
    if level == 'on_start2':
        ess = ctx.doc(MAIN, F_ESS)
        ps = ess.find('MasterCtrlAux/PlayerStatusCtrl')
        cur = ess.get(ps, 'Default_Tequillas')
        mx = ess.get(ps, 'Default_Max_Tequillas')
        if cur is None or mx is None or mx < 2:
            raise OptionError('PlayerStatusCtrl Default_Tequillas / Default_Max_Tequillas missing or max < 2')
        if cur != 2:
            ops.append(Op(MAIN, F_ESS, ps, 'Default_Tequillas', 2, cur, 'a new game starts with 2 Suave charges (slot 11)',
                          ess.path_of(ps)))
    return ops


# ------------------------------------------------------------------ 10. seamless map borders (gameplay/seamless.py)
def gen_seamless(ctx, level):
    """Set ops of the `seamless` option: Teleport_to emptied on the corridor loaders, one box moved per corridor."""
    from . import seamless as sm
    return sm.gen_seamless(ctx, level)

# ------------------------------------------------------------------ 11. controller menu rows (DLL [pad])
F_HUD = '/data/Scripts/fragments/HUD.fragment'
VIBRATION_ENTRY = 'InterMission_HUD/OptionSettingsMenu/Settings/vibration'
# "To confirm Screen Resolution press [Enter]" (uk_Menu #351), shown below the SETTINGS rows while SCREEN RESOLUTION
# has the focus. Its place is data (HUD.fragment TextBox, 800x600 HUD units, not code positioned): y 450 sits right
# under the vanilla last row; with the VIBRATION row the list is one row (25 units) longer and the last row's text
# ends at about 450. 466 keeps a gap under that row and stays inside the menu box (its bottom border at about 490).
SCREENRES_HINT = 'InterMission_HUD/GfxFolder/ScreenResDetailTextTB'
SCREENRES_HINT_Y = (450.0, 466.0)   # vanilla, with the VIBRATION row


def gen_controller(ctx, level):
    """Set ops of the `controller` option: the console-only VIBRATION row of SETTINGS shown on PC (NotPcOption
    true -> false), and the screen-resolution hint under the rows moved down one notch so the longer list does not
    run into it. Existing records only; the game already stores the value (Setting_Allow_vibration) and its own
    rumble controller already checks it (command_set_rumble 0x787200)."""
    if level == 'off':
        return []
    doc = ctx.doc(MAIN, F_HUD)
    n = doc.find(VIBRATION_ENTRY)
    if n is None:
        raise OptionError('%s: no %s' % (F_HUD, VIBRATION_ENTRY))
    old = doc.get(n, 'NotPcOption')
    if old is None:
        raise OptionError('%s has no NotPcOption in the game data' % VIBRATION_ENTRY)
    ops = []
    if old is True:
        ops.append(Op(MAIN, F_HUD, n, 'NotPcOption', False, old,
                      'VIBRATION row shown on PC (pad rumble on/off; DLL [pad])', doc.path_of(n)))
    h = doc.find(SCREENRES_HINT)
    p0 = doc.get(h, 'pos') if h is not None else None
    if p0 is None or len(p0) != 3:
        raise OptionError('%s: no %s with a position' % (F_HUD, SCREENRES_HINT))
    y0, y1 = SCREENRES_HINT_Y
    if abs(p0[1] - y0) < 1e-3:
        ops.append(Op(MAIN, F_HUD, h, 'pos', [f32(p0[0]), f32(y1), f32(p0[2])], p0,
                      'screen-resolution hint %g -> %g (HUD units): below the one-row-longer SETTINGS list' % (y0, y1),
                      doc.path_of(h)))
    elif abs(p0[1] - y1) >= 1e-3:
        raise OptionError('%s y = %r: neither vanilla %g nor %g (another mod moved it?)' % (SCREENRES_HINT, p0[1], y0, y1))
    return ops


# ------------------------------------------------------------------ 12. difficulty in the game's menu (gameplay/menus.py)
def gen_difficulty_menu(ctx, level):
    """Set op of the `difficulty_menu` option: the DIFFICULTY LEVEL entry's OptionValueType4 (never read by the game)
    carries the BRUTAL text ordinal for the DLL ([difficulty] appends the value at run time; without the DLL the entry
    keeps its three vanilla values). The text itself is replaced by builder.build_menu_texts (menus.TEXT_EDITS)."""
    if level == 'off':
        return []
    from . import menus as mn
    path = mn.SETTINGS + '/difficulty'
    vals = mn.entry_values(ctx, path)
    free = all(vals[k][0] in (None, 0, -1) and vals[k][1] in (None, -1) for k in (3, 4))
    if [v for v, _ in vals[:3]] != [160, 161, 162] or not free:
        raise OptionError('%s does not hold the vanilla EASY / NORMAL / HARD values with free slots 4 and 5: %r' % (path, vals))
    ops = [mn.entry_op(ctx, path, 'OptionValueType4', mn.BRUTAL_ORDINAL,
                       'marker for TODGameplay.dll [difficulty]: BRUTAL = Menu text #%d (ValueString4 stays 0: no fourth value '
                       'without the DLL)' % mn.BRUTAL_ORDINAL, Op),
           mn.entry_op(ctx, path, 'OptionValueType5', mn.PSYCHO_ORDINAL,
                       'marker for TODGameplay.dll [difficulty]: PSYCHO = Menu text #%d (ValueString5 stays 0)' % mn.PSYCHO_ORDINAL, Op)]
    return [op for op in ops if op]


# ------------------------------------------------------------------ the catalogue (apply order = this order)
OPTIONS = [
    dict(key='driving', levels=['vanilla', 'better_feel'], gen=gen_driving,
         help='better_feel = vehicles package P1 (steering ramp 0.12, cornering x0.9, slide control x0.5)'),
    dict(key='difficulty', levels=['normal', 'easy', 'hard', 'brutal'], gen=gen_difficulty,
         help='normal = vanilla; see DIFFICULTY for every factor'),
    dict(key='camera_distance', levels=['default', 'near', 'far', 'farther'], gen=gen_camera,
         help='player camera moved along its own line of sight: near -0.5 m, far +0.75 m, farther +1.5 m'),
    dict(key='vehicle_camera', levels=['default', 'far'], gen=gen_vehicle_camera,
         help='far = every vehicle camera 1.5 m further back'),
    dict(key='slowmo_length', levels=['default', 'short', 'long', 'very_long'], gen=gen_slowmo_length,
         help='dive bullet-time drain x2 / x0.5 / x0.25 (full meter 4 / 16 / 32 s instead of 8 s)'),
    dict(key='slowmo_regen', levels=['default', 'slow', 'fast'], gen=gen_slowmo_regen,
         help='meter refill x0.5 / x2'),
    dict(key='cutscenes', levels=['vanilla', 'unlock_all'], gen=gen_cutscenes,
         help='unlock_all = clear Unskipable on the in-engine camera sequences of 22 mission blocks (experimental)'),
    # suave_anims: no set ops; builder.build_suave appends clip records + AnimDict slots after the set options
    dict(key='suave_anims', levels=['off', 'on'], gen=lambda ctx, level: [],
         help='on = the Suave loco move clips (22 new records: intro/grabbed, lead/follow, push/stumble, lookdown, circle) + AnimDict slots PlayerAnims/specials/Suave/* '
              '(appended, save-safe; see gameplay/suave_anims.py); played by the gameplay DLL'),
    # suave: set ops here; builder.build_suave_data adds the uk texts, music stream records and the oggs
    dict(key='suave', levels=['off', 'on', 'on_start2', 'on_slot10'], gen=gen_suave,
         help='on = slot 11 named "Suave" on the HUD, its stand-in icon, the 4 music segments (from the build folder '
              'of suave/audio/song_import.py or make_segments.py, unless suave_music = off); on_start2 = also 2 charges on a new game; on_slot10 = '
              'the Pinata slot (10) labelled Suave for the in-game test with the DLL''s [suave] slot = 10'),
    # no set ops: builder.build_suave_data skips the music stream records and oggs (the player gave no copy of the song)
    dict(key='suave_music', levels=['on', 'off'], gen=lambda ctx, level: [],
         help='off = the suave option without its music: no stream records, no oggs (needs no song file); pair with '
              'TODGameplay.ini [suave] music = 0. Only read when suave is not off'),
    # seamless: set ops only (Teleport_to and one root pos per corridor); builder.build_seamless_check re-scans the result
    dict(key='seamless', levels=['off', 'on', 'on_nomarkers'], gen=gen_seamless,
         help='on = the corridor teleports become walk borders: the Downtown-Harbour bridge and the Downtown 02 - '
              'Industrial 04/02 tunnel (Teleport_to emptied, one box moved 23 m past the other; the farm pass stays '
              'a teleport: the two farm blocks do not share its ground); on_nomarkers = also hides the loader beams and area signs of every walk border (zero size, opacity 0; farm teleport, ZAP and mission markers kept). Pair with the DLL [seamless] section'),
    # controller: two set ops (existing menu records: the VIBRATION row, the screen-resolution hint's place);
    # pair with the DLL [pad] section
    dict(key='controller', levels=['off', 'menu'], gen=gen_controller,
         help='menu = the VIBRATION row (ON / OFF) of SETTINGS is shown on PC (it was console only); the game stores it '
              'and its own rumble obeys it; the "To confirm Screen Resolution" hint moves down below the longer list. '
              'Pair with the DLL [pad] section'),
    # difficulty_menu: one marker set op here; builder.build_menu_texts replaces Menu text #77 with BRUTAL per language block
    dict(key='difficulty_menu', levels=['off', 'on', 'on_all_languages'], gen=gen_difficulty_menu,
         help='on = BRUTAL and PSYCHO as values 4 and 5 of DIFFICULTY LEVEL in the SETTINGS menu, chosen in game (needs '
              'TODGameplay.dll [difficulty] enabled = 1; the uk Menu texts #77 FILL_IN / #101 DEBUG COMPLETE become BRUTAL / PSYCHO); '
              'on_all_languages = the same texts in the de / fr / es / it blocks too. Keep the static option difficulty = normal with it: the DLL applies the '
              'hard / brutal factors at run time'),
    # bigger_fights: no set ops; builder.build_bigger_fights adds nodes to overdose.main and the mission blocks. BREAKS SAVES
    # on purpose (gameplay/bigger_fights.py); pair with the DLL [bigger_fights] section, which keeps separate save slots
    dict(key='bigger_fights', levels=['off', 'on'], gen=lambda ctx, level: [],
         help='on = more enemies at the start of a fight on BRUTAL / PSYCHO and vehicles with gunmen at heat 3+: more pooled '
              'characters, weapons and wear, dormant extra placements in the mission blocks (gameplay/bigger_fights.py). '
              'Saves made without it do not fit: a new game is needed; needs TODGameplay.dll [bigger_fights] enabled = 1'),
    # hud_textures: no set ops; builder.build_hud_textures runs LAST and swaps texture records + payloads in
    # overdose.main (gameplay/hud_textures.py; art from hud_textures/make_hud_textures.py). Save-safe: nothing added.
    dict(key='hud_textures', levels=['off', 'on'], gen=lambda ctx, level: [],
         help='on = HD HUD icons: the pickup coins, ammo icons and bullet tick (256 x 256 / 128 x 256), the 28 weapon '
              'selector cards (512 x 256) and the combo timer strip (128 x 512), RGBA32 with mips, in place of the '
              'originals in overdose.main (per-texture list: hud_textures/textures.json, enabled)'),
]
BY_KEY = {o['key']: o for o in OPTIONS}


def defaults():
    return {o['key']: o['levels'][0] for o in OPTIONS}
