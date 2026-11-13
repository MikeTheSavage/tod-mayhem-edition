// psycho_game.cpp - game glue of PSYCHO's enemy loco moves ([psycho], TODGameplay.dll). Reads the characters around
// Ram, runs ps::pick / ps::Active (psycho_logic.cpp, pure, unit tested) and turns their decisions into the game's own
// calls on the ENEMY: its MoveCtrl plays Ram's special-move clips (motion 5Eh, as the Tornado / El Toro do on Ram), its
// gun fires blanks through its own weapon_effects_ctrl, the game's power-up particles and the loco-move sting are the
// telegraph, and the hit is character_root take_a_hit on RAM with the enemy as owner (so the enemy's Damage_mod and the
// player's difficulty factor apply, as for a bullet). Nothing global is touched: no time scale, no camera, no HUD, no
// power-up inventory. Off unless [psycho] enemy_loco_moves = 1 AND the difficulty is PSYCHO ([difficulty]).
// Rules as in dll/src/gp_game.cpp: every referenced address byte-checked (sites.h REF rows), every game read
// SEH-guarded, no code site patched (it runs in the input tick and the post-input hook other features already use).
// Evidence for every call: dll/HOOKS.md section "PSYCHO enemy loco moves" and psycho/README.md.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../dll/src/gp_glue.h"
#include "psycho_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v)  (*(volatile float*)(uintptr_t)(a) = (float)(v))

// ================================================================================================ game addresses
static const DWORD A_PLAYER_HOLDER = 0xA3B590, A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_LIST_INT = 0xA3BB04;
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_SCENE = 0xA3DCBC;
static const DWORD P_PS_SENDCMD = 0x86A340, P_PS_NATIVE = 0x86A250, P_PS_GETPROP = 0x86A6A0, P_PS_GETMEMBER = 0x869E80,
                   P_PS_QUERY = 0x486950, P_PS_LISTFREE = 0x8718D0, P_PS_GETPOS = 0x88CA80, P_PS_L2W = 0x88C9C0,
                   P_PS_SETPOS = 0x88E600, P_PS_SETROTY = 0x891360, P_PS_FINDCHILD = 0x88EB60, P_PS_LAYERSPEED = 0x5010A0,
                   P_PS_OCCLUDED = 0x91D020, P_PS_STOPEMIT = 0x8EB580, P_PS_KILLEMIT = 0x8EB540;
// script cache data offsets
static const DWORD C_MASTER = 0x254, C_MISSION = 0x25C, C_FACTIONS = 0x278, C_CLOCK = 0x274, C_SOUNDDICT = 0x3D8,
                   C_PARTICLES = 0x3F0, C_ANIM_LOCO = 0x73C, C_ANIM_TORO_IDLE = 0x740, C_ANIM_TORO_RUN = 0x744,
                   C_EXCLUSIVE = 0x864;
// character_root data
static const DWORD R_SUBTYPE = 0x14, R_FACTION = 0x18, R_MOVE = 0x48, R_ANIM = 0x50, R_AI = 0x58, R_PHYS = 0x68,
                   R_WEAPON = 0x6C, R_POWERUP = 0x8C;
// class handlers (script class object +60h, registrations in sites.h)
static const DWORD H_SENTRY = 0x5C90F0, H_ROOT = 0x5C3060, H_PLAYER_AI = 0x5ACE10, H_WEAPON = 0x665630;
// message ids (negative = class method table entry; same rows as suave/code/suave_msgs.h, checked by check_suave.py)
static const int MSG_MOVE_NEW_STATE = -36;     // character_animation_ctrl Request_new_State(integer,integer)
static const int MSG_MOVE_IDLE = -81;          // character_animation_ctrl command_idle
static const int MSG_AI_NO_CONTROL = -23;      // character_sentry_ai command_no_ai_control
static const int MSG_AI_REGAIN = -9;           // character_sentry_ai command_regain_control
static const int MSG_ROOT_TAKE_A_HIT = -2;     // character_root take_a_hit(number,entity,entity,entity,vector,vector,integer)
static const int MSG_PHYS_NO_COLLISION = -17;  // character_physic_ctrl command_disable_collision
static const int MSG_PHYS_COLLISION = -11;     // character_physic_ctrl command_enable_collision
static const int MSG_PLAY_SPEAK = -15;         // camera_sound_ctrl command_Play_Speak(Entity,Entity)
static const int MSG_GUN_STOP_FIRING = -12;    // default_weapon command_stop_firing
// global command ids (Node::SendCommand 0x86A340, command table)
static const int CMD_TRIGGER_ENTITY = 247;     // ParticleSystem Trigger(entity) (take_a_hit's blood 0x5B76FC)
static const int CMD_FIRE_BLANK = 1266;        // weapon_effects_ctrl command_fire_blank_shot(truth) (Tornado 0x535CB5)
static const int NAT_MOTION = 0x5E;            // MotionLayer motion(slot, event) via CallNative (Tornado 0x535914)
static const int PROP_EFFECTCTRL = 0x6C0;      // a gun's EffectCtrl (Tornado 0x535C90)
static const int PROP_FACTION = 0x2A8, PROP_TORSO = 0x69D;
static const int PROP_ISVIP = 642;             // CharacterPlaceHolder IsVIP (the same builtin gang wars reads)
static const DWORD R_PLACEHOLDER = 0x24;       // character_root data: its character_placeholder (as gp_difficulty.cpp)
static const DWORD S_SUBTYPE = 0x28;           // sentry data: sub type (4 escort, 7 player, 8 never aggressive, 9 civilian)

// ================================================================================================ config
static ps::Cfg g_c;
static bool  g_verbose = false, g_installed = false, g_force_psycho = false;
static int   g_flash_fx = 30, g_glow_fx = 180, g_sound_id = -1;    // PickupPowerups_Trigger_Red, Goodie_Glow_Orange, no sting
                                                                     // (107 Loco_Move_Serious is RAM's voice: refused)
static bool  g_sound_speaker = false, g_glow_kill = false, g_toro_nocollide = true;
static float g_toro_sink = 0.5f;               // the charging enemy is put back on its start height when it drops this far
static float g_scan_radius = 20.0f, g_blank_volume_dummy = 0.0f;
static int   g_debug_start_key = gp::KEY_NONE;   // test copy: the nearest qualifying enemy starts a move now
static int   g_debug_dodge = 0;                  // test copy: 1 = Ram shoot-dodges sideways out of every move, 2 = every second
static int   g_dodge_left = 0x1E /*A*/, g_dodge_right = 0x20 /*D*/, g_dodge_key = 0x39 /*SPACE*/;
static float g_debug_dodge_lead = 0.15f;         // s before the spin starts / the bull arrives
static int   g_debug_move = 0;                   // test copy: force this move (1 tornado, 2 toro) for debug starts
static float g_perf_log = 10.0f;
static float g_debug_heal = 0.0f;               // test copy: Ram's DamageTaken back to 0 when his health falls below this fraction
static bool  g_debug_face = false;              // test copy: Ram turns toward a telegraphing enemy (the camera follows: the clip sees it)
static bool  g_debug_marks = false;             // test copy: "debug keys: mark shot ..." lines for the harness at every phase
static int   g_debug_interrupt = 0;             // test copy: 1 = Ram's shot (take_a_hit, owner Ram) hits the enemy 0.4 s into every
                                                // wind-up the debug dodge does not cover (the cancel path, without aiming)
static bool  g_interrupt_armed = false;
static float g_watch_seconds = 30.0f;
static float g_debug_rewind = 0.0f;             // test copy: this many s after a move's hand-back the enemy's AI is switched off
                                                // again (what a rewind restores), so the watchdog's hand-back can be seen          // [psycho] handback_watch_seconds (the hand-back watchdog, see hand_back())
static int   g_reasserts = 0;                   // no_ai_control re-sent during this move

static bool  b_cfg(const char* k, bool d) { return gp_ini().geti("psycho", k, d ? 1 : 0) != 0; }
static float f_cfg(const char* k, float d) { return gp_ini().getf("psycho", k, d); }
static void drops_load_config();
void psycho_load_config() {
    ps::Cfg c;
    c.enabled = b_cfg("enemy_loco_moves", false);
    const char* mv = gp_ini().get("psycho", "moves");
    if (mv) { unsigned m = ps::parse_moves(mv); c.moves = m; }
    const char* cd = gp_ini().get("psycho", "cooldown");
    if (cd) ps::parse_range(cd, &c.cooldown_min, &c.cooldown_max);
    c.telegraph = f_cfg("telegraph_seconds", c.telegraph);
    c.elite_only = b_cfg("elite_only", c.elite_only);
    c.elite_health = f_cfg("elite_health", c.elite_health); c.boss_health = f_cfg("boss_health", c.boss_health);
    c.bosses = b_cfg("bosses", c.bosses);
    g_watch_seconds = f_cfg("handback_watch_seconds", 30.0f); if (g_watch_seconds < 0.0f) g_watch_seconds = 0.0f; if (g_watch_seconds > 300.0f) g_watch_seconds = 300.0f;
    c.chance = f_cfg("chance", c.chance); c.check_interval = f_cfg("check_interval", c.check_interval);
    c.first_delay = f_cfg("first_delay", c.first_delay); c.global_gap = f_cfg("global_gap", c.global_gap);
    c.retry_delay = f_cfg("retry_delay", c.retry_delay);
    c.interrupt_damage = f_cfg("interrupt_damage", c.interrupt_damage); c.interrupt_cooldown = f_cfg("interrupt_cooldown", c.interrupt_cooldown);
    c.dive_dodges = b_cfg("dive_dodges", c.dive_dodges); c.recover = f_cfg("recover_seconds", c.recover);
    c.tornado_min = f_cfg("tornado_min_distance", c.tornado_min); c.tornado_max = f_cfg("tornado_max_distance", c.tornado_max);
    c.tornado_radius = f_cfg("tornado_radius", c.tornado_radius); c.tornado_damage = f_cfg("tornado_damage", c.tornado_damage);
    c.tornado_hit_interval = f_cfg("tornado_hit_interval", c.tornado_hit_interval);
    c.tornado_fire_speed = f_cfg("tornado_fire_speed", c.tornado_fire_speed);
    c.tornado_blank_interval = f_cfg("tornado_blank_interval", c.tornado_blank_interval);
    c.toro_min = f_cfg("toro_min_distance", c.toro_min); c.toro_max = f_cfg("toro_max_distance", c.toro_max);
    c.toro_speed = f_cfg("toro_speed", c.toro_speed); c.toro_overrun = f_cfg("toro_overrun", c.toro_overrun);
    c.toro_max_seconds = f_cfg("toro_max_seconds", c.toro_max_seconds); c.toro_hit_radius = f_cfg("toro_hit_radius", c.toro_hit_radius);
    c.toro_damage = f_cfg("toro_damage", c.toro_damage); c.toro_impulse = f_cfg("toro_impulse", c.toro_impulse);
    c.clamp();
    g_c = c;
    g_verbose = b_cfg("log_verbose", false);
    g_flash_fx = gp_ini().geti("psycho", "flash_effect", 30); g_glow_fx = gp_ini().geti("psycho", "glow_effect", 180);
    int snd = gp_ini().geti("psycho", "sound", -1);
    g_sound_id = ps::sting_sound(snd);
    if (snd >= 0 && g_sound_id < 0) gp_log("psycho: sound = %d is one of Ram's own voice lines: refused, enemy moves play no sting\n", snd);
    g_sound_speaker = b_cfg("sound_speaker", false); g_glow_kill = b_cfg("glow_kill", false);
    g_toro_nocollide = b_cfg("toro_collision_off", true); g_toro_sink = f_cfg("toro_sink_guard", 0.5f);
    g_scan_radius = f_cfg("scan_radius", 20.0f); if (g_scan_radius < 5.0f) g_scan_radius = 5.0f; if (g_scan_radius > 60.0f) g_scan_radius = 60.0f;
    g_force_psycho = b_cfg("debug_force_psycho", false);
    g_debug_start_key = gp_key_cfg("psycho", "debug_start_key", "NONE");
    g_debug_dodge = gp_ini().geti("psycho", "debug_dodge", 0);
    g_debug_dodge_lead = f_cfg("debug_dodge_lead", 0.15f);
    g_debug_move = gp_ini().geti("psycho", "debug_move", 0);
    g_perf_log = f_cfg("perf_log_seconds", 10.0f);
    g_debug_heal = f_cfg("debug_heal_below", 0.0f); if (g_debug_heal < 0.0f) g_debug_heal = 0.0f; if (g_debug_heal > 0.95f) g_debug_heal = 0.95f;
    g_debug_face = b_cfg("debug_face_enemy", false); g_debug_marks = b_cfg("debug_marks", false);
    g_debug_interrupt = gp_ini().geti("psycho", "debug_interrupt", 0);
    g_debug_rewind = f_cfg("debug_rewind_sim", 0.0f);
    drops_load_config();
    (void)g_blank_volume_dummy;
}
bool psycho_enabled() { return g_c.enabled; }

// ================================================================================================ engine calls
typedef char(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);               // ret 8
typedef void(__fastcall* Get_t)(DWORD node, void* edx, int id, void* out);                // ret 8
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);                          // ret 4
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);                    // ret 4
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);                         // ret 4
typedef void(__fastcall* RotY_t)(DWORD node, void* edx, float y);                         // ret 4
typedef void(__fastcall* Find_t)(DWORD node, void* edx, void* q);                         // ret 4
typedef void(__fastcall* Speed_t)(DWORD layer, void* edx, float m);                       // ret 4
typedef void(__fastcall* Emit_t)(DWORD sys, void* edx, DWORD* entity);                    // ret 4
typedef BYTE(__fastcall* Occl_t)(void* self, void* edx, const float* from, const float* to);   // ret 8
typedef void(__fastcall* Touch_t)(DWORD node, void* edx, int slot);                       // member-write notify, ret 4
static const DWORD P_PS_TOUCH = 0x86B4B0;

struct Cur { DWORD a, b, c; };
static Cur cur_save() { Cur k; k.a = RD32(A_CUR_NODE); k.b = RD32(A_CUR_2); k.c = RD32(A_CUR_3); return k; }
static void cur_restore(const Cur& k) { WR32(A_CUR_NODE, k.a); WR32(A_CUR_2, k.b); WR32(A_CUR_3, k.c); }
static DWORD data(DWORD node) { __try { return node ? RD32(node + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_handler(DWORD node) {
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool send(DWORD node, int id, void* args) { return node && gp_send(node, id, args); }
static bool command(DWORD node, int id, void* args) {   // by global command id
    if (!node) return false;
    Cur k = cur_save(); bool ok = true;
    __try { ((Cmd_t)(uintptr_t)P_PS_SENDCMD)(node, nullptr, id, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static bool native(DWORD node, int idx, void* args) {
    if (!node) return false;
    Cur k = cur_save(); bool ok = true;
    __try { ((Cmd_t)(uintptr_t)P_PS_NATIVE)(node, nullptr, idx, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static bool get_prop(DWORD node, int id, void* out) {
    if (!node) return false;
    __try { ((Get_t)(uintptr_t)P_PS_GETPROP)(node, nullptr, id, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool get_member(DWORD node, int slot, void* out) {
    if (!node) return false;
    __try { ((Get_t)(uintptr_t)P_PS_GETMEMBER)(node, nullptr, slot, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool get_pos(DWORD node, ps::Vec3* p) {
    if (!node) return false;
    float v[3];
    __try { ((Vec_t)(uintptr_t)P_PS_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p->x = v[0]; p->y = v[1]; p->z = v[2]; return true;
}
static bool get_fwd(DWORD node, ps::Vec3* f) {
    if (!node) return false;
    float b[6] = {0, 0, 0, 0, 0, 1};
    __try { ((Vec_t)(uintptr_t)P_PS_L2W)(node, nullptr, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float l = sqrtf(b[0] * b[0] + b[2] * b[2]);
    if (!(l > 1e-4f)) return false;
    f->x = b[0] / l; f->y = 0; f->z = b[2] / l; return true;
}
static bool set_pos(DWORD node, const ps::Vec3& p) {
    if (!node) return false;
    float v[3] = {p.x, p.y, p.z};
    __try { ((Vec_t)(uintptr_t)P_PS_SETPOS)(node, nullptr, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool set_rot_y(DWORD node, float y) {
    __try { ((RotY_t)(uintptr_t)P_PS_SETROTY)(node, nullptr, y); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// SetRotationY writes the LOCAL yaw: measure which world yaw it gives (two probes), as Suave does for its partner
static bool set_world_yaw(DWORD node, float want) {
    ps::Vec3 f0, f1;
    if (!node || !set_rot_y(node, 0.0f) || !get_fwd(node, &f0)) return false;
    if (!set_rot_y(node, 1.0f) || !get_fwd(node, &f1)) return false;
    float w0 = ps::yaw_of(f0), w1 = ps::yaw_of(f1);
    float d = w1 - w0; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
    float local = want;
    if (fabsf(fabsf(d) - 1.0f) < 0.2f) local = (d > 0 ? 1.0f : -1.0f) * (want - w0);
    return set_rot_y(node, local);
}
static DWORD find_child(DWORD parent, const char* name) {
    if (!parent || !name || !*name) return 0;
    struct { DWORD out; const char* name; } q = {0, name};
    __try { ((Find_t)(uintptr_t)P_PS_FINDCHILD)(parent, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return q.out;
}
static DWORD find_path(DWORD node, const char* path) {
    char seg[64];
    while (node && *path) {
        const char* e = strchr(path, '/'); size_t n = e ? (size_t)(e - path) : strlen(path);
        if (n >= sizeof seg) return 0;
        memcpy(seg, path, n); seg[n] = 0;
        node = find_child(node, seg);
        path += n + (e ? 1 : 0);
    }
    return node;
}
static DWORD list_stride(DWORD type_global) { __try { DWORD t = RD32(type_global); return t ? RD32(t + 0x24) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int list_count(DWORD L) { __try { return L ? (int)RD32(L + 8) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD list_at(DWORD L, int i, DWORD type_global) {
    __try {
        if (!L || i < 0 || i >= (int)RD32(L + 8)) return 0;
        DWORD k = list_stride(type_global); if (!k) return 0;
        DWORD a = RD32(L + 4); DWORD d = a ? RD32(a + 4) : 0; if (!d) return 0;
        return RD32(d + (DWORD)i * 4 * k);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD sub(DWORD root, DWORD off) { DWORD d = data(root); __try { return d ? RD32(d + off) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static void set_speed(DWORD layer, float m) { __try { if (layer) ((Speed_t)(uintptr_t)P_PS_LAYERSPEED)(layer, nullptr, m); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static float get_speed(DWORD layer) { __try { return layer ? RDF(layer + 0x8C) : 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 1.0f; } }
static int mission_index() { DWORD cd = gp_cache_data(); __try { DWORD m = cd ? RD32(cd + C_MISSION) : 0; DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + 0x38) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
// The move's clock: the engine's game-ms clock [0xA3DCD4] (Scene::Update 0x897E41..0x897E51: += game dt x 1000), so the
// timeline slows with the world (bullet time) like the enemy's own animation. (First probe used the TIMESTAMP node
// cache +274h -> data +4: it advanced at about a tenth of real time in the test fight, every phase lasted 10x too long.)
static const DWORD A_GAME_MS = 0xA3DCD4;
static bool game_seconds(double* t) {
    __try { *t = (double)(DWORD)RD32(A_GAME_MS) / 1000.0; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool timestamp_seconds(double* t) {   // the TIMESTAMP node (only for the verbose clock line)
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try { DWORD n = RD32(cd + C_CLOCK); DWORD d = n ? RD32(n + 0x18) : 0; if (!d) return false; *t = (double)(DWORD)RD32(d + 4) / 1000.0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- line test (Navigator::IsPointOccluded over the scene probe; Suave's free-space probe)
static bool g_ray_ok = true;
static bool blocked(const ps::Vec3& a, const ps::Vec3& b, DWORD ignore) {
    if (!g_ray_ok) return false;
    static BYTE self[0x80];
    *(DWORD*)(self + 0x70) = ignore;
    float f[4] = {a.x, a.y, a.z, 0.0f}, t[4] = {b.x, b.y, b.z, 0.0f};
    BYTE h = 0;
    Cur k = cur_save();
    __try { if (!RD32(A_SCENE) || !RD32(RD32(A_SCENE) + 0xA8)) { cur_restore(k); return false; } h = ((Occl_t)(uintptr_t)P_PS_OCCLUDED)(self, nullptr, f, t); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_ray_ok = false; gp_log("psycho: line test faulted: line tests off (every enemy counts as in sight)\n"); }
    cur_restore(k);
    return h != 0;
}

// ---- hostility: the Tornado's faction test (relations row 0 = the player's, value 4 = hostile)
static DWORD relations() {
    DWORD cd = gp_cache_data(); if (!cd) return 0;
    DWORD rel = 0;
    __try { DWORD ft = RD32(cd + C_FACTIONS); DWORD fd = ft ? RD32(ft + 0x18) : 0; DWORD tab = fd ? RD32(fd + 0x20) : 0; if (!get_member(tab, 0, &rel)) rel = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { rel = 0; }
    return rel;
}
static bool hostile_to_player(DWORD root, DWORD rel_list, int* fac_out) {
    int fac = -1;
    if (!get_prop(root, PROP_FACTION, &fac) || fac < 0 || fac > 255) return false;
    *fac_out = fac;
    DWORD row = list_at(rel_list, 0, A_LIST_ENTITY);
    if (!row) return false;
    __try {
        DWORD k = list_stride(A_LIST_INT); DWORD a = RD32(row + 4); DWORD d = a ? RD32(a + 4) : 0;
        if (!k || !d || fac >= (int)RD32(row + 8)) return false;
        return (int)RD32(d + (DWORD)fac * 4 * k) == 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ state
struct Who { DWORD root, ai, move, anim, phys, weapon, gun, effects, chest; float speed0; };
static ps::Registry g_reg;
static ps::Rng g_rng;
static ps::Active g_act;
static Who g_who;
static double g_next_check = 0.0, g_gap_until = 0.0, g_reassert_t = 0.0, g_last_t = -1.0;
static DWORD g_ram0 = 0; static int g_mission0 = -2;
static DWORD g_glow_on = 0;                    // the chest node the glow is attached to (0 = none)
static int   g_moves_started = 0, g_moves_done = 0, g_moves_cancelled = 0, g_hits = 0, g_misses = 0;
static float g_dealt = 0.0f;
static volatile LONG g_debug_start = 0;
static int   g_dodge_frames = 0, g_dodge_side = 0; static bool g_dodge_armed = false; static int g_dodge_count = 0;
static double g_perf_t0 = 0.0, g_perf_sum = 0.0, g_perf_max = 0.0; static int g_perf_n = 0;
static double g_last_pick_log = 0.0, g_last_cand_log = 0.0;

static bool enemy_alive(const Who& w, float* dmg) {
    if (!w.root || !w.ai) return false;
    __try {
        if (RD32(data(w.root) + R_AI) != w.ai) return false;   // the pooled character was reused
        DWORD a = data(w.ai); if (!a) return false;
        if (dmg) *dmg = RDF(a + 0x30);
        return RDF(a + 0x2C) > RDF(a + 0x30) && RD32(a + 0x1D4) != 28;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool fill_who(DWORD root, Who* w) {
    memset(w, 0, sizeof *w);
    w->root = root;
    DWORD d = data(root); if (!d) return false;
    __try {
        w->move = RD32(d + R_MOVE); w->anim = RD32(d + R_ANIM); w->ai = RD32(d + R_AI);
        w->phys = RD32(d + R_PHYS); w->weapon = RD32(d + R_WEAPON);
        DWORD wd = data(w->weapon); w->gun = wd ? RD32(wd + 0x20) : 0;   // the gun in hand (Suave's weapon_off)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (w->gun && cls_handler(w->gun) != H_WEAPON) w->gun = 0;
    if (w->gun && (!get_prop(w->gun, PROP_EFFECTCTRL, &w->effects) || !w->effects)) w->effects = 0;
    DWORD pelvis = find_path(w->anim, "Bip/Pelvis");
    w->chest = find_path(pelvis, "Spine/Spine1/Spine2");
    w->speed0 = get_speed(w->move);
    return true;
}
static bool ram_health(DWORD ram, float* left) {   // Health - DamageTaken of Ram's player AI (false = not readable)
    DWORD ai = sub(ram, R_AI);
    if (cls_handler(ai) != H_PLAYER_AI) return false;
    __try { DWORD a = data(ai); if (!a) return false; *left = RDF(a + 0x1C) - RDF(a + 0x20); return *left == *left; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float ram_health_left(DWORD ram) { float l = -1.0f; return ram_health(ram, &l) ? l : -1.0f; }   // log lines only

// ================================================================================================ effects / sound
static DWORD particle_template(int id) {
    DWORD cd = gp_cache_data(); if (!cd || id < 0) return 0;
    __try { return list_at(RD32(cd + C_PARTICLES), id, A_LIST_ENTITY); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool fx_on(int id, DWORD node) {
    DWORD t = particle_template(id); if (!t || !node) return false;
    DWORD ent = node;
    return command(t, CMD_TRIGGER_ENTITY, &ent);
}
static void fx_off(int id, DWORD node) {
    DWORD t = particle_template(id); if (!t || !node) return;
    DWORD ent = node; Cur k = cur_save();
    __try { ((Emit_t)(uintptr_t)(g_glow_kill ? P_PS_KILLEMIT : P_PS_STOPEMIT))(t, nullptr, &ent); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}
static void glow_off() { if (g_glow_on) { fx_off(g_glow_fx, g_glow_on); g_glow_on = 0; } }
static bool sting(DWORD speaker) {   // the loco-move sting the Tornado plays (0x5346AD): SoundDict list[107] via Play_Speak
    DWORD cd = gp_cache_data(); if (!cd || g_sound_id < 0 || ps::is_ram_voice(g_sound_id)) return false;
    DWORD slot = 0, snd = 0;
    __try { slot = list_at(RD32(cd + C_SOUNDDICT), g_sound_id, A_LIST_ENTITY); DWORD m = RD32(cd + C_MASTER); snd = m ? RD32(RD32(m + 0x18) + 0x3C) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!slot || !snd) return false;
    struct { DWORD slot, speaker; } a = {slot, g_sound_speaker ? speaker : 0};
    return send(snd, MSG_PLAY_SPEAK, &a);
}

// ================================================================================================ clips
static DWORD clip(DWORD cache_off, const char* path) {
    DWORD cd = gp_cache_data(); DWORD s = 0;
    __try { s = cd ? RD32(cd + cache_off) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { s = 0; }
    if (s) return s;
    DWORD animdict = 0;
    __try { DWORD h = RD32(A_PLAYER_HOLDER); animdict = h ? RD32(RD32(h + 0x18) + 0x9C) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { animdict = 0; }
    return find_path(animdict, path);
}
static bool play(const Who& w, DWORD slot) {
    if (!w.move || !slot) return false;
    struct { DWORD slot; int ev; } m = {slot, -1};
    return native(w.move, NAT_MOTION, &m);
}
static void park(DWORD move) { struct { int ctrl, state; } r = {3, 22}; send(move, MSG_MOVE_NEW_STATE, &r); }   // Victim_Move_ctrl 22 (EngageBeating)
static void unpark(DWORD move) { struct { int ctrl, state; } r = {0, 0}; send(move, MSG_MOVE_NEW_STATE, &r); send(move, MSG_MOVE_IDLE, nullptr); }

// ================================================================================================ the move
// ---- giving control back, and a watchdog over it. A rewind (after Ram's death) puts
// the world back a few seconds: an enemy whose move ended inside that window comes back as it was during the move (AI
// off = state 2, MoveCtrl parked, our clip speed) and nothing gave it back any more - the MM032 boss froze for good.
// Every enemy a move touched is watched for handback_watch_seconds (real) after its release and handed back again when
// its AI is in state 2 once more (never during a rewind or a pause).
static void hand_back(const Who& w, bool toro) {
    set_speed(w.move, w.speed0 > 0.05f ? w.speed0 : 1.0f);
    if (toro && g_toro_nocollide) send(w.phys, MSG_PHYS_COLLISION, nullptr);
    unpark(w.move);
    __try { DWORD a = data(w.ai); if (a && RDF(a + 0x264) > 0.0f) WRF(a + 0x264, 0.0f); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // KnockedOutTimer
    send(w.ai, MSG_AI_REGAIN, nullptr);
}
struct Recent { Who w; DWORD ph; bool toro, used, sim; double t, next; int fixes; };
static Recent g_recent[8];
static DWORD placeholder_of(DWORD root) { return sub(root, R_PLACEHOLDER); }
static int ai_state_of(DWORD ai) { __try { DWORD a = data(ai); return a ? (int)RD32(a + 0x1D4) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static void remember(const Who& w, bool toro) {
    Recent* r = nullptr;
    for (Recent& x : g_recent) if (x.used && x.w.root == w.root) { r = &x; break; }
    if (!r) for (Recent& x : g_recent) if (!x.used) { r = &x; break; }
    if (!r) { r = &g_recent[0]; for (Recent& x : g_recent) if (x.t < r->t) r = &x; }
    r->w = w; r->ph = placeholder_of(w.root); r->toro = toro; r->used = true; r->sim = false; r->t = gp_now(); r->next = r->t + 0.5; r->fixes = 0;
}
static void watchdog(double now) {
    if (g_watch_seconds <= 0.0f) return;
    int pm = gp_playmode(); if (pm == 2 || pm == 3) return;   // paused, or the rewind still plays: look again after it
    for (Recent& r : g_recent) {
        if (!r.used) continue;
        if (now - r.t > g_watch_seconds || r.fixes >= 4) { r.used = false; continue; }
        if (now < r.next) continue;
        r.next = now + 0.5;
        if (g_act.running() && g_who.root == r.w.root) continue;   // its next move runs
        if (!enemy_alive(r.w, nullptr) || placeholder_of(r.w.root) != r.ph) { r.used = false; continue; }   // dead, or the shell went to another placement
        if (g_debug_rewind > 0.0f && r.fixes == 0 && !r.sim && now - r.t >= g_debug_rewind) {   // test copy only
            r.sim = true; send(r.w.ai, MSG_AI_NO_CONTROL, nullptr); park(r.w.move);
            gp_log("psycho: debug_rewind_sim: enemy %08X put back into the move's state (AI off, MoveCtrl parked; AI state %d)\n", r.w.root, ai_state_of(r.w.ai));
            continue;
        }
        if (!ps::needs_handback(ai_state_of(r.w.ai))) continue;
        hand_back(r.w, r.toro); ++r.fixes;
        gp_log("psycho: enemy %08X had no AI control again %.1f s after its move (a rewind brought the move's state back): handed back again (AI state now %d)\n",
               r.w.root, now - r.t, ai_state_of(r.w.ai));
    }
}

static void release(int end, const char* why) {
    const Who& w = g_who;
    glow_off();
    float dmg = 0.0f; bool alive = enemy_alive(w, &dmg);
    bool same_level = ram_root() == g_ram0 && mission_index() == g_mission0;
    if (alive && same_level) {
        hand_back(w, g_act.move == ps::MOVE_TORO);
        remember(w, g_act.move == ps::MOVE_TORO);
    } else if (same_level && w.root && w.ai && sub(w.root, R_AI) == w.ai) {   // dead (or a lethal hit not processed yet), same shell
        set_speed(w.move, w.speed0 > 0.05f ? w.speed0 : 1.0f);   // the death clip at its own speed, not the spin's 0.4
        if (g_act.move == ps::MOVE_TORO && g_toro_nocollide && w.phys) send(w.phys, MSG_PHYS_COLLISION, nullptr);
        int st = ai_state_of(w.ai);
        if (st >= 0 && st != 28) {   // DamageTaken >= Health but the AI (off since the telegraph) never went to its death state
            hand_back(w, g_act.move == ps::MOVE_TORO);
            gp_log("psycho: enemy %08X took a lethal hit during the move (AI state %d): AI given back so the game lets it die\n", w.root, st);
        }
    }
    double t = 0.0; game_seconds(&t);
    g_gap_until = t + g_c.global_gap;
    if (end == ps::END_INTERRUPTED) {
        ps::Known* k = g_reg.find(g_act.root, g_act.ai);
        if (k) { double cd = g_rng.range(g_c.cooldown_min, g_c.cooldown_max) * g_c.interrupt_cooldown; k->next_ok = t + cd; }
        ++g_moves_cancelled;
    } else if (end == ps::END_ENEMY_DEAD || end == ps::END_RAM_BUSY || end == ps::END_LOST) ++g_moves_cancelled;
    else ++g_moves_done;
    g_hits += g_act.hits; g_misses += g_act.misses; g_dealt += g_act.dealt;
    gp_log("psycho: %s by %08X ENDS: %s%s%s - %d hit(s) %.0f raw damage, %d miss(es), %d blank(s), charge %.1f m, %.2f game s; AI %s "
           "(moves %d done / %d cancelled, total hits %d)\n",
           ps::move_name(g_act.move), w.root, ps::end_name(end), why ? " - " : "", why ? why : "", g_act.hits, g_act.dealt, g_act.misses,
           g_act.blanks, g_act.travelled, t - g_act.t_start, alive && same_level ? "back" : "not touched (dead / level changed)",
           g_moves_done, g_moves_cancelled, g_hits);
    g_act.phase = ps::PH_IDLE;
    memset(&g_who, 0, sizeof g_who);
    g_dodge_armed = false;
}

// ---- test copy only (all off unless the ini asks)
static int g_mark_n = 0;
static void mark(const char* tag) { if (g_debug_marks) gp_log("debug keys: mark shot ps%02d_%s\n", g_mark_n, tag); }
static void debug_heal(DWORD ram) {   // keeps a long proof fight going: the hits stay real, Ram just never dies
    if (g_debug_heal <= 0.0f) return;
    DWORD ai = sub(ram, R_AI); if (cls_handler(ai) != H_PLAYER_AI) return;
    __try {
        DWORD a = data(ai); if (!a) return;
        float hp = RDF(a + 0x1C), dt = RDF(a + 0x20);
        if (hp > 0.0f && dt > 0.0f && hp - dt < g_debug_heal * hp && dt < hp) {
            ((Touch_t)(uintptr_t)P_PS_TOUCH)(ai, nullptr, 3);   // DamageTaken: the game's own touch first (rewind record)
            WRF(a + 0x20, 0.0f);
            gp_log("psycho: debug_heal: Ram %.0f / %.0f -> DamageTaken now %.0f\n", hp - dt, hp, RDF(a + 0x20));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void debug_face(DWORD ram, const ps::Vec3& rp, const ps::Vec3& ep) {
    if (!g_debug_face) return;
    ps::Vec3 f; if (!get_fwd(ram, &f)) return;
    float d = ps::yaw_of(ps::norm_xz(ps::sub(ep, rp))) - ps::yaw_of(f);
    while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
    bool ok = gp_yaw_add(d * 57.29578f);
    gp_log("psycho: debug_face_enemy: Ram turns %.0f deg toward the enemy%s\n", d * 57.29578f, ok ? "" : " (mouse-turn path not available: not turned)");
}

static void start(const ps::Cand& e, int move, float dist, const ps::World& w) {
    Who who; if (!fill_who(e.root, &who) || !who.move || !who.ai) { gp_log("psycho: enemy %08X: controllers not readable, skipped\n", e.root); return; }
    DWORD slot = move == ps::MOVE_TORNADO ? clip(C_ANIM_LOCO, "PlayerAnims/specials/Loco_Move/LocoMove_Anim")
                                          : clip(C_ANIM_TORO_IDLE, "PlayerAnims/specials/Ram_Move/ramMove_idle");
    if (!slot) { gp_log("psycho: %s clip slot not found: skipped\n", ps::move_name(move)); return; }
    g_who = who;
    g_act.begin(g_c, move, e, w);
    ps::Known* k = g_reg.get(e.root, e.ai, w.t, g_c, e.ph, e.health);
    double cd = g_rng.range(g_c.cooldown_min, g_c.cooldown_max);
    k->next_ok = w.t + cd; ++k->moves;
    ++g_moves_started;
    // the enemy: its AI off (no_ai_control, as Suave's partner), its gun's burst stopped, its MoveCtrl parked in the Victim
    // controller (EngageBeating's {3,22}) so the clip is not overwritten, turned to Ram, then the clip (motion 5Eh)
    send(who.ai, MSG_AI_NO_CONTROL, nullptr);
    if (who.gun) send(who.gun, MSG_GUN_STOP_FIRING, nullptr);
    park(who.move);
    set_world_yaw(who.root, g_act.yaw);
    bool played = play(who, slot);
    set_speed(who.move, g_act.telegraph_speed(g_c));
    // telegraph: the red power-up flash + the orange goodie glow on the chest, the loco-move sting
    DWORD at = who.chest ? who.chest : who.root;
    bool fl = fx_on(g_flash_fx, at);
    bool gl = fx_on(g_glow_fx, at); if (gl) g_glow_on = at;
    bool sd = sting(who.root);
    float rh = ram_health_left(ram_root());
    gp_log("psycho: TELEGRAPH %s by enemy %08X (faction %d, hp %.0f/%.0f%s, state %d) %.1f m from Ram (Ram hp %.0f): %.2f s wind-up, "
           "clip %s, flash fx %d %s, glow fx %d %s, sting %d %s, gun %s; cooldown %.1f s\n",
           ps::move_name(move), e.root, e.faction, e.health - e.damage, e.health, ps::is_boss(g_c, e) ? ", BOSS" : ps::is_elite(g_c, e) ? ", elite" : "",
           e.state, dist, rh, g_c.telegraph, played ? "playing" : "FAILED", g_flash_fx, fl ? "on" : "FAILED", g_glow_fx, gl ? "on" : "FAILED",
           g_sound_id, g_sound_id < 0 ? "none (Ram's voice is never used)" : sd ? "played" : "FAILED", who.effects ? "with effects" : who.gun ? "(no EffectCtrl)" : "none", cd);
    g_reassert_t = gp_now(); g_reasserts = 0;
    ++g_mark_n; mark("telegraph");
    debug_face(ram_root(), w.ram, e.pos);
    if (g_debug_dodge && (g_debug_dodge == 1 || (g_dodge_count++ % 2) == 0)) { g_dodge_armed = true; g_dodge_side = (g_moves_started & 1) ? g_dodge_left : g_dodge_right; }
    else g_dodge_armed = false;
    g_interrupt_armed = g_debug_interrupt && !g_dodge_armed;
}

static void keep_ai_off(double now) {   // no_ai_control again if anything put the AI back (state 2 = no AI control)
    if (now - g_reassert_t < 0.3) return;
    int st = ai_state_of(g_who.ai);
    if (st != 2 && st != 28 && st >= 0) {
        g_reassert_t = now;
        send(g_who.ai, MSG_AI_NO_CONTROL, nullptr);
        if (g_verbose || g_reasserts++ == 0) gp_log("psycho: enemy %08X: something set its AI state to %d during the move -> no_ai_control again\n", g_who.root, st);
    }
}

static void hit_ram(DWORD ram, const ps::Step& s, const ps::Vec3& rp) {
    DWORD vol = 0; get_prop(sub(ram, R_PHYS), PROP_TORSO, &vol);
    float before = ram_health_left(ram);
    struct { float dmg; DWORD owner, inflictor, volume; float imp[3]; float pos[3]; int type; } h =
        {s.hit_damage, g_who.root, g_who.gun ? g_who.gun : g_who.root, vol, {s.hit_impulse.x, s.hit_impulse.y, s.hit_impulse.z}, {rp.x, rp.y + 1.0f, rp.z}, 1};
    bool ok = send(ram, MSG_ROOT_TAKE_A_HIT, &h);
    float after = ram_health_left(ram);
    gp_log("psycho: HIT Ram (%s): %.0f raw damage, owner enemy %08X, Ram hp %.0f -> %.0f%s\n", ps::move_name(g_act.move), s.hit_damage,
           g_who.root, before, after, ok ? "" : " (take_a_hit FAILED)");
}

static bool ram_dodging(DWORD ram) {
    __try { DWORD md = data(sub(ram, R_MOVE)); int ms = md ? (int)RD32(md + 0x6C) : -1; return (ms >= 0x1B && ms <= 0x20) || ms == 0x24; }   // shoot-dodges (PD_MOVESTATE)
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ram_busy(DWORD ram, const char** why) {
    int pm = gp_playmode();
    if (pm == 2 || pm == 3) { *why = pm == 3 ? "rewind" : "paused"; return true; }
    if (gp_cutscene()) { *why = "cut-scene"; return true; }
    DWORD cd = gp_cache_data();
    __try {
        if (cd && RD32(cd + C_EXCLUSIVE) != 0) { *why = "exclusive time (Tornado / power-up menu)"; return true; }
        DWORD sc = RD32(A_SCENE); if (sc && RDF(sc + 0x114) < 0.05f) { *why = "time frozen"; return true; }
        DWORD pd = data(sub(ram, R_POWERUP)); if (pd && RD32(pd + 0x20) != 0) { *why = "Ram's own loco move"; return true; }
        DWORD md = data(sub(ram, R_MOVE)); DWORD act = md ? RD32(md + 0x30) : 0;
        static DWORD veh = 0, veh_move = 0;
        DWORD mv = sub(ram, R_MOVE); if (mv != veh_move) { veh_move = mv; veh = find_child(mv, "InsideVehicleMoveCtrl"); }
        if (veh && act == veh) { *why = "vehicle"; return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Ram dead: health left <= 0 (it goes negative: one test log had a spin hitting on at -46 hp, through the death
    // and the rewind that followed; the old test only caught exactly 0)
    float hl = 0.0f; if (ram_health(ram, &hl) && hl <= 0.0f) { *why = "Ram dead"; return true; }
    return false;
}

// ================================================================================================ the tick
static int scan(DWORD ram, const ps::Vec3& rp, ps::Cand* out, int cap, float radius) {
    DWORD rel = relations(); if (!rel) return 0;
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp.x, rp.y, rp.z, radius, 0x40, 0};
    Cur k = cur_save();
    __try { ((Query_t)(uintptr_t)P_PS_QUERY)(RD32(A_QUADTREE), nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { q.out = 0; }
    cur_restore(k);
    DWORD L = q.out; int n = 0;
    for (int i = 0, c = list_count(L); i < c && n < cap; ++i) {
        DWORD r = list_at(L, i, A_LIST_ENTITY);
        if (!r || r == ram || cls_handler(r) != H_ROOT) continue;
        ps::Cand e; e.root = r;
        DWORD d = data(r); if (!d) continue;
        __try {
            e.ai = RD32(d + R_AI);
            e.sentry = e.ai && cls_handler(e.ai) == H_SENTRY;
            if (e.sentry) {
                DWORD a = data(e.ai); e.health = RDF(a + 0x2C); e.damage = RDF(a + 0x30); e.state = (int)RD32(a + 0x1D4); e.buddy = RD32(a + 0xB8) != 0;
                int sbt = (int)RD32(a + S_SUBTYPE); if (sbt == 4 || sbt == 7 || sbt == 8 || sbt == 9) e.special = true;   // escort / player / never aggressive / civilian
                if (e.state == 2 || e.state == 3 || e.state == 12 || e.state == 31) e.special = true;   // scripted / mounted gun / priority target / buddy (as gang wars)
            }
            e.ph = RD32(d + R_PLACEHOLDER);
            e.uid = RD32((e.ph ? e.ph : r) + 0x14) >> 8;   // Node m_Id: the same in every run for one placement
            DWORD wd = data(RD32(d + R_WEAPON)); DWORD gun = wd ? RD32(wd + 0x20) : 0;
            e.has_gun = gun && cls_handler(gun) == H_WEAPON;
            DWORD st = RD32(d + R_SUBTYPE); if (st == 7 || st == 8) e.sentry = false;   // not killable (Suave's rule)
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!e.sentry || !get_pos(r, &e.pos)) continue;
        e.hostile = hostile_to_player(r, rel, &e.faction);
        if (e.hostile && e.ph) { int vip = 0; if (get_prop(e.ph, PROP_ISVIP, &vip) && vip) e.special = true; }   // a VIP placement (a mission target)
        out[n++] = e;
    }
    k = cur_save();
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (L && lt) ((Free_t)(uintptr_t)P_PS_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
    return n;
}
static ps::Vec3 chest_of(DWORD root, DWORD chest) { ps::Vec3 p; if (get_pos(chest, &p)) return p; get_pos(root, &p); p.y += 1.2f; return p; }

#include "psycho_drops.inl"

static void perf(double t0, double now) {
    double ms = (gp_now() - t0) * 1000.0;
    g_perf_sum += ms; ++g_perf_n; if (ms > g_perf_max) g_perf_max = ms;
    if (g_perf_log > 0.0f && now - g_perf_t0 >= g_perf_log) {
        if (g_perf_n) gp_log("psycho: cost %.3f ms avg / %.3f ms max per tick over %d ticks (%s)\n", g_perf_sum / g_perf_n, g_perf_max, g_perf_n,
                             g_act.running() ? "a move runs" : "watching");
        static double gm0 = -1, ts0 = -1, rt0 = -1; double gm = 0, ts = 0;
        if (g_verbose && game_seconds(&gm) && timestamp_seconds(&ts)) {
            if (rt0 > 0 && now > rt0) gp_log("psycho: clocks over %.1f real s: game-ms %.2f s, TIMESTAMP %.2f s\n", now - rt0, gm - gm0, ts - ts0);
            gm0 = gm; ts0 = ts; rt0 = now;
        }
        g_perf_t0 = now; g_perf_sum = 0; g_perf_max = 0; g_perf_n = 0;
    }
}

void psycho_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed) return;
    double t0 = gp_now();
    DWORD ram = ram_root();
    int mis = mission_index();
    if (ram != g_ram0 || mis != g_mission0) {   // level change / new game / load: forget everything, touch nothing
        if (g_act.running()) { gp_log("psycho: level or mission changed during a move: released without touching the enemy\n"); g_act.phase = ps::PH_IDLE; memset(&g_who, 0, sizeof g_who); g_glow_on = 0; }
        if (g_reg.count()) gp_log("psycho: level / mission changed: %d known enemies forgotten\n", g_reg.count());
        g_reg.clear(); g_ram0 = ram; g_mission0 = mis; g_next_check = 0; g_gap_until = 0;
        for (Recent& r : g_recent) r.used = false;
        drops_level_change();
    }
    watchdog(now);
    ps::World w;
    if (!game_seconds(&w.t)) return;
    if (w.t < g_last_t - 1.0) { g_reg.clear(); g_next_check = 0; g_gap_until = 0; }   // the clock went back (rewind / load)
    g_last_t = w.t;
    w.ram_ok = ram && get_pos(ram, &w.ram);
    const char* busy = "";
    w.ram_busy = w.ram_ok && ram_busy(ram, &busy);
    w.ram_dodging = w.ram_ok && ram_dodging(ram);
    w.psycho = difficulty_enemy_loco_moves() || g_force_psycho;
    if (w.ram_ok) debug_heal(ram);
    drops_tick(ram, w);
    if (g_act.running()) {
        if (!w.psycho) { w.ram_busy = true; busy = "no longer PSYCHO"; }
        ps::EnemyNow en;
        en.alive = enemy_alive(g_who, &en.damage);
        if (en.alive) get_pos(g_who.root, &en.pos);
        if (en.alive && g_act.phase == ps::PH_ACTIVE) {
            if (g_act.move == ps::MOVE_TORNADO && w.t >= g_act.next_hit) {   // in sight: enemy chest -> Ram's chest
                ps::Vec3 a = chest_of(g_who.root, g_who.chest), b = w.ram; b.y += 1.2f;
                en.los_ok = !blocked(a, b, g_who.root);
            }
            if (g_act.move == ps::MOVE_TORO) {   // a wall 1.2 m ahead at knee and chest height
                ps::Vec3 a = en.pos, b = ps::add(en.pos, ps::mul(g_act.dir, 1.2f));
                a.y += 0.5f; b.y += 0.5f; bool k1 = blocked(a, b, g_who.root);
                a.y += 0.8f; b.y += 0.8f; bool k2 = !k1 && blocked(a, b, g_who.root);
                en.wall_ahead = k1 || k2;
            }
        }
        if (en.alive) keep_ai_off(now);
        ps::Step s = g_act.step(g_c, w, en);
        if (s.face) set_world_yaw(g_who.root, s.face_yaw);
        if (s.go_active) {
            mark("active");
            glow_off();
            if (g_act.move == ps::MOVE_TORO) {
                DWORD run = clip(C_ANIM_TORO_RUN, "PlayerAnims/specials/Ram_Move/ramMove_run");
                if (g_toro_nocollide) send(g_who.phys, MSG_PHYS_NO_COLLISION, nullptr);
                bool ok = play(g_who, run);
                gp_log("psycho: CHARGE by %08X: run clip %s, %.1f m toward (%.1f %.1f) at %.1f m/s\n", g_who.root, ok ? "playing" : "FAILED",
                       g_act.limit, g_act.target.x, g_act.target.z, g_c.toro_speed);
            } else gp_log("psycho: SPIN by %08X: firing frames at speed %.2f, gun %s\n", g_who.root, g_c.tornado_fire_speed, g_who.effects ? "fires blanks" : "silent (no EffectCtrl)");
        }
        if (s.clip_speed >= 0.0f) set_speed(g_who.move, s.clip_speed);
        if (s.place) set_pos(g_who.root, s.place_pos);
        else if (g_act.move == ps::MOVE_TORO && g_act.phase != ps::PH_TELEGRAPH && en.alive && g_toro_sink > 0.0f && en.pos.y < g_act.start.y - g_toro_sink) {
            ps::Vec3 p = en.pos; p.y = g_act.start.y; set_pos(g_who.root, p);   // collision off: never through the floor
            gp_log("psycho: charging enemy %08X sank to y %.2f: put back on %.2f\n", g_who.root, en.pos.y, p.y);
        }
        if (s.blank && g_who.effects) { int no = 0; command(g_who.effects, CMD_FIRE_BLANK, &no); }
        if (s.hit) { hit_ram(ram, s, w.ram); if (g_act.hits == 1) mark("hit"); }
        if (s.miss) gp_log("psycho: %s by %08X misses Ram: %s (%.1f m)\n", ps::move_name(g_act.move), g_who.root, s.miss_why, ps::dist_xz(w.ram, en.pos));
        if (s.recover) {
            if (g_act.move == ps::MOVE_TORO) {
                if (g_toro_nocollide) send(g_who.phys, MSG_PHYS_COLLISION, nullptr);
                play(g_who, clip(C_ANIM_TORO_IDLE, "PlayerAnims/specials/Ram_Move/ramMove_idle"));
            }
            if (g_verbose) gp_log("psycho: %s by %08X: recover (%s)\n", ps::move_name(g_act.move), g_who.root, ps::end_name(g_act.end));
        }
        // test copy: a shot into the wind-up (the game's own damage path, owner Ram)
        if (g_interrupt_armed && g_act.phase == ps::PH_TELEGRAPH && w.t - g_act.t_phase >= 0.4) {
            g_interrupt_armed = false;
            DWORD vol = 0; get_prop(g_who.phys, PROP_TORSO, &vol);
            ps::Vec3 c = chest_of(g_who.root, g_who.chest);
            struct { float dmg; DWORD owner, inflictor, volume; float imp[3]; float pos[3]; int type; } h =
                {20.0f, ram, ram, vol, {0, 0, 0}, {c.x, c.y, c.z}, 1};
            bool ok = send(g_who.root, MSG_ROOT_TAKE_A_HIT, &h);
            gp_log("psycho: debug_interrupt: Ram's shot hits enemy %08X in the wind-up (20 damage)%s\n", g_who.root, ok ? "" : " (FAILED)");
            mark("interrupt");
        }
        // test copy: Ram dives out of the way (the proof's dodge)
        if (g_dodge_armed && g_act.phase != ps::PH_IDLE) {
            bool go = false;
            if (g_act.move == ps::MOVE_TORNADO && g_act.phase == ps::PH_TELEGRAPH && w.t - g_act.t_phase >= g_c.telegraph - g_debug_dodge_lead) go = true;
            if (g_act.move == ps::MOVE_TORO && g_act.phase == ps::PH_ACTIVE && ps::dist_xz(w.ram, en.pos) <= g_c.toro_speed * (0.35f + g_debug_dodge_lead)) go = true;
            if (go) { g_dodge_frames = 6; g_dodge_armed = false; gp_log("psycho: debug_dodge: Ram dives (%s)\n", g_dodge_side == g_dodge_left ? "left" : "right"); }
        }
        if (s.release) release(s.end, s.end == ps::END_RAM_BUSY ? busy : nullptr);
        perf(t0, now);
        return;
    }
    if (w.t < g_next_check) { perf(t0, now); return; }
    g_next_check = w.t + g_c.check_interval;
    bool forced = InterlockedExchange(&g_debug_start, 0) != 0;
    if (!w.psycho && !forced) { perf(t0, now); return; }
    static ps::Cand cand[48];
    int n = w.ram_ok ? scan(ram, w.ram, cand, 48, g_scan_radius) : 0;
    drops_fill_abilities(cand, n);                // every enemy has one fixed move (the one it drops)
    ps::Pick p;
    if (forced) {   // test copy: the nearest qualifying enemy now (no roll, no cooldown, no gap)
        if (g_debug_move == 1 || g_debug_move == 2) for (int i = 0; i < n; ++i) cand[i].ability = g_debug_move;
        ps::Cfg fc = g_c; fc.chance = 1.0f; fc.enabled = true; if (g_debug_move == 1 || g_debug_move == 2) fc.moves = 1u << g_debug_move;
        ps::Registry tmp; ps::World fw = w; fw.psycho = true; fc.first_delay = 0;
        p = ps::pick(fc, tmp, fw, cand, n, g_rng, false, 0);
        gp_log("psycho: debug start key: %s\n", p.index >= 0 ? "nearest qualifying enemy picked" : p.why);
    } else p = ps::pick(g_c, g_reg, w, cand, n, g_rng, false, g_gap_until);
    if (g_verbose && now - g_last_pick_log > 5.0) {
        g_last_pick_log = now; int el = 0; for (int i = 0; i < n; ++i) el += ps::alive(cand[i]) && cand[i].hostile && ps::is_elite(g_c, cand[i]);
        gp_log("psycho: check: %d characters within %.0f m, %d hostile elite(s) alive, known %d: %s%s%s\n", n, g_scan_radius, el, g_reg.count(), p.why,
               w.ram_busy ? " - Ram busy: " : "", w.ram_busy ? busy : "");
    }
    if ((forced || g_verbose) && p.index < 0 && n > 0 && (forced || now - g_last_cand_log > 5.0)) {   // why not (each character)
        g_last_cand_log = now;
        for (int i = 0; i < n && i < 12; ++i) {
            const ps::Cand& e = cand[i];
            gp_log("psycho:   %08X faction %d hp %.0f/%.0f state %d %.1f m gun %d hostile %d: %s\n", e.root, e.faction, e.health - e.damage, e.health,
                   e.state, ps::dist_xz(e.pos, w.ram), e.has_gun ? 1 : 0, e.hostile ? 1 : 0, ps::reject_reason(g_c, g_reg, w, e));
        }
    }
    if (p.index < 0) { perf(t0, now); return; }
    const ps::Cand& e = cand[p.index];
    // line of sight from the enemy's chest to Ram's chest (no move through a wall); the charge also needs a clear run
    Who tmp; fill_who(e.root, &tmp);
    ps::Vec3 a = chest_of(e.root, tmp.chest), b = w.ram; b.y += 1.2f;
    bool ok = !blocked(a, b, e.root);
    if (ok && p.move == ps::MOVE_TORO) { ps::Vec3 k1 = e.pos, k2 = w.ram; k1.y += 0.5f; k2.y += 0.5f; ok = !blocked(k1, k2, e.root); }
    if (!ok) {
        ps::Known* k = g_reg.get(e.root, e.ai, w.t, g_c, e.ph, e.health); if (k->next_ok < w.t + g_c.retry_delay) k->next_ok = w.t + g_c.retry_delay;
        if (g_verbose) gp_log("psycho: enemy %08X would %s but has no clear line to Ram: retry in %.1f s\n", e.root, ps::move_name(p.move), g_c.retry_delay);
        perf(t0, now); return;
    }
    start(e, p.move, p.distance, w);
    perf(t0, now);
}

// main loop after the input poll: the debug start key and the test copy's dodge (the game's own keyboard arrays)
void psycho_post_input(BYTE* edges, BYTE* state, double now) {
    (void)now;
    if (!g_installed || !edges || !state) return;
    drops_post_input(state);
    static bool held = false;
    if (g_debug_start_key >= 0 && g_debug_start_key < 256) {
        bool h = (state[g_debug_start_key] & 0x80) != 0;
        if (h && !held) InterlockedExchange(&g_debug_start, 1);
        held = h;
    }
    if (g_dodge_frames > 0) {   // as Suave's automatic dive: the side key held, SPACE pressed on frame 4
        __try {
            state[g_dodge_side] = 0x80;
            if (g_dodge_frames == 6) edges[g_dodge_side] |= 1;
            if (g_dodge_frames <= 4 && g_dodge_frames >= 2) state[g_dodge_key] = 0x80;
            if (g_dodge_frames == 4) edges[g_dodge_key] |= 1;
            if (g_dodge_frames == 1) { edges[g_dodge_key] |= 2; edges[g_dodge_side] |= 2; state[g_dodge_side] = 0; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        --g_dodge_frames;
    }
}

// ================================================================================================ install
static const char* PS_REFS[] = {
    // shared with Suave / Smart AI / difficulty (same addresses, same use)
    "F_SENDMSG", "F_CMD", "F_NATIVE", "F_GETPROP", "F_GETMEMBER", "F_QUERY", "F_LISTFREE", "F_GETPOS", "F_L2W", "F_SETPOS",
    "F_SETROTY", "F_FINDCHILD", "F_LAYERSPEED", "F_OCCLUDED", "SV_OCCL_IGNORE", "SV_OCCL_MASK", "SV_CLASSOBJ", "SV_CLS_SENTRY",
    "SV_CLS_ROOT", "SV_CLS_PLAYERAI", "SV_CLS_WEAPON", "SV_QUERY_A", "SV_QUERY_FREE", "SV_FACTION_A", "SV_FACTION_B", "SV_FACTION_C",
    "SV_MOTION", "SV_BEATING", "SV_ANIMDICT", "H_NO_AI", "H_ROOT_HIT", "SM_ROOT_AI", "SM_SLOT_02", "SM_SLOT_03", "SM_RD_STATE",
    "SM_CLOCK", "PS_GAMEMS", "G_MISSION", "G_MISSION_CM", "G_CACHE_EXC", "G_SCENE_TM", "PD_MOVESTATE", "SV_MUSIC_B",
    // this feature's own (sites.h section "PSYCHO enemy loco moves")
    "PS_ANIM_LOCO", "PS_ANIM_TORO_IDLE", "PS_ANIM_TORO_RUN", "PS_STING", "PS_BLANK", "PS_PARTLIST", "PS_PARTTRIG", "PS_STOPEMIT",
    "PS_KILLEMIT", "PS_RAM_HEALTH", "PS_RUNNING_MOVE"
};
static const char* DROP_REFS[] = {
    "PS_RES_REQUEST", "PS_RES_LIST", "PS_RES_LIST2", "PS_RES_DANGLING", "PS_RES_GIVEUP", "PS_PROP_TAKEOVER", "PS_PROP_TIMER", "PS_PROP_TIMER0",
    "PS_SHOW", "PS_SETFLAGS", "PS_POWERUP_COUNT"
};
bool psycho_install(bool tick_ok) {
    g_installed = false;
    if (!g_c.enabled) return false;
    if (!tick_ok) { gp_log("psycho: the input tick is not installed: OFF\n"); return false; }
    for (const char* r : PS_REFS) if (!gp_ref_ok(r)) { gp_log("psycho: reference %s differs: OFF\n", r); return false; }
    if (!g_c.moves) { gp_log("psycho: no known move in 'moves' (tornado, toro): OFF\n"); return false; }
    g_installed = true;
    if (g_c.drops) for (const char* r : DROP_REFS) if (!gp_ref_ok(r)) { gp_log("psycho: reference %s differs: elite drops OFF\n", r); g_c.drops = false; break; }
    if (g_c.drops) gp_log("psycho: elite drops on: a killed elite leaves its ability for %.0f s, at most %d at once\n", g_c.drop_seconds, g_c.drop_max);
    g_rng.s ^= (uint32_t)GetTickCount() * 2654435761u; if (!g_rng.s) g_rng.s = 1;
    gp_log("psycho: enemy loco moves installed (active only on PSYCHO%s): moves%s%s, cooldown %.0f-%.0f s, telegraph %.2f s, %s, "
           "chance %.2f per %.1f s, first move after %.0f s, gap %.0f s; tornado %.0f-%.0f m %.0f dmg/hit, toro %.0f-%.0f m %.0f dmg%s%s\n",
           g_force_psycho ? ", FORCED by debug_force_psycho" : "", (g_c.moves & 2) ? " tornado" : "", (g_c.moves & 4) ? " toro" : "",
           g_c.cooldown_min, g_c.cooldown_max, g_c.telegraph, g_c.elite_only ? (g_c.bosses ? "elites and bosses only" : "elites only, never bosses") : (g_c.bosses ? "every hostile enemy" : "every hostile enemy but bosses"),
           g_c.chance, g_c.check_interval, g_c.first_delay, g_c.global_gap, g_c.tornado_min, g_c.tornado_max, g_c.tornado_damage,
           g_c.toro_min, g_c.toro_max, g_c.toro_damage, g_debug_start_key >= 0 ? ", DEBUG START KEY" : "", g_debug_dodge ? ", DEBUG DODGE" : "");
    return true;
}

// ---- test host only
static int __cdecl TestInstalled() { return g_installed ? 1 : 0; }
static int __cdecl TestMoves() { return (int)g_c.moves; }
void* psycho_test_stub(const char* name) {
    if (!strcmp(name, "psycho_installed")) return (void*)&TestInstalled;
    if (!strcmp(name, "psycho_moves")) return (void*)&TestMoves;
    return nullptr;
}
