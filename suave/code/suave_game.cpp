// suave_game.cpp - game glue of the Suave loco move (power-up slot 11) in TODGameplay.dll. Reads the game into a
// sv::Snap, runs sv::Suave (suave_logic.cpp, pure, unit tested) and turns its actions into the game's own calls.
// Compiled into TODGameplay.dll by dll/build.bat. Rules as in dll/src/gp_game.cpp: off unless [suave] enabled = 1,
// every site and every referenced address byte-checked (sites.h SITE/REF rows), every game read SEH-guarded, no
// engine classes, nothing patched when off. Evidence for every call: dll/HOOKS.md section "Suave".
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../dll/src/gp_glue.h"
#include "suave_logic.h"

// ================================================================================================ game addresses
#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v)  (*(volatile float*)(uintptr_t)(a) = (float)(v))
static const DWORD A_PLAYER_HOLDER = 0xA3B590, A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_LIST_INT = 0xA3BB04;
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_SCENE = 0xA3DCBC;
// functions (calling conventions: suave/code/tools/callconv.py)
static const DWORD P_SENDCMD = 0x86A340, P_NATIVE = 0x86A250, P_TRIGGER = 0x86A2B0, P_GETPROP = 0x86A6A0,
                   P_GETMEMBER = 0x869E80, P_TOUCH = 0x86B4B0, P_QUERY = 0x486950, P_LISTFREE = 0x8718D0,
                   P_GETPOS = 0x88CA80, P_L2W = 0x88C9C0, P_SETPOS = 0x88E600, P_SETROTY = 0x891360,
                   P_FINDCHILD = 0x88EB60, P_SETSTREAM = 0x89E270, P_LAYERSPEED = 0x5010A0, P_MODELRES = 0x9159E0,
                   P_ELIGIBLE = 0x5BCD90, P_SETFLAGS = 0x500BE0, P_OCCLUDED = 0x91D020;
// script cache data offsets
static const DWORD C_MASTER = 0x254, C_PLAYER_AI = 0x258, C_MISSION = 0x25C, C_GAMECAM = 0x260, C_FACTIONS = 0x278,
                   C_TIMECTRL = 0x280, C_SOUNDDICT = 0x3D8, C_SHELLS = 0x550, C_COMBOMUSIC = 0x5B4, C_PLAYER_SPEED = 0x5A0,
                   C_WORLDTIME = 0x850;
// character_root data
static const DWORD R_SUBTYPE = 0x14, R_MOVE = 0x48, R_ACTION = 0x4C, R_ANIM = 0x50, R_AI = 0x58, R_PHYS = 0x68,
                   R_WEAPON = 0x6C, R_GOODIE = 0x88, R_POWERUP = 0x8C, R_FACE = 0x9C;
// class handlers (script class object +0x60, registration sites in sites.h)
static const DWORD H_SENTRY = 0x5C90F0, H_ROOT = 0x5C3060, H_SHELL = 0x843D10, H_PLAYER_AI = 0x5ACE10, H_FACE = 0x541A50,
                   H_WEAPON = 0x665630;   // default_weapon (the guns in hands)
// message ids (checked against the class method tables by suave/code/tools/check_suave.py)
#define MSG(name, cls, id, cmd) static const int name = id;
#include "suave_msgs.h"
#undef MSG

// ================================================================================================ config
static sv::Cfg g_c;
static int   g_cheat_key = gp::KEY_NONE, g_key_fwd = 0x11 /*W*/, g_key_dodge = 0x39 /*SPACE*/;
static int   g_key_back = 0x1F /*S*/, g_key_left = 0x1E /*A*/, g_key_right = 0x20 /*D*/;
static int   g_slot = 11, g_hud_effect = 11, g_music_slot = 110, g_dive_state = 0x24, g_dive_state2 = -1;
static float g_boom_mass = 80.0f, g_kill_delay = 0.05f;
static bool  g_face_force = true, g_setplaypos = true, g_rotate = true, g_ram_park = true;
static char  g_clip_root[96] = "@camera:Suave";   // see clip_slot()
static char  g_clip_fmt[sv::CLIP_COUNT][48] = {"suave_lead_%s", "suave_follow_%s", "suave_push_%s", "suave_stumble_%s",
                                               "suave_circle", "suave_intro_%s", "suave_grabbed_%s", "suave_lookdown_%s"};
static char  g_music_fmt[96] = "/Sounds/StreamedPowerMoves/Suave_Mus_%d.wav";
static char  g_music_restore[96] = "/Sounds/StreamedPowerMoves/Mariachi_02.wav";
static float g_music_len = 17.4f;         // seconds a segment plays (restore the borrowed slot after it)
// the dynamite in a hand (fourth test: at the belt it never showed): the game's own grenade hold is attachnode(slot 3,
// node, "l_hand_default", "L Hand") on the character's Animation (in 0x5055B0 and 0x5A2EC0); Ram shows it, then hands it over
static char  g_dyn_grip[32] = "l_hand_default", g_dyn_ram_hand[32] = "L Hand", g_dyn_partner_hand[32] = "L Hand";
static int   g_dyn_slot = 3;
// fifth test: "REALLY visible, like strapped to their chest" - a bundle of sticks held on the partner's chest by the DLL
// (each frame: Spine2 + strap_out m along his body's forward, side by side), not a bone attachment inside the body
static int   g_dyn_sticks = 3; static float g_strap_out = 0.22f, g_strap_drop = -0.12f, g_strap_gap = 0.07f;
static bool  g_space_probe = true, g_cam_clamp = true;     // the free-space probe / camera kept out of walls (rays)
static int   g_debug_start_key = gp::KEY_NONE;             // test copy: start the move on this key (no charge, no gate)
static bool  g_debug_shots = false;                        // test copy: "debug keys: mark shot sv_..." at every cut
// the dive (fourth test: the quaternion half turn put Ram under the map): the game's own turning, then the key by his
// measured facing; nothing within dive_clear of him at the takeoff; Ram put back when he sinks below the takeoff floor
static float g_turn_seconds = 1.0f, g_turn_tol_deg = 15.0f, g_turn_sign = 1.0f, g_side_sign = 1.0f;
static float g_dive_clear = 0.6f, g_dive_step = 0.0f, g_ram_snap = 0.35f, g_partner_clear = 1.5f;
static bool  g_installed = false, g_verbose = false;
static bool  g_taken[1 + sv::MAX_RING];          // partner (0) and ring members (1..8) under our control
static double g_reassert_t[1 + sv::MAX_RING];
static int   g_weapon_mode = 2;     // NPCs: 0 keep, 1 holster (switch_to_unarmed), 2 drop (holster + force_drop_weapon of the gun in hand)
static bool  g_ram_holster = true, g_music_kill_combo = true, g_music_pause_level = true, g_music_duck = true, g_level_paused = false;

static void str_cfg(const char* k, char* out, size_t cap) {
    const char* v = gp_ini().get("suave", k);
    if (v) { strncpy(out, v, cap - 1); out[cap - 1] = 0; }
}
static bool b_cfg(const char* k, bool d) { return gp_ini().geti("suave", k, d ? 1 : 0) != 0; }
static float f_cfg(const char* k, float d) { return gp_ini().getf("suave", k, d); }
void suave_load_config() {
    sv::Cfg c;
    c.enabled = b_cfg("enabled", false);
    c.grab_radius = f_cfg("grab_radius", c.grab_radius); c.pull_radius = f_cfg("pull_radius", c.pull_radius);
    c.ring_max = gp_ini().geti("suave", "ring_max", c.ring_max); c.ring_radius = f_cfg("ring_radius", c.ring_radius);
    c.max_height = f_cfg("max_height", c.max_height);
    c.bpm = f_cfg("bpm", c.bpm);
    c.freeze_seconds = f_cfg("freeze_seconds", c.freeze_seconds); c.freeze_scale = f_cfg("freeze_scale", c.freeze_scale);
    c.freeze_ramp_in = f_cfg("freeze_ramp_in", c.freeze_ramp_in); c.freeze_ramp_out = f_cfg("freeze_ramp_out", c.freeze_ramp_out);
    c.intro_beats = f_cfg("intro_beats", c.intro_beats); c.gather_lead_beats = f_cfg("gather_lead_beats", c.gather_lead_beats);
    c.gather_beats = f_cfg("gather_beats", c.gather_beats); c.orbit_beats = f_cfg("orbit_beats", c.orbit_beats);
    c.cut_beats = f_cfg("cut_beats", c.cut_beats); c.dance_beats = f_cfg("dance_beats", c.dance_beats);
    c.dance_cycles = gp_ini().geti("suave", "dance_cycles", c.dance_cycles); c.release_beat = f_cfg("release_beat", c.release_beat);
    c.attach_seconds = f_cfg("attach_seconds", c.attach_seconds); c.closeup_seconds = f_cfg("closeup_seconds", c.closeup_seconds);
    c.push_seconds = f_cfg("push_seconds", c.push_seconds);
    c.intro_place = b_cfg("intro_place", c.intro_place); c.intro_x = f_cfg("intro_x", c.intro_x); c.intro_z = f_cfg("intro_z", c.intro_z);
    c.face_offset_deg = f_cfg("face_offset_deg", c.face_offset_deg);
    c.dive_at_seconds = f_cfg("dive_at_seconds", c.dive_at_seconds);
    c.ending_scale = f_cfg("ending_scale", c.ending_scale); c.ending_ramp = f_cfg("ending_ramp", c.ending_ramp);
    c.plant_scale = f_cfg("plant_scale", c.plant_scale); c.dive_scale = f_cfg("dive_scale", c.dive_scale);
    c.takeoff_fallback = f_cfg("takeoff_fallback", c.takeoff_fallback); c.front_height = f_cfg("front_height", c.front_height);
    c.auto_dive = b_cfg("auto_dive", c.auto_dive); c.dive_wait = f_cfg("dive_wait", c.dive_wait);
    // fifth test: new key names (the old lookdown_ / back_ / boom_real|game values in installed inis were the slower defaults)
    c.lookdown_real = f_cfg("cut_lookdown_real", c.lookdown_real); c.lookdown_game = f_cfg("cut_lookdown_game", c.lookdown_game);
    // sixth round: the panic cut by length and Ram's height (cut_back_* / blast_* of the 10:04 quick fix are not read)
    c.panic_min = f_cfg("panic_min", c.panic_min); c.panic_max = f_cfg("panic_max", c.panic_max);
    c.panic_lift = f_cfg("panic_lift", c.panic_lift); c.panic_ram_speed = f_cfg("panic_ram_speed", c.panic_ram_speed);
    c.blast_after = f_cfg("blast_after", c.blast_after);
    c.boom_real = f_cfg("blast_max_real", c.boom_real); c.boom_game = f_cfg("blast_max_game", c.boom_game);
    c.land_hold = f_cfg("land_hold", c.land_hold); c.after_max = f_cfg("after_max", c.after_max);
    c.god_margin = f_cfg("god_margin", c.god_margin);
    c.arrive_distance = f_cfg("arrive_distance", c.arrive_distance); c.place_late_members = b_cfg("place_late_members", c.place_late_members);
    c.ring_phase_beats = f_cfg("ring_phase_beats", c.ring_phase_beats);
    c.lock_input = b_cfg("lock_input", c.lock_input);
    c.camera = b_cfg("camera", c.camera); c.camera_blend_back = f_cfg("camera_blend_back", c.camera_blend_back);
    c.faces = b_cfg("faces", c.faces); c.face_stay = f_cfg("face_stay", c.face_stay);
    c.dynamite = b_cfg("dynamite", c.dynamite); c.boom_radius = f_cfg("boom_radius", c.boom_radius);
    c.boom_damage = f_cfg("boom_damage", c.boom_damage); c.boom_impulse = f_cfg("boom_impulse", c.boom_impulse);
    c.boom_slowmo = f_cfg("boom_slowmo", c.boom_slowmo); c.shake = b_cfg("shake", c.shake);
    c.music = b_cfg("music", c.music); c.music_segments = gp_ini().geti("suave", "music_segments", c.music_segments);
    c.music_soldier = gp_ini().geti("suave", "music_soldier", c.music_soldier);
    c.music_civil = gp_ini().geti("suave", "music_civil", c.music_civil);
    c.music_dea = gp_ini().geti("suave", "music_dea", c.music_dea);
    c.music_other = gp_ini().geti("suave", "music_other", c.music_other);
    c.abort_on_partner_death = b_cfg("abort_on_partner_death", c.abort_on_partner_death);
    c.partner_drift = f_cfg("partner_drift", c.partner_drift);
    c.dynamite_show = f_cfg("dynamite_show", c.dynamite_show);
    c.clamp();
    g_c = c;
    g_cheat_key = gp_key_cfg("suave", "cheat_key", "NONE");
    g_key_fwd = gp_key_cfg("suave", "dive_key_forward", "W"); g_key_dodge = gp_key_cfg("suave", "dive_key_dodge", "SPACE");
    g_key_back = gp_key_cfg("suave", "dive_key_back", "S"); g_key_left = gp_key_cfg("suave", "dive_key_left", "A"); g_key_right = gp_key_cfg("suave", "dive_key_right", "D");
    if (g_key_back < 0 || g_key_back > 255) g_key_back = 0x1F; if (g_key_left < 0 || g_key_left > 255) g_key_left = 0x1E; if (g_key_right < 0 || g_key_right > 255) g_key_right = 0x20;
    if (g_key_fwd < 0 || g_key_fwd > 255) g_key_fwd = 0x11;
    if (g_key_dodge < 0 || g_key_dodge > 255) g_key_dodge = 0x39;
    g_slot = gp_ini().geti("suave", "slot", 11);
    if (g_slot != 10 && g_slot != 11) g_slot = 11;   // 10 = replace the Pinata (test), 11 = Tequila
    g_hud_effect = gp_ini().geti("suave", "hud_effect", g_slot);
    g_verbose = b_cfg("log_verbose", false);
    g_weapon_mode = gp_ini().geti("suave", "enemy_weapons", 2); if (g_weapon_mode < 0 || g_weapon_mode > 2) g_weapon_mode = 2;
    g_ram_holster = b_cfg("ram_holster", true);
    g_music_kill_combo = b_cfg("music_kill_combo", true); g_music_pause_level = b_cfg("music_pause_level", true); g_music_duck = b_cfg("music_duck", true);
    g_music_slot = gp_ini().geti("suave", "music_slot", 110);
    g_dive_state = gp_ini().geti("suave", "dive_state", 0x24); g_dive_state2 = gp_ini().geti("suave", "dive_state2", -1);
    g_boom_mass = f_cfg("boom_mass", 80.0f); g_kill_delay = f_cfg("kill_pass_delay", 0.05f);
    g_face_force = b_cfg("face_force", true); g_setplaypos = b_cfg("ring_setplaypos", true);
    g_rotate = b_cfg("rotate", true); g_ram_park = b_cfg("ram_park", true);
    g_music_len = f_cfg("music_seconds", 17.4f);
    str_cfg("clip_root", g_clip_root, sizeof g_clip_root);
    static const char* keys[sv::CLIP_COUNT] = {"clip_lead", "clip_follow", "clip_push", "clip_stumble", "clip_circle",
                                               "clip_intro", "clip_grabbed", "clip_lookdown"};
    for (int i = 0; i < sv::CLIP_COUNT; ++i) str_cfg(keys[i], g_clip_fmt[i], sizeof g_clip_fmt[i]);
    str_cfg("music_path", g_music_fmt, sizeof g_music_fmt); str_cfg("music_restore_path", g_music_restore, sizeof g_music_restore);
    // (the belt version's dynamite_bone / dynamite_pivot keys are no longer read)
    str_cfg("dynamite_grip", g_dyn_grip, sizeof g_dyn_grip);
    str_cfg("dynamite_ram_hand", g_dyn_ram_hand, sizeof g_dyn_ram_hand); str_cfg("dynamite_partner_hand", g_dyn_partner_hand, sizeof g_dyn_partner_hand);
    g_dyn_slot = gp_ini().geti("suave", "dynamite_slot", 3); if (g_dyn_slot < 0 || g_dyn_slot > 15) g_dyn_slot = 3;
    g_dyn_sticks = gp_ini().geti("suave", "dynamite_sticks", 3); if (g_dyn_sticks < 1) g_dyn_sticks = 1; if (g_dyn_sticks > 4) g_dyn_sticks = 4;
    g_strap_out = f_cfg("dynamite_out", 0.22f); g_strap_drop = f_cfg("dynamite_drop", -0.12f); g_strap_gap = f_cfg("dynamite_gap", 0.07f);
    g_space_probe = b_cfg("space_probe", true); g_cam_clamp = b_cfg("camera_clamp", true);
    g_debug_start_key = gp_key_cfg("suave", "debug_start_key", "NONE"); g_debug_shots = b_cfg("debug_shots", false);
    g_turn_seconds = f_cfg("turn_seconds", 1.0f); if (g_turn_seconds < 0.0f) g_turn_seconds = 0.0f; if (g_turn_seconds > 3.0f) g_turn_seconds = 3.0f;
    g_turn_tol_deg = f_cfg("turn_tolerance_deg", 15.0f); if (g_turn_tol_deg < 2.0f) g_turn_tol_deg = 2.0f; if (g_turn_tol_deg > 60.0f) g_turn_tol_deg = 60.0f;
    g_turn_sign = f_cfg("turn_sign", 1.0f) < 0.0f ? -1.0f : 1.0f;
    g_side_sign = f_cfg("dive_side_sign", 1.0f) < 0.0f ? -1.0f : 1.0f;
    g_dive_clear = f_cfg("dive_clear", 0.6f); g_dive_step = f_cfg("dive_step", 0.0f); g_ram_snap = f_cfg("ram_ground_snap", 0.35f);
    if (g_dive_step < 0.0f) g_dive_step = 0.0f; if (g_dive_step > 1.5f) g_dive_step = 1.5f;
    g_partner_clear = f_cfg("partner_clear", 1.5f); if (g_partner_clear < 0.0f) g_partner_clear = 0.0f; if (g_partner_clear > 3.0f) g_partner_clear = 3.0f;
}
bool suave_enabled() { return g_c.enabled; }

// ================================================================================================ engine calls
typedef void(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);                  // ret 8
typedef void(__fastcall* Trig_t)(DWORD node, void* edx, int idx, int gid, void* args);       // ret 12
typedef void(__fastcall* Get_t)(DWORD node, void* edx, int id, void* out);                   // ret 8
typedef void(__fastcall* Touch_t)(DWORD node, void* edx, int slot);                          // ret 4
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);                             // ret 4
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);                       // ret 4
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);                            // ret 4
typedef void(__fastcall* RotY_t)(DWORD node, void* edx, float y);                            // ret 4
typedef void(__fastcall* Find_t)(DWORD node, void* edx, void* q);                            // ret 4
typedef void(__fastcall* Str_t)(DWORD node, void* edx, const char* s);                       // ret 4
typedef void(__fastcall* Speed_t)(DWORD layer, void* edx, float m);                          // ret 4
typedef const char*(__fastcall* Model_t)(DWORD ch, void* edx);                               // ret 0
typedef void(__cdecl* Elig_t)(DWORD thread, void* res);
typedef void(__fastcall* Flags_t)(DWORD node, void* edx, DWORD flags);                     // 0x500BE0 -> vtable +10h, ret 4

static DWORD data(DWORD node) { __try { return node ? RD32(node + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_handler(DWORD node) {   // script class object (node -> +4 type -> +28h) -> +60h handler
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool send(DWORD node, int id, void* args) { return node && gp_send(node, id, args); }
static bool native(DWORD node, int idx, void* args) {
    if (!node) return false;
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3); bool ok = true;
    __try { ((Cmd_t)(uintptr_t)P_NATIVE)(node, nullptr, idx, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    WR32(A_CUR_NODE, s1); WR32(A_CUR_2, s2); WR32(A_CUR_3, s3);
    return ok;
}
static bool trigger(DWORD node, int idx, int gid, void* args) {
    if (!node) return false;
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3); bool ok = true;
    __try { ((Trig_t)(uintptr_t)P_TRIGGER)(node, nullptr, idx, gid, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    WR32(A_CUR_NODE, s1); WR32(A_CUR_2, s2); WR32(A_CUR_3, s3);
    return ok;
}
static bool get_prop(DWORD node, int id, void* out) {
    if (!node) return false;
    __try { ((Get_t)(uintptr_t)P_GETPROP)(node, nullptr, id, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool get_member(DWORD node, int slot, void* out) {
    if (!node) return false;
    __try { ((Get_t)(uintptr_t)P_GETMEMBER)(node, nullptr, slot, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void touch(DWORD node, int slot) { __try { if (node) ((Touch_t)(uintptr_t)P_TOUCH)(node, nullptr, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static bool get_pos(DWORD node, sv::Vec3* p) {
    if (!node) return false;
    float v[3];
    __try { ((Vec_t)(uintptr_t)P_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p->x = v[0]; p->y = v[1]; p->z = v[2]; return true;
}
static bool get_fwd(DWORD node, sv::Vec3* f) {   // world direction of local +z (the Tornado's way)
    if (!node) return false;
    float b[6] = {0, 0, 0, 0, 0, 1};
    __try { ((Vec_t)(uintptr_t)P_L2W)(node, nullptr, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float l = sqrtf(b[0] * b[0] + b[2] * b[2]);
    if (!(l > 1e-4f)) return false;
    f->x = b[0] / l; f->y = 0; f->z = b[2] / l; return true;
}
static bool set_pos(DWORD node, const sv::Vec3& p) {
    if (!node) return false;
    float v[3] = {p.x, p.y, p.z};
    __try { ((Vec_t)(uintptr_t)P_SETPOS)(node, nullptr, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool set_rot_y(DWORD node, float y) {
    __try { ((RotY_t)(uintptr_t)P_SETROTY)(node, nullptr, y); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD find_child(DWORD parent, const char* name) {
    if (!parent || !name || !*name) return 0;
    struct { DWORD out; const char* name; } q = {0, name};
    __try { ((Find_t)(uintptr_t)P_FINDCHILD)(parent, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return q.out;
}
static DWORD find_path(DWORD node, const char* path) {   // "a/b/c", one child lookup per segment
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
static DWORD list_at(DWORD L, int i, DWORD type_global) {   // element i of an engine list (lists of entities / ints)
    __try {
        if (!L || i < 0 || i >= (int)RD32(L + 8)) return 0;
        DWORD k = list_stride(type_global); if (!k) return 0;
        DWORD a = RD32(L + 4); DWORD d = a ? RD32(a + 4) : 0; if (!d) return 0;
        return RD32(d + (DWORD)i * 4 * k);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD sub(DWORD root, DWORD off) { DWORD d = data(root); __try { return d ? RD32(d + off) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static void set_speed(DWORD layer, float m) { __try { if (layer) ((Speed_t)(uintptr_t)P_LAYERSPEED)(layer, nullptr, m); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static float get_speed(DWORD layer) { __try { return layer ? RDF(layer + 0x8C) : 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 1.0f; } }
static int mission_index() { DWORD cd = gp_cache_data(); __try { DWORD m = cd ? RD32(cd + C_MISSION) : 0; DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + 0x38) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }

// ---- yaw: SetRotationY writes the LOCAL rotation; the world yaw it gives is measured (two probes) so a parent
// rotation or a mirrored sign convention is handled. If the world matrix does not follow at once, assume local = world.
static bool g_yaw_probe_warned = false;
static bool set_world_yaw(DWORD node, float want) {
    if (!g_rotate || !node) return false;
    sv::Vec3 f0, f1;
    if (!set_rot_y(node, 0.0f) || !get_fwd(node, &f0)) return false;
    if (!set_rot_y(node, 1.0f) || !get_fwd(node, &f1)) return false;
    float w0 = sv::yaw_of(f0), w1 = sv::yaw_of(f1);
    float d = w1 - w0; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
    float local = want;
    if (fabsf(fabsf(d) - 1.0f) < 0.2f) local = (d > 0 ? 1.0f : -1.0f) * (want - w0);
    else if (!g_yaw_probe_warned) { g_yaw_probe_warned = true; gp_log("suave: yaw probe saw %.3f rad instead of 1 (world matrix lags?): local yaw = world yaw\n", d); }
    return set_rot_y(node, local);
}

// ================================================================================================ the move's game state
struct Who { DWORD root, ai, move, action, anim, phys, weapon, face; DWORD head, chest, pelvis; float speed0; };
static sv::Suave g_sv;
static Who   g_ram, g_partner, g_mem[sv::MAX_RING];
static DWORD g_powerup = 0, g_goodie = 0, g_effects = 0, g_ram_root0 = 0;
static int   g_mission0 = -1;
static DWORD g_vehicle_ctrl = 0, g_combat_ctrl = 0;
static float g_ground_y[1 + sv::MAX_RING]; static bool g_ground_ok[1 + sv::MAX_RING];
static bool  g_solid_pending[1 + sv::MAX_RING];   // collision off until Ram is 1.5 m away (partner 0, members 1..8)
static sv::Pair g_pair = sv::PAIR_SOLDIER;
static DWORD g_clip[sv::CLIP_COUNT];
static DWORD g_shell = 0; static float g_shell_radius0 = 3.0f; static bool g_shell_attached = false;
static int   g_shell_holder = 0;                   // 0 none, 1 Ram's hand, 2 the partner's hand, 3 strapped to his chest
static DWORD g_extra[4]; static int g_nextra = 0;  // the bundle's other sticks (held by the DLL on the chest)
static DWORD g_clav_l = 0, g_clav_r = 0;
static DWORD g_cam_node = 0, g_cam_ai = 0, g_gamecam = 0; static float g_fov0 = 64.0f; static double g_cam_switch_t = 0;
static int   g_cam_shot = 0;
static sv::Vec3 g_release_pos; static float g_release_yaw = 0; static bool g_partner_frozen = false;
static bool  g_input_lock = false; static int g_autodive_frames = 0, g_dive_key = 0x11, g_dive_check = 0;
static sv::Vec3 g_dive_from;
static sv::TurnCtl g_turn; static int g_turn_path = 0; static float g_turn_queue = 0.0f;   // 1 mouse accumulator, 2 turn_speed
static sv::Vec3 g_dive_want, g_dive_world, g_dive_cam; static bool g_dive_known = false;
static float g_dive_floor = 0.0f; static bool g_ram_snap_on = false; static int g_ram_snaps = 0;
static bool  g_rs_on = false; static float g_rs_from = 1.0f, g_rs_to = 1.0f, g_rs_len = 0.1f, g_rs_t = 0.0f, g_last_height = 0.0f;
static bool  g_floor_known = false; static float g_floor_y = 0.0f;
static volatile LONG g_cheat_press = 0, g_debug_start = 0;
static double g_last_tick = 0.0, g_mariachi_t = -100.0, g_music_restore_at = 0.0, g_last_post_tick_seen = 0.0;
static bool  g_music_borrowed = false, g_music_on = false;
static int   g_kill_mask = 0; static float g_kill_wait = -1.0f;
static uint32_t g_flags = 0;
static bool  g_pending_start = false;
static DWORD g_pending_root[1 + sv::MAX_RING]; static int g_pending_n = 0;
static DWORD R_TOUCH = P_TOUCH, R_USE_EXIT = 0x5C0FE9, R_USE_GENERIC = 0x5C0E94;
static volatile LONG g_test_force_running = 0, g_test_force_gate = -1;

static bool fill_who(DWORD root, Who* w) {
    memset(w, 0, sizeof *w);
    w->root = root;
    DWORD d = data(root); if (!d) return false;
    __try {
        w->move = RD32(d + R_MOVE); w->action = RD32(d + R_ACTION); w->anim = RD32(d + R_ANIM); w->ai = RD32(d + R_AI);
        w->phys = RD32(d + R_PHYS); w->weapon = RD32(d + R_WEAPON); w->face = RD32(d + R_FACE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (w->face && cls_handler(w->face) != H_FACE) w->face = 0;
    w->pelvis = find_path(w->anim, "Bip/Pelvis");
    w->chest = find_path(w->pelvis, "Spine/Spine1/Spine2");
    w->head = find_path(w->chest, "Spine3/Neck/Head");
    w->speed0 = get_speed(w->move);
    return true;
}
static bool npc_alive(const Who& w) {
    if (!w.root || !w.ai) return false;
    __try {
        if (RD32(data(w.root) + R_AI) != w.ai) return false;   // the pooled character was reused
        DWORD a = data(w.ai); if (!a) return false;
        return RDF(a + 0x2C) > RDF(a + 0x30) && RD32(a + 0x1D4) != 28;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ selection
static bool hostile_to_player(DWORD root, DWORD rel_list) {
    int fac = -1;
    if (!get_prop(root, 0x2A8 /*FactionID*/, &fac) || fac < 0 || fac > 255) return false;
    DWORD row = list_at(rel_list, 0, A_LIST_ENTITY);   // row 0 = the player's relations (the Tornado's test)
    if (!row) return false;
    __try {
        DWORD k = list_stride(A_LIST_INT); DWORD a = RD32(row + 4); DWORD d = a ? RD32(a + 4) : 0;
        if (!k || !d || fac >= (int)RD32(row + 8)) return false;
        return (int)RD32(d + (DWORD)fac * 4 * k) == 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// spatial query around Ram (Tornado parameters, mask 0x40 = characters), filter, choose the partner and the ring
static bool choose_now(DWORD ram, sv::Vec3 rampos, DWORD* partner, DWORD* members, int* nmem, sv::Vec3* mpos) {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    DWORD rel = 0;
    __try { DWORD ft = RD32(cd + C_FACTIONS); DWORD fd = ft ? RD32(ft + 0x18) : 0; DWORD tab = fd ? RD32(fd + 0x20) : 0; if (!get_member(tab, 0, &rel)) rel = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { rel = 0; }
    if (!rel) { gp_log("suave: faction table not readable\n"); return false; }
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rampos.x, rampos.y, rampos.z,
        g_c.pull_radius > g_c.grab_radius ? g_c.pull_radius : g_c.grab_radius, 0x40, 0};
    __try { ((Query_t)(uintptr_t)P_QUERY)(RD32(A_QUADTREE), nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("suave: query failed\n"); return false; }
    DWORD L = q.out;
    static DWORD roots[sv::MAX_CAND]; static sv::Cand cand[sv::MAX_CAND]; int n = 0;
    int cnt = list_count(L);
    for (int i = 0; i < cnt && n < sv::MAX_CAND; ++i) {
        DWORD r = list_at(L, i, A_LIST_ENTITY);
        if (!r || r == ram || cls_handler(r) != H_ROOT) continue;
        sv::Cand c; Who w;
        if (!fill_who(r, &w) || !get_pos(r, &c.pos)) continue;
        DWORD st = 0; __try { st = RD32(data(r) + R_SUBTYPE); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        c.has_ai = w.ai && cls_handler(w.ai) == H_SENTRY;
        c.alive = c.has_ai && npc_alive(w);
        c.killable = st != 8 && st != 7;
        c.hostile = c.has_ai && hostile_to_player(r, rel);
        roots[n] = r; cand[n++] = c;
    }
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (L && lt) ((Free_t)(uintptr_t)P_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    sv::Choice ch = sv::choose(g_c, rampos, cand, n);
    gp_log("suave: %d characters around Ram, partner %s, %d ring member(s)\n", n, ch.partner >= 0 ? "found" : "none", ch.n);
    if (ch.partner < 0) return false;
    *partner = roots[ch.partner];
    *nmem = ch.n;
    for (int j = 0; j < ch.n; ++j) { members[j] = roots[ch.members[j]]; mpos[j] = cand[ch.members[j]].pos; }
    return true;
}

// ================================================================================================ use-power-up gate
// UseGateStub replaces `call 0x86B4B0` (touch, thiscall ret 4, arg 4 pushed) at the start of the three use branches of
// command_use_power_up (0x5C0CB0): ecx = edi = the powerup node, esi = its data, ebx = the script thread.
// Refused: drop the return address and the pushed 4, continue at the handler's common exit 0x5C0FE9 (no charge spent).
static double g_last_refuse_log = 0;
static int __cdecl UseGateC(DWORD thread, DWORD node, DWORD pdata) {
    int slot = -1;
    __try { slot = (int)RD32(pdata + 0x1C); } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    int ok = 1;
    if (g_sv.running() || g_pending_start || g_test_force_running) {
        ok = 0;
        if (gp_now() - g_last_refuse_log > 1.0) { g_last_refuse_log = gp_now(); gp_log("suave: power-up %d refused while Suave runs\n", slot); }
    } else if (slot == 9) {
        g_mariachi_t = gp_now();
    } else if (slot == g_slot && g_installed) {
        DWORD k = list_stride(A_LIST_INT); int count = 0;
        __try { DWORD inv = RD32(pdata + 0x14); DWORD L = inv ? RD32(inv) : 0; DWORD a = L ? RD32(L + 4) : 0; DWORD d = a ? RD32(a + 4) : 0; count = d ? (int)RD32(d + (DWORD)g_slot * 4 * k) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; }
        if (count > 0) {
            struct { int res; int slot; } e = {0, g_slot};
            WR32(A_CUR_NODE, node);
            __try { ((Elig_t)(uintptr_t)P_ELIGIBLE)(thread, &e); } __except (EXCEPTION_EXECUTE_HANDLER) { e.res = 0; }
            DWORD ram = ram_root(); sv::Vec3 rp; DWORD partner = 0, mem[sv::MAX_RING]; int nm = 0; sv::Vec3 mp[sv::MAX_RING];
            if (!e.res) { ok = 0; gp_log("suave: not now (the game's move eligibility says no: dive, ladder, reload, ...)\n"); }
            else if (!ram || !get_pos(ram, &rp) || !choose_now(ram, rp, &partner, mem, &nm, mp)) { ok = 0; gp_log("suave: no enemy within %.1f m: charge kept\n", g_c.grab_radius); }
            else {
                g_powerup = node; g_pending_start = true;
                g_pending_root[0] = partner; for (int j = 0; j < nm; ++j) g_pending_root[1 + j] = mem[j]; g_pending_n = nm;
                gp_log("suave: approved on slot %d (charges %d -> %d by the game's own decrement%s)\n", g_slot, count, count - 1,
                       g_slot == 11 ? "" : ", the slot's own effect skipped");
                if (g_slot != 11) ok = 2;   // continue in the generic branch: decrement + selection refresh, no Pinata
            }
        }
    }
    // put the script globals back only where they changed (the eligibility call sets the current node)
    if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
    if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
    if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
    if (g_test_force_gate >= 0) return g_test_force_gate;   // test host only
    return ok;
}
static volatile int g_gate_ok = 1;
#define SAVE_XMM  __asm sub esp, 0x80 __asm movdqu [esp+0x00], xmm0 __asm movdqu [esp+0x10], xmm1 __asm movdqu [esp+0x20], xmm2 \
                  __asm movdqu [esp+0x30], xmm3 __asm movdqu [esp+0x40], xmm4 __asm movdqu [esp+0x50], xmm5 \
                  __asm movdqu [esp+0x60], xmm6 __asm movdqu [esp+0x70], xmm7
#define LOAD_XMM  __asm movdqu xmm0, [esp+0x00] __asm movdqu xmm1, [esp+0x10] __asm movdqu xmm2, [esp+0x20] \
                  __asm movdqu xmm3, [esp+0x30] __asm movdqu xmm4, [esp+0x40] __asm movdqu xmm5, [esp+0x50] \
                  __asm movdqu xmm6, [esp+0x60] __asm movdqu xmm7, [esp+0x70] __asm add esp, 0x80
__declspec(naked) void UseGateStub() {
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm push esi
    __asm push edi
    __asm push ebx
    __asm call UseGateC
    __asm add esp, 12
    __asm mov dword ptr [g_gate_ok], eax
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm cmp dword ptr [g_gate_ok], 0
    __asm je refuse
    __asm cmp dword ptr [g_gate_ok], 2
    __asm je generic
    __asm jmp dword ptr [R_TOUCH]
generic:                                  // slot 10: touch(4), then return into the generic branch after its own touch
    __asm push eax
    __asm mov eax, dword ptr [R_USE_GENERIC]
    __asm mov dword ptr [esp + 4], eax
    __asm pop eax
    __asm jmp dword ptr [R_TOUCH]
refuse:
    __asm add esp, 8
    __asm jmp dword ptr [R_USE_EXIT]
}

// ================================================================================================ collision ray
// The game's line-of-sight test Navigator::IsPointOccluded 0x91D020: thiscall(this, float from[4], float to[4]) ret 8,
// al = blocked. It sets up the scene's shared CollisionProbe (Scene +A8h) over the segment's bounding sphere, ignores
// the entity at this +70h, mask 21h, then CollisionProbe::IsLineColliding (0x8B7000, the bullets' test). The hit
// distance comes from a bisection on the segment (7 steps: 1/128 of its length).
// (Fifth-test lesson: the script native's helper 0x892FC0 is GetClosestNode over the sphere - distance 0 whenever the
// ground is inside it - not a ray.)
typedef BYTE(__fastcall* Occl_t)(void* self, void* edx, const float* from, const float* to);
static bool g_ray_ok = true; static int g_ray_logs = 0;
struct RayHit { bool hit; float dist; DWORD node; };
static bool blocked(const sv::Vec3& a, const sv::Vec3& b, DWORD ignore) {
    static BYTE self[0x80];
    *(DWORD*)(self + 0x70) = ignore;
    float f[4] = {a.x, a.y, a.z, 0.0f}, t[4] = {b.x, b.y, b.z, 0.0f};
    BYTE h = 0;
    __try { if (!RD32(A_SCENE) || !RD32(RD32(A_SCENE) + 0xA8)) return false; h = ((Occl_t)(uintptr_t)P_OCCLUDED)(self, nullptr, f, t); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_ray_ok = false; gp_log("suave: line test faulted: rays off\n"); return false; }
    return h != 0;
}
static RayHit ray(const sv::Vec3& a, const sv::Vec3& b, bool log = false, DWORD ignore = 0) {
    RayHit r = {false, sv::dist(a, b), 0};
    float len = r.dist;
    if (!g_ray_ok || len < 0.05f) return r;
    if (!blocked(a, b, ignore)) {
        if (log || (g_verbose && g_ray_logs < 30)) { ++g_ray_logs; gp_log("suave: ray (%.2f %.2f %.2f)->(%.2f %.2f %.2f) clear (%.1f m)\n", a.x, a.y, a.z, b.x, b.y, b.z, len); }
        return r;
    }
    sv::Vec3 dir = sv::mul(sv::sub(b, a), 1.0f / len);
    float lo = 0.0f, hi = len;
    for (int i = 0; i < 7 && g_ray_ok; ++i) {
        float mid = 0.5f * (lo + hi);
        if (blocked(a, sv::add(a, sv::mul(dir, mid)), ignore)) hi = mid; else lo = mid;
    }
    r.hit = true; r.dist = lo;
    if (log || (g_verbose && g_ray_logs < 30)) { ++g_ray_logs; gp_log("suave: ray (%.2f %.2f %.2f)->(%.2f %.2f %.2f) blocked at %.2f m\n", a.x, a.y, a.z, b.x, b.y, b.z, lo); }
    return r;
}
static float free_along(const sv::Vec3& from, const sv::Vec3& dir, float len, DWORD ignore = 0) { RayHit h = ray(from, sv::add(from, sv::mul(dir, len)), false, ignore); return h.hit ? h.dist : len; }

// ================================================================================================ camera
static DWORD target_node(int t, sv::Vec3* off) {
    *off = sv::v3(0, 0, 0);
    const Who& w = t <= sv::TGT_RAM_PELVIS ? g_ram : g_partner;
    int part = t % 3;   // 0 head, 1 chest, 2 pelvis
    DWORD n = part == 0 ? w.head : part == 1 ? w.chest : w.pelvis;
    if (n) return n;
    *off = sv::v3(0, part == 0 ? 1.6f : part == 1 ? 1.25f : 0.9f, 0);   // no bone: the root (at the feet) + height
    return w.root;
}
// Camera kept out of walls (fifth test, rooms): the eye is pulled in along the target -> eye line to just before a
// wall; when that leaves less than 1.6 m (inside the characters - test-copy run v14), the shot is mirrored to the other
// side of its target for the whole shot (g_cam_mirror), if that side is more open.
static bool g_cam_mirror = false;
static float clear_len(const sv::Vec3& tp, const sv::Vec3& dir, float len, DWORD ignore) {
    sv::Vec3 from = sv::add(tp, sv::mul(dir, 0.45f));   // start outside the target's own body
    RayHit h = ray(from, sv::add(tp, sv::mul(dir, len)), false, ignore);
    if (!h.hit) return len;
    float keep = 0.45f + h.dist - 0.35f;
    return keep < 0.8f ? 0.8f : (keep > len ? len : keep);
}
static sv::Vec3 clamp_eye(int tgt, const sv::Vec3& eye, bool decide = false) {
    if (!g_cam_clamp || !g_ray_ok) return eye;
    sv::Vec3 off; DWORD tn = target_node(tgt, &off); sv::Vec3 tp;
    if (!tn || !get_pos(tn, &tp)) return eye;
    tp = sv::add(tp, off);
    float len = sv::dist(tp, eye); if (len < 0.6f) return eye;
    sv::Vec3 d = sv::mul(sv::sub(eye, tp), 1.0f / len), m = sv::v3(-d.x, d.y, -d.z);
    DWORD ign = tgt <= sv::TGT_RAM_PELVIS ? g_ram.root : g_partner.root;
    if (decide) {
        float k1 = clear_len(tp, d, len, ign);
        g_cam_mirror = false;
        if (k1 < len - 0.01f && k1 < 1.6f) { float k2 = clear_len(tp, m, len, ign); g_cam_mirror = k2 > k1 + 0.3f; }
    }
    sv::Vec3 dir = g_cam_mirror ? m : d;
    return sv::add(tp, sv::mul(dir, clear_len(tp, dir, len, ign)));
}
static void cam_switch(int shot, int tgt, const sv::Vec3& eye0, float blend, float fov) {
    if (!g_cam_node || !g_cam_ai) return;
    sv::Vec3 eye = clamp_eye(tgt, eye0, true);
    if (sv::dist(eye, eye0) > 0.05f) gp_log("suave: camera %s %s (a wall between the eye and the target)\n", sv::shot_name(shot), g_cam_mirror ? "mirrored to the open side" : "pulled in");
    set_pos(g_cam_node, eye);
    sv::Vec3 off; DWORD tn = target_node(tgt, &off);
    struct { DWORD tgt; float off[3]; } la = {tn, {off.x, off.y, off.z}};
    if (tn) send(g_cam_ai, MSG_CAM_LOOK_AT, &la); else send(g_cam_ai, MSG_CAM_STOP_LOOK, nullptr);
    if (blend <= 0.0f) { struct { DWORD logic, pivot; } a = {g_cam_node, 0}; send(g_cam_ai, MSG_CAM_CUT_TO, &a); }
    else { struct { DWORD logic, pivot; float in, out, total; } a = {g_cam_node, 0, blend * 0.5f, blend * 0.5f, blend}; send(g_cam_ai, MSG_CAM_TRANSITION, &a); }
    struct { float fov, secs; } f = {fov, blend > 0 ? blend : 0.0f};
    send(g_gamecam, MSG_FOV, &f);
    g_cam_switch_t = gp_now(); g_cam_shot = shot;
    gp_log("suave: camera %s (%s, fov %.0f)\n", sv::shot_name(shot), blend > 0 ? "blend" : "cut", fov);
    if (g_debug_shots) { static int k = 0; gp_log("debug keys: mark shot sv_%02d_%s\n", ++k, sv::shot_name(shot)); }
    if (g_debug_shots && shot == sv::SH_LOOKDOWN) gp_log("debug keys: mark burst sv_panic 3 400\n");
}
static bool cam_take() {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try {
        g_gamecam = RD32(cd + C_GAMECAM); g_cam_ai = g_gamecam ? RD32(RD32(g_gamecam + 0x18) + 0x14) : 0;
        DWORD pai = RD32(cd + C_PLAYER_AI); g_cam_node = pai ? RD32(RD32(pai + 0x18) + 0x114) : 0;
        g_fov0 = g_gamecam ? RDF(g_gamecam + 0xB4) : 64.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cam_ai = g_cam_node = 0; }
    if (!g_cam_ai || !g_cam_node) { gp_log("suave: camera nodes not found: no camera shots\n"); return false; }
    if (!(g_fov0 > 20.0f && g_fov0 < 130.0f)) g_fov0 = 64.0f;
    send(g_cam_ai, MSG_CAM_BT_ON, nullptr);   // the camera runs on real time (the freeze, the slow motion)
    return true;
}
static void cam_release(float blend) {
    if (!g_cam_ai) return;
    DWORD ram = ram_root();
    send(g_cam_ai, MSG_CAM_STOP_LOOK, nullptr);
    if (blend <= 0.0f) { struct { DWORD logic, pivot; } a = {ram, 0}; send(g_cam_ai, MSG_CAM_CUT_TO, &a); }
    else { struct { DWORD logic, pivot; float in, out, total; } a = {ram, 0, blend * 0.5f, blend * 0.5f, blend}; send(g_cam_ai, MSG_CAM_TRANSITION, &a); }
    struct { float fov, secs; } f = {g_fov0, blend > 0 ? blend : 0.01f};
    send(g_gamecam, MSG_FOV, &f);
    send(g_cam_ai, MSG_CAM_BT_OFF, nullptr);
    gp_log("suave: camera back to gameplay (blend %.2f s, fov %.1f)\n", blend, g_fov0);
    g_cam_ai = g_cam_node = 0; g_cam_shot = 0;
}
static bool cam_lost() {
    if (!g_cam_ai || !g_cam_node || g_cam_shot == 0 || gp_now() - g_cam_switch_t < 0.5) return false;
    __try { DWORD d = RD32(g_cam_ai + 0x18); return d && RD32(d + 0x08) != g_cam_node; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ music
static DWORD music_slot_node() {
    DWORD cd = gp_cache_data(); __try { return cd ? list_at(RD32(cd + C_SOUNDDICT), g_music_slot, A_LIST_ENTITY) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD ambient_sound() { DWORD cd = gp_cache_data(); __try { DWORD m = cd ? RD32(cd + C_MASTER) : 0; return m ? RD32(RD32(m + 0x18) + 0x3C) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool set_stream(DWORD slot, const char* path) {
    __try { ((Str_t)(uintptr_t)P_SETSTREAM)(slot, nullptr, path); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void music_start(int seg) {
    DWORD slot = music_slot_node(), snd = ambient_sound();
    if (!slot || !snd) { gp_log("suave: music slot %d / sound ctrl not found: no music\n", g_music_slot); return; }
    if (gp_now() - g_mariachi_t < 20.0) { gp_log("suave: El Mariachi's stream may still play on slot %d: no Suave music this time\n", g_music_slot); return; }
    char path[128]; sprintf_s(path, g_music_fmt, seg);
    if (!set_stream(slot, path)) { gp_log("suave: streaming_sound setter failed\n"); return; }
    g_music_borrowed = true;
    DWORD cd = gp_cache_data(); DWORD speaker = 0;
    __try { if (g_music_duck || RD32(cd + C_COMBOMUSIC) != 0 || RD32(data(snd) + 0xC4) != 0) speaker = ram_root(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_music_kill_combo) { send(snd, MSG_KILL_COMBO_MUSIC, nullptr); gp_log("suave: combo music streams stopped\n"); }
    if (g_music_pause_level && !g_level_paused) { g_level_paused = send(snd, MSG_PAUSE_MISSION_SOUND, nullptr); gp_log("suave: mission sound emitters paused%s\n", g_level_paused ? "" : " (FAILED)"); }
    struct { DWORD slot, speaker; } a = {slot, speaker};
    bool ok = send(snd, MSG_PLAY_SPEAK, &a);
    g_music_on = ok; g_music_restore_at = gp_now() + g_music_len + 1.0;
    if (ok) g_flags |= TODGP_SV_MUSIC;
    gp_log("suave: music segment %d (%s) on SoundDict %d, speaker %s%s\n", seg, path, g_music_slot, speaker ? "player (combo music on)" : "none", ok ? "" : " (FAILED)");
}
static void music_stop() {
    DWORD slot = music_slot_node(), snd = ambient_sound();
    if (g_music_on && slot && snd) { struct { DWORD slot; } a = {slot}; send(snd, MSG_STOP_SPEAK, &a); gp_log("suave: music stopped\n"); }
    g_music_on = false;
    if (g_music_borrowed) g_music_restore_at = gp_now() + 1.0;   // the stream needs a moment to close
}
static void music_level_resume() {
    if (!g_level_paused) return;
    DWORD snd = ambient_sound();
    send(snd, MSG_RESUME_MISSION_SOUND, nullptr);
    g_level_paused = false;
    gp_log("suave: mission sound emitters resumed\n");
}
static void music_restore_if_due(double now) {
    if (!g_music_borrowed || now < g_music_restore_at) return;
    DWORD slot = music_slot_node();
    if (slot && set_stream(slot, g_music_restore)) gp_log("suave: SoundDict %d back to %s\n", g_music_slot, g_music_restore);
    g_music_borrowed = false; g_music_on = false;
}

// ================================================================================================ helpers for actions
static Who* who_of(int w) { return w < 0 ? &g_ram : w == 0 ? &g_partner : (w <= sv::MAX_RING ? &g_mem[w - 1] : nullptr); }
static DWORD clip_slot(int clip) {
    if (clip < 0 || clip >= sv::CLIP_COUNT) return 0;
    if (g_clip[clip]) return g_clip[clip];
    // "@camera:<path>" = under the GameCamera node (cache +260h): the suave_anims data option creates the slots at the
    // end of GameCamera.fragment so no existing node changes its creation index (saves). Else under the AnimDict root;
    // the other place is tried too, so either data layout works.
    DWORD animdict = 0, camera = 0, cd = gp_cache_data();
    __try { DWORD h = RD32(A_PLAYER_HOLDER); animdict = h ? RD32(RD32(h + 0x18) + 0x9C) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { animdict = 0; }
    __try { camera = cd ? RD32(cd + C_GAMECAM) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { camera = 0; }
    char name[64]; sprintf_s(name, g_clip_fmt[clip], sv::pair_name(g_pair));
    bool at_cam = !strncmp(g_clip_root, "@camera:", 8);
    const char* path = at_cam ? g_clip_root + 8 : g_clip_root;
    DWORD dir = find_path(at_cam ? camera : animdict, path);
    g_clip[clip] = find_child(dir, name);
    if (!g_clip[clip]) {
        g_clip[clip] = at_cam ? find_child(find_path(animdict, "PlayerAnims/specials/Suave"), name) : find_child(find_path(camera, "Suave"), name);
        if (g_clip[clip] && g_verbose) gp_log("suave: clip %s found at the other place\n", name);
    }
    if (!g_clip[clip]) gp_log("suave: clip %s/%s not found (the suave_anims data option installed?): not played\n", g_clip_root, name);
    return g_clip[clip];
}
static void play(int w, int clip, float pos) {
    Who* x = who_of(w); if (!x || !x->move) return;
    DWORD s = clip_slot(clip); if (!s) return;
    struct { DWORD slot; int ev; } m = {s, -1};
    native(x->move, NAT_MOTION, &m);
    if (pos > 0.0f && g_setplaypos) { float p = pos; native(x->move, NAT_SETPLAYPOS, &p); }
}
static void face(int w, int expr, float stay) {
    Who* x = who_of(w); if (!x || !x->face) return;
    if (g_face_force) { struct { int e; float t; int next; } d = {sv::FACE_DEFAULT, 0.0f, 0}; send(x->face, MSG_FACE, &d); }
    struct { int e; float t; int next; } f = {expr, stay, 0};
    send(x->face, MSG_FACE, &f);
}
static void park(DWORD move) { struct { int ctrl, state; } r = {3, 22}; send(move, MSG_MOVE_NEW_STATE, &r); }   // Victim_Move_ctrl state 22
static void unpark(DWORD move) { struct { int ctrl, state; } r = {0, 0}; send(move, MSG_MOVE_NEW_STATE, &r); send(move, MSG_MOVE_IDLE, nullptr); }
static void ai_off(const Who& w) { send(w.ai, MSG_AI_NO_CONTROL, nullptr); }
static void ai_on(const Who& w) {
    __try { DWORD a = data(w.ai); if (a && RDF(a + 0x264) > 0.0f) WRF(a + 0x264, 0.0f); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // KnockedOutTimer
    send(w.ai, MSG_AI_REGAIN, nullptr);
}
static void move_auto(DWORD move, bool on, const sv::Vec3* p) {   // MoveCtrl auto_move_to_* (slots 28..30)
    DWORD d = data(move); if (!d) return;
    __try {
        touch(move, 29); WR32(d + 0xBC, 0);
        if (p) { touch(move, 30); WRF(d + 0xC0, p->x); WRF(d + 0xC4, p->y); WRF(d + 0xC8, p->z); }
        touch(move, 28); WR32(d + 0xB8, on ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void place_partner() {
    sv::Vec3 rp, rf; if (!get_pos(g_ram.root, &rp) || !get_fwd(g_ram.root, &rf)) return;
    set_pos(g_partner.root, g_partner_frozen ? g_release_pos : rp);
    set_world_yaw(g_partner.root, g_partner_frozen ? g_release_yaw : sv::yaw_of(rf));
    send(g_partner.phys, MSG_PHYS_RESET, nullptr);
    send(g_partner.phys, MSG_PHYS_NO_COLLISION, nullptr);
}

// ---- dynamite (the game's weapon_shell, type 3)
static DWORD hand_node(const Who& w, const char* hand) {   // the hand bone (for the log): Biped "<s> Clavicle/<s> UpperArm/..."
    if (!w.anim || !hand || !*hand) return 0;
    DWORD n = find_child(w.anim, hand); if (n) return n;
    char path[128]; char s = hand[0];
    sprintf_s(path, "Spine3/%c Clavicle/%c UpperArm/%c Forearm/%s", s, s, s, hand);
    return find_path(w.chest, path);
}
static void shell_line(const char* tag) {   // shown / armed / fuse / where it is against the hand it should follow
    if (!g_shell) return;
    DWORD fl = 0, ready = 0, inaim = 0, ign = 0, owner = 0;
    __try {
        fl = RD32(g_shell + 0x28) & 0xFFF; DWORD d = data(g_shell);
        if (d) { ready = RD32(d + 0x1C); inaim = RD32(d + 0x28); ign = RD32(d + 0x80); owner = RD32(d + 0x24); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const Who* w = g_shell_holder == 1 ? &g_ram : (g_shell_holder == 2 ? &g_partner : nullptr);
    const char* hn = g_shell_holder == 1 ? g_dyn_ram_hand : g_dyn_partner_hand;
    sv::Vec3 sp, hp; bool sok = get_pos(g_shell, &sp), hok = w && get_pos(hand_node(*w, hn), &hp);
    if (g_shell_holder == 3) {
        sv::Vec3 cp; bool cok = get_pos(g_partner.chest, &cp);
        gp_log("suave: dynamite %s: STRAPPED to the partner's chest (%d sticks), flags %03X (%s), Ready %u InAim %u (%s), ignition effect %s, at (%.2f %.2f %.2f), %.2f m from Spine2\n",
               tag, 1 + g_nextra, fl, (fl & 5) ? "HIDDEN" : "shown", ready, inaim, (!ready && inaim) ? "armed, fuse lit" : "NOT armed", ign ? "loaded" : "none",
               sp.x, sp.y, sp.z, cok && sok ? sv::dist(sp, cp) : -1.0f);
        return;
    }
    char hand[128] = "";
    if (hok) sprintf_s(hand, ", %s at (%.2f %.2f %.2f): %.2f m%s", hn, hp.x, hp.y, hp.z, sv::dist(sp, hp), sv::dist(sp, hp) < 0.35f ? " (attached)" : " (NOT at the hand)");
    else if (w) sprintf_s(hand, ", hand bone '%s' not found", hn);
    gp_log("suave: dynamite %s: %s, flags %03X (%s), Ready %u InAim %u (%s), ignition effect %s, owner %s, at (%.2f %.2f %.2f)%s%s\n",
           tag, g_shell_holder == 1 ? "in Ram's hand" : (g_shell_holder == 2 ? "in the partner's hand" : "not attached"), fl,
           (fl & 5) ? "HIDDEN" : "shown", ready, inaim, (!ready && inaim) ? "armed, fuse lit" : "NOT armed", ign ? "loaded" : "none",
           owner == g_partner.root ? "partner" : (owner == ram_root() ? "Ram" : "other"), sp.x, sp.y, sp.z, sok ? "" : " (no position)", hand);
}
// Node flag 4 HIDES the shell's model: the explode handler sets it after the blast (0x848B7E) and command_release_grenade
// clears it (0x62E1B0 with 4) when a grenade leaves the hand - a pooled shell keeps it from its last blast (906 in every
// test: only the fuse sparks showed, test-copy run v7). show() = flag 1 clear (0x62E110) and flag 4 clear.
static void shell_show() {
    __try {
        DWORD fl = RD32(g_shell + 0x28) & 0xFFF, nf = fl & ~5u;
        if (nf != fl) ((Flags_t)(uintptr_t)P_SETFLAGS)(g_shell, nullptr, nf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void shell_to(int holder) {   // 0 detach, 1 Ram's hand, 2 the partner's hand (the game's grenade hold)
    if (!g_shell) { g_shell_holder = 0; return; }
    int slot = g_dyn_slot;
    if (g_shell_holder == 1 && g_ram.anim) trigger(g_ram.anim, NAT_DETACH, GID_DETACH, &slot);
    if (g_shell_holder == 2 && g_partner.anim) trigger(g_partner.anim, NAT_DETACH, GID_DETACH, &slot);
    g_shell_holder = 0;
    if (holder) {
        const Who& w = holder == 1 ? g_ram : g_partner;
        const char* hand = holder == 1 ? g_dyn_ram_hand : g_dyn_partner_hand;
        struct { int slot; DWORD node; const char* pivot; const char* bone; } a = {g_dyn_slot, g_shell, g_dyn_grip, hand};
        if (w.anim && trigger(w.anim, NAT_ATTACH, GID_ATTACH, &a)) g_shell_holder = holder;
        shell_show();
        gp_log("suave: dynamite -> %s's %s (slot %d, grip '%s')%s\n", holder == 1 ? "Ram" : "the partner", hand, g_dyn_slot, g_dyn_grip,
               g_shell_holder ? "" : " (FAILED)");
        shell_line(holder == 1 ? "shown by Ram" : "handed over");
    }
    g_shell_attached = g_shell_holder != 0;
    if (g_shell_attached) g_flags |= TODGP_SV_DYNAMITE; else g_flags &= ~TODGP_SV_DYNAMITE;
}
static void set_hidden(DWORD n, bool hide) {
    __try {
        DWORD fl = RD32(n + 0x28) & 0xFFF, nf = hide ? (fl | 1) : (fl & ~5u);
        if (nf != fl) ((Flags_t)(uintptr_t)P_SETFLAGS)(n, nullptr, nf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static DWORD arm_one(DWORD L, int n, DWORD skip_upto) {   // a free shell -> dynamite, aimed (Ready 0, InAim 1, fuse sparks)
    for (int i = 0; i < n; ++i) {
        DWORD s = list_at(L, i, A_LIST_ENTITY);
        if (!s || cls_handler(s) != H_SHELL) continue;
        bool used = s == g_shell; for (int k = 0; k < g_nextra; ++k) used |= s == g_extra[k];
        if (used) continue;
        DWORD d = data(s); bool ready = false;
        __try { ready = d && RD32(d + 0x1C) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { ready = false; }
        if (!ready) continue;
        __try { WR32(d + 0x24, g_partner.root); if (!g_shell) g_shell_radius0 = RDF(d + 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        int type = 3; send(s, MSG_SHELL_MORPH, &type);   // the dynamite model + show() (0x8486D0)
        sv::Vec3 p; get_pos(g_partner.chest ? g_partner.chest : g_partner.root, &p);
        float v[3] = {p.x, p.y, p.z}; send(s, MSG_SHELL_AIM, v);   // owner not the player: Ready 0, InAim 1, the fuse sparks
        (void)skip_upto;
        return s;
    }
    return 0;
}
// the body's forward from the shoulder line (Biped L / R clavicles), signed toward Ram: the partner faces Ram through
// the push, the stumble and the look-down, while his root keeps the shared node's yaw (Ram's) - test-copy run v5 put
// the bundle on his back with the root's sign
static bool chest_frame(sv::Vec3* chest, sv::Vec3* fwd, sv::Vec3* right) {
    if (!get_pos(g_partner.chest, chest)) return false;
    sv::Vec3 rp, rf; bool rok = get_pos(g_ram.root, &rp);
    if (rok) { rf = sv::flat_dir(*chest, rp, sv::v3(0, 0, 1)); rok = sv::dist_xz(*chest, rp) > 0.15f; }
    if (!rok) rok = get_fwd(g_partner.root, &rf);
    if (!g_clav_l) g_clav_l = find_path(g_partner.chest, "Spine3/L Clavicle");
    if (!g_clav_r) g_clav_r = find_path(g_partner.chest, "Spine3/R Clavicle");
    sv::Vec3 l, r;
    if (get_pos(g_clav_l, &l) && get_pos(g_clav_r, &r)) {
        sv::Vec3 s = sv::sub(r, l); s.y = 0; float sl = sqrtf(s.x * s.x + s.z * s.z);
        if (sl > 0.05f) {
            sv::Vec3 f = sv::v3(s.z / sl, 0, -s.x / sl);
            if (rok && f.x * rf.x + f.z * rf.z < 0.0f) f = sv::mul(f, -1.0f);
            *fwd = f; *right = sv::v3(f.z, 0, -f.x);
            return true;
        }
    }
    if (!rok) return false;
    *fwd = rf; *right = sv::v3(rf.z, 0, -rf.x);
    return true;
}
static void strap_frame() {   // every frame: the bundle sits in front of the partner's chest, side by side
    if (!g_shell) return;
    sv::Vec3 c, f, r; if (!chest_frame(&c, &f, &r)) return;
    sv::Vec3 base = sv::add(sv::add(c, sv::mul(f, g_strap_out)), sv::v3(0, -g_strap_drop, 0));
    int total = (g_shell_holder == 3 ? 1 : 0) + g_nextra, k = 0;
    float yaw = sv::yaw_of(f) + 1.5707963f;   // the sticks across the chest
    for (int i = -1; i < g_nextra; ++i) {
        DWORD s = i < 0 ? (g_shell_holder == 3 ? g_shell : 0) : g_extra[i];
        if (!s) continue;
        float off = (k - (total - 1) * 0.5f) * g_strap_gap; ++k;
        set_pos(s, sv::add(base, sv::mul(r, off)));
        set_rot_y(s, yaw);
    }
}
static void dynamite_arm() {   // the main stick in Ram's hand (he shows it); the rest of the bundle armed, hidden
    DWORD cd = gp_cache_data(); DWORD L = 0;
    __try { L = cd ? RD32(cd + C_SHELLS) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { L = 0; }
    int n = list_count(L);
    g_nextra = 0; g_clav_l = g_clav_r = 0;
    DWORD s = arm_one(L, n, 0);
    if (!s) { gp_log("suave: no free shell in the pool (%d): no dynamite prop; the kill pass still runs\n", n); return; }
    g_shell = s;
    gp_log("suave: dynamite shell armed (fuse sparks)\n");
    shell_to(1);
    for (int k = 1; k < g_dyn_sticks && g_nextra < 4; ++k) {
        DWORD e = arm_one(L, n, 0); if (!e) break;
        g_extra[g_nextra++] = e; set_hidden(e, true);
    }
    gp_log("suave: dynamite bundle: %d stick(s) (%d extra, hidden until the plant)\n", 1 + g_nextra, g_nextra);
}
static void dynamite_attach() {   // the plant at the push contact: off Ram's hand, strapped to the partner's chest
    if (!g_shell) return;
    shell_to(0);
    g_shell_holder = 3; g_shell_attached = true; g_flags |= TODGP_SV_DYNAMITE;
    shell_show();
    for (int k = 0; k < g_nextra; ++k) set_hidden(g_extra[k], false);
    strap_frame();
    shell_line("strapped");
    if (g_debug_shots) gp_log("debug keys: mark burst sv_strapped 2 300\n");
}
static void bundle_release() {   // the extra sticks back to the pool (goto_inactive)
    for (int k = 0; k < g_nextra; ++k) if (g_extra[k]) { send(g_extra[k], MSG_SHELL_INACTIVE, nullptr); g_extra[k] = 0; }
    g_nextra = 0;
}
static void dynamite_detach() {
    if (g_shell_holder == 3) { g_shell_holder = 0; g_shell_attached = false; g_flags &= ~TODGP_SV_DYNAMITE; }
    else shell_to(0);
}
static void restore_shell() {
    if (!g_shell) return;
    __try { DWORD d = data(g_shell); if (d) WRF(d + 0x38, g_shell_radius0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_shell = 0;
}
static void boom(int mask) {
    sv::Vec3 c; if (!get_pos(g_partner.chest ? g_partner.chest : g_partner.root, &c)) get_pos(g_ram.root, &c);
    DWORD ram = ram_root();
    if (g_shell) {
        __try { DWORD d = data(g_shell); if (d) { WR32(d + 0x24, ram); WRF(d + 0x38, g_c.boom_radius); } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        struct { int explode, water; float v[3]; } e = {1, 0, {0, 0, 0}};
        bool ok = send(g_shell, MSG_SHELL_EXPLODE, &e);
        gp_log("suave: BOOM - dynamite explode (radius %.1f m, owner Ram)%s\n", g_c.boom_radius, ok ? "" : " (FAILED)");
        if (g_debug_shots) gp_log("debug keys: mark burst sv_boom 4 100\n");
        dynamite_detach(); bundle_release();
    } else gp_log("suave: BOOM - no shell, kill pass only\n");
    g_flags |= TODGP_SV_BOOM;
    DWORD cd = gp_cache_data();
    if (g_c.shake && g_cam_ai == 0) {   // the shake goes to the gameplay camera's AI (it exists while our shots run too)
        __try { DWORD gc = RD32(cd + C_GAMECAM); DWORD ai = gc ? RD32(RD32(gc + 0x18) + 0x14) : 0; struct { float t, amp; DWORD src; } s = {0.6f, 1.0f, g_partner.root}; send(ai, MSG_CAM_SHAKE, &s); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else if (g_c.shake) { struct { float t, amp; DWORD src; } s = {0.6f, 1.0f, g_partner.root}; send(g_cam_ai, MSG_CAM_SHAKE, &s); }
    if (g_c.boom_slowmo > 0.0f) { float sz = g_c.boom_slowmo; __try { send(RD32(cd + C_TIMECTRL), MSG_TIME_EXPLOSION, &sz); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    g_kill_mask = mask; g_kill_wait = g_c.dynamite && g_shell ? g_kill_delay : 0.0f;
    (void)c;
}
static void kill_pass() {
    sv::Vec3 c; if (!get_pos(g_partner.chest ? g_partner.chest : g_partner.root, &c)) return;
    DWORD ram = ram_root(); int killed = 0, hit = 0;
    for (int b = 0; b <= sv::MAX_RING; ++b) {
        if (!(g_kill_mask & (1 << b))) continue;
        const Who& w = b == 0 ? g_partner : g_mem[b - 1];
        if (!npc_alive(w)) continue;
        sv::Vec3 p; if (!get_pos(w.root, &p)) continue;
        if (b > 0 && sv::dist(p, c) > g_c.boom_radius) continue;   // walked off: out of the blast
        DWORD vol = 0; get_prop(w.phys, 0x69D /*torso_cs*/, &vol);
        sv::Vec3 d = sv::sub(p, c); d.y = 0; float l = sqrtf(d.x * d.x + d.z * d.z);
        if (l < 0.01f) { d = sv::v3(0, 0, 1); l = 1; }
        float k = (g_c.boom_radius - sv::dist(p, c)) / g_c.boom_radius; if (k < 0.3f) k = 0.3f;
        float sp = g_c.boom_impulse * k * g_boom_mass;
        struct { float dmg; DWORD owner, inflictor, volume; float imp[3]; float pos[3]; int type; } h =
            {g_c.boom_damage, ram, g_shell ? g_shell : ram, vol, {d.x / l * sp, 0.6f * sp, d.z / l * sp}, {p.x, p.y + 1.0f, p.z}, 1};
        ++hit;
        if (send(w.root, MSG_ROOT_TAKE_A_HIT, &h)) ++killed;
    }
    gp_log("suave: kill pass: %d character(s) hit (owner Ram, %.0f damage, explosive)\n", hit, g_c.boom_damage);
    g_kill_mask = 0;
}

// ================================================================================================ action executor
// Fifth test: "the gun stays in the air and sometimes keeps firing" - force_drop_weapon only clears the inventory slot
// (0x618C60) and leaves the gun attached where the hand was, with its fire state running. Mode 2 now uses the gun's own
// drop (default_weapon command_drop 0x667FC0: seize fire, detach from the hand, the dropped state falls to the ground)
// after command_stop_firing, then the inventory drop; mode 3 hides the gun (show() back for survivors).
static DWORD g_hidden_gun[1 + sv::MAX_RING];
static void weapon_off(const Who& w, int b) {
    if (g_weapon_mode < 1 || !w.weapon) return;
    DWORD prim = 0; __try { DWORD wd = data(w.weapon); prim = wd ? RD32(wd + 0x20) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { prim = 0; }
    bool gun = prim && cls_handler(prim) == H_WEAPON;
    if (gun) { send(prim, MSG_GUN_STOP_FIRING, nullptr); send(prim, MSG_GUN_KILL_EFFECTS, nullptr); }
    send(w.weapon, MSG_WEAPON_UNARMED, nullptr);
    if (g_weapon_mode == 2 && prim) {
        if (gun) send(prim, MSG_GUN_DROP, nullptr);   // the game's own drop: it falls
        DWORD a = prim; send(w.weapon, MSG_WEAPON_DROP, &a);
        gp_log("suave: %s %d: gun %08X %s\n", b ? "member" : "partner", b, prim, gun ? "stops firing and drops (the gun's own drop)" : "dropped (not a default_weapon)");
    } else if (g_weapon_mode == 3 && prim) {
        __try { DWORD fl = RD32(prim + 0x28) & 0xFFF; if (!(fl & 1)) ((Flags_t)(uintptr_t)P_SETFLAGS)(prim, nullptr, fl | 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (b >= 0 && b <= sv::MAX_RING) g_hidden_gun[b] = prim;
        gp_log("suave: %s %d: gun %08X stops firing and is hidden\n", b ? "member" : "partner", b, prim);
    }
}
static void weapon_back(const Who& w, int b) {
    if (b >= 0 && b <= sv::MAX_RING && g_hidden_gun[b]) {
        DWORD g = g_hidden_gun[b]; g_hidden_gun[b] = 0;
        __try { DWORD fl = RD32(g + 0x28) & 0xFFF; if (fl & 1) ((Flags_t)(uintptr_t)P_SETFLAGS)(g, nullptr, fl & 0xFFE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    send(w.weapon, MSG_WEAPON_BEST, nullptr);
}
static void ground_snap() {   // a parked character never drops below its ground height (test 2: the partner fell)
    sv::Vec3 rp; bool rok = get_pos(g_ram.root, &rp);
    for (int b = 0; b <= sv::MAX_RING && rok; ++b) {   // collision back once Ram has left (test 3: overlapping pushed him down)
        if (!g_solid_pending[b]) continue;
        const Who& w = b == 0 ? g_partner : g_mem[b - 1];
        sv::Vec3 pp;
        if (!w.root || !get_pos(w.root, &pp)) { g_solid_pending[b] = false; continue; }
        if (sv::dist_xz(rp, pp) > 1.5f) {
            send(w.phys, MSG_PHYS_COLLISION, nullptr); g_solid_pending[b] = false;
            gp_log("suave: %s %d collision back on (Ram %.1f m away)\n", b ? "member" : "partner", b, sv::dist_xz(rp, pp));
        }
    }
    // Ram never sinks more than ram_ground_snap below the floor he took off from (test 4: y -1.9 ... -41.7 in the dive)
    if (g_ram_snap_on && rok && rp.y < g_dive_floor - g_ram_snap) {
        set_pos(g_ram.root, sv::v3(rp.x, g_dive_floor, rp.z));
        send(g_ram.phys, MSG_PHYS_RESET, nullptr);
        if (g_ram_snaps++ < 12) gp_log("suave: Ram sank to y %.2f (takeoff floor %.2f): put back on the floor\n", rp.y, g_dive_floor);
    }
    for (int b = 0; b <= sv::MAX_RING; ++b) {
        if (!g_taken[b] || !g_ground_ok[b]) continue;
        const Who& w = b == 0 ? g_partner : g_mem[b - 1];
        if (!npc_alive(w)) continue;
        sv::Vec3 p; if (!get_pos(w.root, &p)) continue;
        // test 3: a pooled character that was released / re-spawned reads (0, 0, 0) or jumps away: let it go
        if (fabsf(p.x) + fabsf(p.z) < 1.0f || cls_handler(w.ai) != H_SENTRY) {
            g_taken[b] = false; g_ground_ok[b] = false;
            gp_log("suave: %s %d no longer readable (despawned / re-used): released from the move\n", b ? "member" : "partner", b);
            continue;
        }
        if (p.y < g_ground_y[b] - 0.6f) {
            set_pos(w.root, sv::v3(p.x, g_ground_y[b], p.z));
            send(w.phys, MSG_PHYS_RESET, nullptr);
            gp_log("suave: %s %d fell to y %.2f: put back on y %.2f\n", b ? "member" : "partner", b, p.y, g_ground_y[b]);
        }
    }
}
// ---- the dive (fourth test). No writes to Ram's orientation (the test-4 quaternion half turn sank him through the map):
// he turns by the game's own yaw - the mouse-turn accumulator the physics turn step consumes (mouse_turn_fix), else
// PhysicCtrl.turn_speed written right after the input procedure, as the mouse code does (0x73FFD5) - and the physics
// turns him (0x57D504: next_world_orient by turn_speed x TimeStep). The key is then chosen by his MEASURED facing.
static const char* key_label(int k) { static const char* n[] = {"W (forward)", "S (back)", "A (side)", "D (side)"}; return k >= 0 && k < 4 ? n[k] : "?"; }
static void turn_request(float deg) {
    if (deg == 0.0f) return;
    if (gp_yaw_add(deg)) { g_turn_path = 1; return; }
    g_turn_queue = deg; g_turn_path = 2;
}
static void turn_flush_in_input_tick() {   // after the input procedure wrote its own turn_speed (the direct path)
    if (g_turn_queue == 0.0f) return;
    DWORD pd = data(g_ram.phys); if (!pd) { g_turn_queue = 0.0f; return; }
    __try {
        float ts = RDF(pd + 0x84);   // TimeStep (what the turn step multiplies by)
        if (ts > 0.0f) { touch(g_ram.phys, 0); WRF(pd + 0x24, g_turn_queue / ts); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_turn_queue = 0.0f;
}
static void clear_takeoff(const sv::Vec3& p0, const sv::Vec3& d) {   // nothing may overlap Ram when he takes off
    bool crowded = false;
    for (int b = 0; b <= sv::MAX_RING; ++b) {
        if (!g_taken[b]) continue;
        const Who& w = b == 0 ? g_partner : g_mem[b - 1];
        sv::Vec3 q; if (!w.root || !get_pos(w.root, &q)) continue;
        float dd = sv::dist_xz(p0, q);
        if (dd >= g_dive_clear) continue;
        crowded = true;
        if (!g_solid_pending[b]) { send(w.phys, MSG_PHYS_NO_COLLISION, nullptr); g_solid_pending[b] = true; }
        gp_log("suave: takeoff: %s %d stands %.2f m from Ram: collision off until he is 1.5 m away\n", b ? "member" : "partner", b, dd);
    }
    if (crowded && g_dive_step > 0.0f) {   // and Ram steps off the shared spot, away from the partner
        sv::Vec3 np = sv::add(p0, sv::mul(d, g_dive_step));
        set_pos(g_ram.root, np); send(g_ram.phys, MSG_PHYS_RESET, nullptr);
        sv::Vec3 q; get_pos(g_ram.root, &q);
        gp_log("suave: takeoff: Ram stepped %.2f m off the shared spot: (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n", g_dive_step, p0.x, p0.y, p0.z, q.x, q.y, q.z);
    }
}
static void dive_go(int result) {   // the turn is over (1 done, 2 out of time): the key by his actual facing + SPACE
    gp_yaw_add(0.0f); g_turn_queue = 0.0f;   // no leftover yaw may turn him after the keys (run v12: 95 deg off at the takeoff)
    sv::Vec3 f, p; bool fok = get_fwd(g_ram.root, &f); get_pos(g_ram.root, &p);
    {   // test-copy runs v4-v11: with the partner's root on Ram's spot (the shared node) every forward / back dive sank Ram
        // into the floor even with the partner's collision off; with it 1.5 m behind him the dive is clean. The root goes
        // behind Ram on the frame the low front camera cuts in (his body keeps its clip offset from the root).
        sv::Vec3 pr;
        if (g_partner_clear > 0.0f && npc_alive(g_partner) && get_pos(g_partner.root, &pr) && sv::dist_xz(pr, p) < g_partner_clear - 0.1f) {
            sv::Vec3 np = sv::sub(p, sv::mul(g_dive_want, g_partner_clear)); np.y = pr.y;
            set_pos(g_partner.root, np); send(g_partner.phys, MSG_PHYS_RESET, nullptr);
            gp_log("suave: takeoff: the partner's root %.2f m from Ram -> %.2f m behind him (no overlap at the takeoff)\n", sv::dist_xz(pr, p), g_partner_clear);
        }
    }
    if (!fok) f = g_dive_want;
    sv::Vec3 w, cam; int k = sv::dive_key_rel(f, g_dive_want, g_side_sign, 45.0f, &w, &cam);
    const int keys[4] = {g_key_fwd, g_key_back, g_key_left, g_key_right};
    g_dive_key = keys[k];
    g_dive_world = w; g_dive_cam = cam; g_dive_known = true;
    g_dive_from = p; g_dive_floor = g_floor_known && g_floor_y > p.y ? g_floor_y : p.y; g_ram_snap_on = g_ram_snap > 0.0f; g_ram_snaps = 0;
    g_dive_check = 12; g_autodive_frames = 6;
    g_turn_sign = g_turn.sign;   // learnt (a flip means the yaw sign is the other one)
    float off = fabsf(sv::signed_yaw(f, g_dive_want)) * 57.29578f;
    gp_log("suave: dive: %s after %.2f s, %d yaw request(s) via %s%s: Ram faces (%.2f %.2f), %.0f deg from away-from-the-partner (%.2f %.2f) -> %s + %s, dives (%.2f %.2f), camera side (%.2f %.2f), floor y %.2f\n",
           result == 1 ? "TURNED" : "turn NOT complete", g_turn.t, g_turn.kicks,
           g_turn_path == 1 ? "the mouse-turn accumulator" : (g_turn_path == 2 ? "PhysicCtrl.turn_speed" : "nothing"),
           g_turn.flips ? " (yaw sign flipped)" : "", f.x, f.z, off, g_dive_want.x, g_dive_want.z, key_label(k), gp::key_name(g_key_dodge),
           w.x, w.z, cam.x, cam.z, p.y);
}
static void dive_begin(const sv::Vec3& planned) {
    sv::Vec3 p0, f0; get_pos(g_ram.root, &p0); bool fok = get_fwd(g_ram.root, &f0);
    sv::Vec3 d = sv::flat_dir(sv::v3(0, 0, 0), planned, fok ? sv::mul(f0, -1.0f) : sv::v3(0, 0, 1));
    sv::Vec3 pb;   // away from the partner's body when it is readable and apart from Ram
    if (get_pos(g_partner.pelvis ? g_partner.pelvis : g_partner.root, &pb) && sv::dist_xz(p0, pb) > 0.3f) d = sv::flat_dir(pb, p0, d);
    if (g_space_probe && g_ray_ok) {   // the most open direction within 75 deg of away-from-the-partner
        sv::Vec3 from = sv::add(p0, sv::v3(0, 1.0f, 0)); float base = sv::yaw_of(d), best = -1.0f; sv::Vec3 bd = d;
        char line[160] = ""; size_t o = 0;
        for (int k = 0; k < 7; ++k) {
            static const float off[7] = {0.0f, -0.436f, 0.436f, -0.873f, 0.873f, -1.309f, 1.309f};
            sv::Vec3 v = sv::v3(sinf(base + off[k]), 0, cosf(base + off[k]));
            float fr = free_along(from, v, 8.0f, g_ram.root);
            o += sprintf_s(line + o, sizeof line - o, " %.1f", fr);
            float score = fr - 0.4f * fabsf(off[k]);   // straight away is preferred when it is open enough
            if (k == 0 && fr >= 6.0f) { best = 99.0f; bd = v; }
            if (score > best) { best = score; bd = v; }
        }
        if (bd.x != d.x || bd.z != d.z) gp_log("suave: dive: free space away from the partner (0, -25, 25, -50, 50, -75, 75 deg):%s m -> dive (%.2f %.2f)\n", line, bd.x, bd.z);
        d = bd;
    }
    g_dive_want = d;
    clear_takeoff(p0, d);
    {   // Ram's root on the collision floor before the takeoff (runs v4-v7: 9 cm under it after the dance -> every dive sank)
        sv::Vec3 rp; get_pos(g_ram.root, &rp);
        sv::Vec3 top = sv::add(rp, sv::v3(0, 1.0f, 0));
        RayHit h = ray(top, sv::add(rp, sv::v3(0, -1.5f, 0)), true, g_ram.root);
        if (h.hit) {
            float fy = top.y - h.dist;
            g_floor_known = true; g_floor_y = fy;
            if (rp.y < fy - 0.02f) {
                set_pos(g_ram.root, sv::v3(rp.x, fy + 0.01f, rp.z)); send(g_ram.phys, MSG_PHYS_RESET, nullptr);
                gp_log("suave: dive: Ram's root %.2f m under the floor (y %.2f): lifted onto it\n", fy - rp.y, fy);
            }
        } else g_floor_known = false;
    }
    DWORD md = data(g_ram.move); DWORD lock = 0, lockpos = 0;   // an aim lock makes the physics turn to its target instead
    __try {
        if (md) { lock = RD32(md + 0xA8); lockpos = RD32(md + 0xA4); }
        if (md && (lock || lockpos)) { touch(g_ram.move, 26); WR32(md + 0xA8, 0); touch(g_ram.move, 25); WR32(md + 0xA4, 0); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("suave: dive: Ram at (%.2f %.2f %.2f) faces (%.2f %.2f); away from the partner = (%.2f %.2f) (planned %.2f %.2f)%s; turning by the game's yaw (up to %.2f s)\n",
           p0.x, p0.y, p0.z, fok ? f0.x : 0.0f, fok ? f0.z : 0.0f, d.x, d.z, planned.x, planned.z,
           (lock || lockpos) ? ", aim lock cleared" : "", g_turn_seconds);
    g_turn.sign = g_turn_sign; g_turn_path = 0; g_turn_queue = 0.0f; g_dive_known = false;
    g_turn.begin(d, g_turn_tol_deg * 3.14159265f / 180.0f, g_turn_seconds);
    if (g_turn_seconds <= 0.0f) { g_turn.on = false; dive_go(2); }
}
static void dive_turn_frame(float dt) {
    if (!g_turn.on) return;
    sv::Vec3 f;
    if (!get_fwd(g_ram.root, &f)) { g_turn.on = false; dive_go(2); return; }
    float deg = 0.0f; int r = g_turn.step(f, dt, &deg);
    if (r) { dive_go(r); return; }
    if (deg != 0.0f) {
        turn_request(deg);
        if (g_verbose) gp_log("suave: dive: yaw request %.0f deg (faces (%.2f %.2f), %.0f deg to go)\n", deg, f.x, f.z, fabsf(g_turn.err(f)) * 57.29578f);
    }
}
static const char* act_name(int k) {
    static const char* n[] = {"none", "freeze_on", "freeze_off", "slowmo", "god_on", "god_cut", "hud_start", "hud_end",
        "stop_other_music", "music_start", "music_stop", "ram_take", "ram_release", "partner_take", "partner_place",
        "partner_freeze", "play", "face", "member_take", "member_walk", "member_arrive", "dynamite_arm", "dynamite_attach",
        "boom", "member_release", "partner_release", "dynamite_drop", "input_lock", "auto_dive", "cam_take", "cam",
        "cam_release", "ram_speed", "end"};
    return (k >= 0 && k < (int)(sizeof n / sizeof n[0])) ? n[k] : "?";
}
static void exec(const sv::Acts& as) {
    if (g_verbose)
        for (int i = 0; i < as.n; ++i) {
            const sv::Act& a = as.a[i];
            if (a.kind == sv::A_CAM && a.f[3] < 0.0f) continue;   // per-frame eye moves
            if (a.kind == sv::A_PARTNER_PLACE) continue;
            gp_log("suave:   act %-16s who %2d i %2d  %.2f %.2f %.2f %.2f\n", act_name(a.kind), a.who, a.i, a.f[0], a.f[1], a.f[2], a.f[3]);
        }
    DWORD cd = gp_cache_data();
    DWORD tc = 0; __try { tc = cd ? RD32(cd + C_TIMECTRL) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (int i = 0; i < as.n; ++i) {
        const sv::Act& a = as.a[i];
        switch (a.kind) {
        case sv::A_FREEZE_ON: { gp_bt_force_off("suave"); struct { float t, s; int nb; } s = {a.f[0], a.f[1], 0}; send(tc, MSG_TIME_SLOWMO, &s); break; }
        case sv::A_FREEZE_OFF: case sv::A_SLOWMO: {
            float target = a.kind == sv::A_SLOWMO ? a.f[0] : 1.0f;
            if (target >= 1.0f) { __try { float w = RDF(cd + C_WORLDTIME); if (w > 0.0f && w <= 1.0f) target = w; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            struct { float t, s; int nb; } s = {target, a.f[1], 0}; send(tc, MSG_TIME_SLOWMO, &s); break; }
        case sv::A_GOD_ON: { float t = a.f[0]; if (send(g_goodie, MSG_GOD_INDUCE, &t)) g_flags |= TODGP_SV_ARMOR; gp_log("suave: armor (god mode) %.1f s\n", t); break; }
        case sv::A_GOD_CUT: {   // what power_up_used does for El Mariachi (0x5C0C51): CurrentGodTime -= unused
            DWORD d = data(g_goodie);
            if (d) { touch(g_goodie, 3); __try { float v = RDF(d + 0x10) - a.f[0]; WRF(d + 0x10, v > 0.01f ? v : 0.01f); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            g_flags &= ~TODGP_SV_ARMOR; gp_log("suave: armor ends (%.1f s taken back)\n", a.f[0]); break; }
        case sv::A_HUD_START: if (g_hud_effect > 0) { struct { int type; float t; } e = {g_hud_effect, 60.0f}; send(g_effects, MSG_HUD_START, &e); } break;
        case sv::A_HUD_END: if (g_hud_effect > 0) send(g_effects, MSG_HUD_END, nullptr); break;
        case sv::A_STOP_OTHER_MUSIC: break;   // handled in music_start (El Mariachi's stream: no retarget while it may play)
        case sv::A_MUSIC_START: music_start(a.i); break;
        case sv::A_MUSIC_STOP: music_stop(); break;
        case sv::A_RAM_TAKE: {
            if (g_ram_park) park(g_ram.move);
            if (g_ram_holster) send(g_ram.weapon, MSG_WEAPON_UNARMED, nullptr);   // guns away for the intro and the dance
            send(g_ram.weapon, MSG_WEAPON_SPECIAL, nullptr);
            set_speed(g_ram.move, 1.0f); set_speed(g_ram.action, 1.0f);
            set_world_yaw(g_ram.root, a.f[3]);   // toward the partner, minus the intro's start angle
            if (g_c.intro_place) set_pos(g_ram.root, sv::v3(a.f[0], a.f[1], a.f[2]));   // the shared node: Ram's intro Bip lands on his spot
            send(g_ram.phys, MSG_PHYS_RESET, nullptr);
            break; }
        case sv::A_RAM_RELEASE: {
            if (g_ram_park) unpark(g_ram.move);
            send(g_ram.weapon, MSG_WEAPON_SPECIAL_END, nullptr);
            if (g_ram_holster) send(g_ram.weapon, MSG_WEAPON_BEST, nullptr);      // guns back for the dive
            float m = 0.95f; __try { float v = RDF(cd + C_PLAYER_SPEED); if (v > 0.1f && v < 2.0f) m = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            set_speed(g_ram.move, m); set_speed(g_ram.action, m);
            break; }
        case sv::A_PARTNER_TAKE:
            ai_off(g_partner); weapon_off(g_partner, 0); g_taken[0] = true;
            { sv::Vec3 rp; g_ground_ok[0] = get_pos(g_ram.root, &rp); g_ground_y[0] = rp.y; }   // the partner stands where Ram stands
            send(g_partner.phys, MSG_PHYS_NO_COLLISION, nullptr);
            park(g_partner.move); set_speed(g_partner.move, 1.0f);
            place_partner();
            break;
        case sv::A_PARTNER_PLACE: place_partner(); break;
        case sv::A_PARTNER_FREEZE: {
            sv::Vec3 rf; g_partner_frozen = get_pos(g_ram.root, &g_release_pos) && get_fwd(g_ram.root, &rf);
            if (g_partner_frozen) g_release_yaw = sv::yaw_of(rf);
            g_solid_pending[0] = true;   // solid again once Ram has left his spot (test 3: enabling it while the two overlapped pushed Ram 2 m into the floor)
            break; }
        case sv::A_PARTNER_RELEASE:
            g_taken[0] = false;
            if (!npc_alive(g_partner)) break;
            unpark(g_partner.move); set_speed(g_partner.move, g_partner.speed0 > 0.1f ? g_partner.speed0 : 1.0f);
            send(g_partner.phys, MSG_PHYS_COLLISION, nullptr);
            ai_on(g_partner); weapon_back(g_partner, 0);
            break;
        case sv::A_PLAY: play(a.who, a.i, a.f[0]); break;
        case sv::A_FACE: face(a.who, a.i, a.f[0]); break;
        case sv::A_MEMBER_TAKE: {
            Who* w = who_of(a.who); if (!w) break; ai_off(*w); weapon_off(*w, a.who);
            if (a.who >= 1 && a.who <= sv::MAX_RING) { g_taken[a.who] = true; g_ground_ok[a.who] = false; }   // ground reference at the arrival
            break; }
        case sv::A_MEMBER_WALK: {
            Who* w = who_of(a.who); if (!w) break;
            sv::Vec3 p = sv::v3(a.f[0], a.f[1], a.f[2]);
            move_auto(w->move, true, &p); send(w->move, MSG_MOVE_WALK, nullptr);
            break; }
        case sv::A_MEMBER_ARRIVE: {
            Who* w = who_of(a.who); if (!w) break;
            move_auto(w->move, false, nullptr); send(w->move, MSG_MOVE_STOP, nullptr);
            if (a.i == 1) { float v[3] = {a.f[0], a.f[1], a.f[2]}; send(w->move, MSG_MOVE_TELEPORT, v); }
            if (a.who >= 1 && a.who <= sv::MAX_RING) { sv::Vec3 p; g_ground_ok[a.who] = get_pos(w->root, &p) && (fabsf(p.x) + fabsf(p.z) > 1.0f); g_ground_y[a.who] = a.i == 1 ? a.f[1] : p.y; }
            park(w->move);
            if (set_world_yaw(w->root, a.f[3])) send(w->phys, MSG_PHYS_RESET, nullptr);
            break; }
        case sv::A_MEMBER_RELEASE: {
            Who* w = who_of(a.who); if (a.who >= 1 && a.who <= sv::MAX_RING) g_taken[a.who] = false;
            if (!w || !npc_alive(*w)) break;
            move_auto(w->move, false, nullptr); unpark(w->move);
            ai_on(*w); weapon_back(*w, a.who);
            break; }
        case sv::A_DYNAMITE_ARM: dynamite_arm(); break;
        case sv::A_DYNAMITE_ATTACH: dynamite_attach(); break;
        case sv::A_DYNAMITE_DROP: if (g_shell) { dynamite_detach(); send(g_shell, MSG_SHELL_INACTIVE, nullptr); restore_shell(); } bundle_release(); break;
        case sv::A_BOOM: boom(a.i); break;
        case sv::A_INPUT_LOCK: g_input_lock = a.i != 0; if (g_input_lock) g_flags |= TODGP_SV_INPUT_LOCKED; else g_flags &= ~TODGP_SV_INPUT_LOCKED; break;
        case sv::A_AUTO_DIVE: dive_begin(sv::v3(a.f[0], 0, a.f[2])); break;   // the keys follow the turn (dive_turn_frame)
        case sv::A_CAM_TAKE: if (!cam_take()) g_sv.cam_taken = false; break;
        case sv::A_CAM:
            if (!g_cam_node) break;
            if (a.f[3] >= 0.0f) cam_switch(a.who, a.i, sv::v3(a.f[0], a.f[1], a.f[2]), a.f[3], a.f[4]);
            else set_pos(g_cam_node, clamp_eye(a.i, sv::v3(a.f[0], a.f[1], a.f[2])));
            break;
        case sv::A_CAM_RELEASE: cam_release(a.f[0]); break;
        case sv::A_RAM_SPEED: {   // Ram's own animation layers (the world slow motion does not slow the player)
            float to = a.f[0];
            if (to <= 0.0f) { to = 0.95f; __try { float v = RDF(gp_cache_data() + C_PLAYER_SPEED); if (v > 0.1f && v < 2.0f) to = v; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            g_rs_from = get_speed(g_ram.move); g_rs_to = to; g_rs_len = a.f[1] > 0.01f ? a.f[1] : 0.01f; g_rs_t = 0.0f; g_rs_on = true;
            gp_log("suave: Ram's animation speed %.2f -> %.2f over %.2f s (height %.2f m)\n", g_rs_from, g_rs_to, g_rs_len, g_last_height);
            break; }
        case sv::A_END:
            if (a.i) gp_log("suave: ABORTED (%s)\n", g_sv.reason); else gp_log("suave: done\n");
            if (g_kill_mask && g_kill_wait >= 0.0f) kill_pass();
            if (g_shell && !g_shell_attached) restore_shell();
            bundle_release();
            for (int b = 0; b <= sv::MAX_RING; ++b) if (g_hidden_gun[b]) { set_hidden(g_hidden_gun[b], false); g_hidden_gun[b] = 0; }
            g_input_lock = false; g_partner_frozen = false;
            for (int b = 0; b <= sv::MAX_RING; ++b) if (g_solid_pending[b]) {
                const Who& w = b == 0 ? g_partner : g_mem[b - 1];
                if (w.root) send(w.phys, MSG_PHYS_COLLISION, nullptr);
                g_solid_pending[b] = false;
            }
            g_turn.on = false; g_turn_queue = 0.0f; g_dive_known = false; g_ram_snap_on = false; g_autodive_frames = 0;
            if (g_rs_on) { set_speed(g_ram.move, g_rs_to); set_speed(g_ram.action, g_rs_to); g_rs_on = false; }
            for (int b = 0; b <= sv::MAX_RING; ++b) g_taken[b] = false;
            music_level_resume();
            g_flags &= ~(TODGP_SV_INPUT_LOCKED | TODGP_SV_DYNAMITE);
            break;
        }
    }
}

// ================================================================================================ snapshot
static bool is_vehicle() {
    DWORD d = data(g_ram.move); if (!d || !g_vehicle_ctrl) return false;
    __try { return RD32(d + 0x30) == g_vehicle_ctrl; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static sv::Snap snapshot(bool in_tick) {
    sv::Snap s;
    DWORD ram = ram_root();
    s.level_changed = ram != g_ram_root0 || mission_index() != g_mission0;
    if (s.level_changed) return s;
    int pm = gp_playmode();
    s.paused = pm == 2; s.rewind = pm == 3;
    s.cutscene = gp_cutscene();
    s.vehicle = is_vehicle();
    __try { DWORD sc = RD32(0xA3DCBC); s.time_mult = sc ? RDF(sc + 0x114) : 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { s.time_mult = 1.0f; }
    bool ok = get_pos(ram, &s.ram) && get_fwd(ram, &s.ram_fwd);
    __try {
        DWORD md = data(g_ram.move); int ms = md ? (int)RD32(md + 0x6C) : -1;   // MoveState
        DWORD active = md ? RD32(md + 0x30) : 0;
        if (!g_combat_ctrl) g_combat_ctrl = find_child(g_ram.move, "CombatMoveCtrl");
        s.ram_diving = (g_combat_ctrl && active == g_combat_ctrl && ms != 0) || ms == g_dive_state || (g_dive_state2 >= 0 && ms == g_dive_state2);
        DWORD a = data(g_ram.ai);
        if (a && cls_handler(g_ram.ai) == H_PLAYER_AI) s.player_dead = RDF(a + 0x1C) > 0.0f && RDF(a + 0x20) >= RDF(a + 0x1C);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    s.partner_alive = npc_alive(g_partner);
    if (s.partner_alive) {
        get_pos(g_partner.root, &s.partner);
        sv::Vec3 pf; if (get_fwd(g_partner.root, &pf)) { float d = sv::yaw_of(pf) - (g_partner_frozen ? g_release_yaw : sv::yaw_of(s.ram_fwd)); while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f; s.partner_yaw_err = fabsf(d); }
    }
    if (!get_pos(g_partner.pelvis, &s.partner_body)) s.partner_body = s.partner;
    if (!get_pos(g_partner.chest, &s.partner_chest)) s.partner_chest = sv::add(s.partner_body, sv::v3(0, 0.35f, 0));
    for (int j = 0; j < g_sv.n; ++j) { s.member_alive[j] = npc_alive(g_mem[j]); if (s.member_alive[j]) get_pos(g_mem[j].root, &s.member[j]); }
    s.camera_lost = cam_lost();
    s.dive_turning = g_turn.on; s.dive_known = g_dive_known; s.dive_world = g_dive_world; s.dive_cam = g_dive_cam;
    s.ram_height = g_dive_known ? s.ram.y - g_dive_floor : 0.0f; g_last_height = s.ram_height;
    s.ok = ok && in_tick;
    return s;
}

// ================================================================================================ start / tick
static void start_now() {
    g_pending_start = false;
    DWORD ram = ram_root();
    memset(g_clip, 0, sizeof g_clip); memset(g_mem, 0, sizeof g_mem);
    if (!fill_who(ram, &g_ram) || !fill_who(g_pending_root[0], &g_partner) || !npc_alive(g_partner)) {
        gp_log("suave: partner gone before the start: charge given back\n");
        struct { int id; float n; } gv = {g_slot + 6, 1.0f}; send(sub(ram, R_GOODIE), MSG_GOODIE_ACTIVATE, &gv);
        return;
    }
    g_goodie = sub(ram, R_GOODIE);
    DWORD pu = g_powerup ? g_powerup : sub(ram, R_POWERUP);
    g_effects = 0; __try { DWORD pd = data(pu); g_effects = pd ? RD32(pd + 0x10) : 0; if (pd && RD32(pd + 0x08) != g_goodie) gp_log("suave: note: powerup.pGoodieCtrl != root.GoodieCtrl\n"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ram_root0 = ram; g_mission0 = mission_index();
    g_vehicle_ctrl = find_child(g_ram.move, "InsideVehicleMoveCtrl");
    g_combat_ctrl = find_child(g_ram.move, "CombatMoveCtrl");
    const char* model = ""; __try { model = ((Model_t)(uintptr_t)P_MODELRES)(g_partner.anim, nullptr); if (!model) model = ""; } __except (EXCEPTION_EXECUTE_HANDLER) { model = ""; }
    bool matched = false; g_pair = sv::pair_from_model(model, &matched);
    if (!matched) {   // another model: the closest pairing by limb lengths (pose-independent)
        DWORD sp3 = find_path(g_partner.chest, "Spine3");
        DWORD ua = find_path(sp3, "L Clavicle/L UpperArm"), fa = find_path(ua, "L Forearm"), hd = find_path(fa, "L Hand");
        DWORD th = find_path(g_partner.pelvis, "L Thigh"), ca = find_path(th, "L Calf"), ft = find_path(ca, "L Foot");
        sv::Vec3 p1, p2, p3, q1, q2, q3;
        if (get_pos(ua, &p1) && get_pos(fa, &p2) && get_pos(hd, &p3) && get_pos(th, &q1) && get_pos(ca, &q2) && get_pos(ft, &q3)) {
            float arm = sv::dist(p1, p2) + sv::dist(p2, p3), leg = sv::dist(q1, q2) + sv::dist(q2, q3);
            g_pair = sv::pair_by_limbs(arm, leg);
            gp_log("suave: partner limbs arm %.3f m leg %.3f m -> closest pairing %s\n", arm, leg, sv::pair_name(g_pair));
        } else gp_log("suave: partner bones not found: soldier pairing\n");
    }
    sv::Vec3 mp[sv::MAX_RING]; int n = 0;
    for (int j = 0; j < g_pending_n; ++j) {
        Who w; if (!fill_who(g_pending_root[1 + j], &w) || !npc_alive(w) || !get_pos(w.root, &mp[n])) continue;
        g_mem[n++] = w;
    }
    g_flags = 0; g_shell = 0; g_shell_attached = false; g_shell_holder = 0; g_kill_mask = 0; g_kill_wait = -1; g_partner_frozen = false;
    memset(g_solid_pending, 0, sizeof g_solid_pending); g_turn.on = false; g_turn_queue = 0.0f; g_dive_known = false; g_ram_snap_on = false;
    g_sv.n = n;
    sv::Snap s = snapshot(true); s.ok = true;
    if (gp_ini().geti("suave", "debug_rays", 0)) {   // test copy: the line test straight down / up / out
        sv::Vec3 c0 = sv::add(s.ram, sv::v3(0, 1.0f, 0));
        ray(c0, sv::add(c0, sv::v3(0, -3.0f, 0)), true, g_ram.root);
        ray(c0, sv::add(c0, sv::v3(0, 30.0f, 0)), true, g_ram.root);
        for (int i = 0; i < 4; ++i) { float an = i * 1.5707963f; ray(c0, sv::add(c0, sv::v3(8.0f * sinf(an), 0, 8.0f * cosf(an))), true, g_ram.root); }
        ray(c0, s.partner_chest, true, g_ram.root);
        ray(c0, s.partner_chest, true, 0);
    }
    if (g_space_probe) {   // fifth test, rooms: the nearest wall around Ram (12 rays at chest height, 8 m)
        float fr[12]; float mn = 99.0f; char line[200] = ""; size_t o = 0;
        sv::Vec3 from = sv::add(s.ram, sv::v3(0, 1.0f, 0));
        for (int i = 0; i < 12; ++i) {
            float a = i * 0.5235988f; fr[i] = free_along(from, sv::v3(sinf(a), 0, cosf(a)), 8.0f, g_ram.root);
            if (fr[i] < mn) mn = fr[i];
            o += sprintf_s(line + o, sizeof line - o, " %.1f", fr[i]);
        }
        s.free_min = mn;
        gp_log("suave: space around Ram (m, every 30 deg from +z):%s; nearest %.1f m%s\n", line, mn, mn < g_c.ring_radius + 0.7f ? " -> TIGHT: smaller ring, cameras kept inside" : "");
    }
    sv::Acts a = g_sv.start(g_c, s, g_pair, matched, mp, n, GetTickCount());
    gp_log("suave: START partner model '%s' -> pair %s%s, ring %d (radius %.1f m), music segment %d, phases: intro %.2f s, release %.2f s, dive %.2f s\n",
           model, sv::pair_name(g_pair), matched ? "" : " (fallback)", g_sv.n, g_sv.ring_r, g_sv.segment, g_c.intro_end(), g_c.release_t(), g_c.dive_t());
    exec(a);
}
static int g_last_phase = 0;
static double g_last_input_tick = 0.0;
// The input procedures (KbProcWrap / PadProcWrap) do not run every frame while the world is slowed or the game holds
// the player (first in-game test, 2026-10-23: two moves aborted as "player not in control" after 1.4 / 3.4 s). The move
// is therefore driven from the main loop (suave_post_input, every frame, real time); the input tick is only noted.
void suave_input_tick(void* thread, double now) { (void)thread; g_last_input_tick = now; turn_flush_in_input_tick(); }

static const char* ctrl_name(DWORD move) {   // which MoveCtrl sub-controller is active (for the state lines)
    static const char* kids[] = {"NormalMoveCtrl", "CombatMoveCtrl", "InsideVehicleMoveCtrl", "SpecialMoveCtrl",
                                 "MiscMoveCtrl", "VictimMoveCtrl", "TakeAHitMoveCtrl"};
    static const char* shortn[] = {"normal", "combat", "vehicle", "special", "misc", "victim", "takeahit"};
    DWORD d = data(move); DWORD a = 0;
    __try { a = d ? RD32(d + 0x30) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { a = 0; }
    if (!a) return "-";
    for (int i = 0; i < 7; ++i) if (find_child(move, kids[i]) == a) return shortn[i];
    return "?";
}
static void who_line(const char* tag, const Who& w) {
    if (!w.root) return;
    int st = -1, ms = -1; float hp = 0, dmg = 0; DWORD prim = 0; sv::Vec3 p;
    __try {
        DWORD a = data(w.ai); if (a && cls_handler(w.ai) == H_SENTRY) { st = (int)RD32(a + 0x1D4); hp = RDF(a + 0x2C); dmg = RDF(a + 0x30); }
        DWORD md = data(w.move); ms = md ? (int)RD32(md + 0x6C) : -1;
        DWORD wd = data(w.weapon); prim = wd ? RD32(wd + 0x20) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    get_pos(w.root, &p);
    gp_log("suave:   %-8s ai %3d hp %.0f/%.0f move %-8s ms %3d weapon %s pos (%.1f %.1f %.1f)\n", tag, st, dmg, hp,
           ctrl_name(w.move), ms, prim ? "in hand" : "none", p.x, p.y, p.z);
}
static void keep_ai_off(double now) {   // the AI must stay in state 2 (no AI control) while taken
    for (int b = 0; b <= sv::MAX_RING; ++b) {
        if (!g_taken[b]) continue;
        const Who& w = b == 0 ? g_partner : g_mem[b - 1];
        if (!npc_alive(w) || now - g_reassert_t[b] < 0.3) continue;
        int st = -1; __try { DWORD a = data(w.ai); st = a ? (int)RD32(a + 0x1D4) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (st != 2 && st != 28 && st >= 0) {
            g_reassert_t[b] = now;
            send(w.ai, MSG_AI_NO_CONTROL, nullptr);
            if (g_weapon_mode >= 1) send(w.weapon, MSG_WEAPON_UNARMED, nullptr);
            gp_log("suave: %s %d: AI state %d -> no_ai_control again\n", b ? "member" : "partner", b, st);
        }
    }
}
static void suave_frame(double now) {
    static double last = 0.0;
    float dt = last > 0.0 ? (float)(now - last) : 0.0f;
    if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    last = now; g_last_tick = now;
    if (InterlockedExchange(&g_cheat_press, 0)) {
        DWORD g = sub(ram_root(), R_GOODIE);
        struct { int id; float n; } gv = {g_slot + 6, 1.0f};   // goodie 16 Pinata / 17 Tequila (slot = goodie - 6)
        bool ok = send(g, MSG_GOODIE_ACTIVATE, &gv);
        gp_log("suave: cheat key: +1 slot-%d charge through activate_goodie(%d)%s (refused by the game when full)\n", g_slot, gv.id, ok ? "" : " (FAILED)");
    }
    static bool debug_started = false;   // once per game run
    if (g_debug_start_key >= 0 && !debug_started && !g_sv.running() && !g_pending_start && InterlockedExchange(&g_debug_start, 0)) {
        DWORD ram = ram_root(); sv::Vec3 rp; DWORD partner = 0, mem[sv::MAX_RING]; int nm = 0; sv::Vec3 mp[sv::MAX_RING];
        if (gp_ini().geti("suave", "debug_near_wall", 0)) {   // test copy: Ram 2 m from the nearest wall (a tight spot)
            DWORD r0 = ram_root(); sv::Vec3 rp0;
            if (r0 && get_pos(r0, &rp0)) {
                sv::Vec3 from = sv::add(rp0, sv::v3(0, 1.0f, 0)); float best = 99.0f; sv::Vec3 bd = sv::v3(0, 0, 1);
                for (int i = 0; i < 24; ++i) { float an = i * 0.2617994f; sv::Vec3 dv = sv::v3(sinf(an), 0, cosf(an)); float fr = free_along(from, dv, 40.0f, r0); if (fr < best) { best = fr; bd = dv; } }
                if (best < 39.0f && best > 2.2f) {
                    sv::Vec3 np = sv::add(rp0, sv::mul(bd, best - 2.0f));
                    set_pos(r0, np); send(sub(r0, R_PHYS), MSG_PHYS_RESET, nullptr);
                    gp_log("suave: debug: Ram moved %.1f m toward the nearest wall (%.1f m away) -> 2.0 m from it\n", best - 2.0f, best);
                }
            }
        }
        float g0 = g_c.grab_radius, p0 = g_c.pull_radius;   // the test copy's fights may be far: a wider search
        g_c.grab_radius = g_c.pull_radius = gp_ini().getf("suave", "debug_radius", 60.0f);
        bool found = ram && get_pos(ram, &rp) && choose_now(ram, rp, &partner, mem, &nm, mp);
        g_c.grab_radius = g0; g_c.pull_radius = p0;
        if (found) {
            g_pending_start = true; g_pending_root[0] = partner; for (int j = 0; j < nm; ++j) g_pending_root[1 + j] = mem[j]; g_pending_n = nm;
            debug_started = true;
            gp_log("suave: debug start key: started without a charge (test copy)\n");
        } else gp_log("suave: debug start key: no enemy within %.1f m\n", g_c.grab_radius);
    }
    if (g_pending_start) start_now();
    if (!g_sv.running()) return;
    if (g_kill_mask && g_kill_wait >= 0.0f) { g_kill_wait -= dt; if (g_kill_wait < 0.0f) { g_kill_wait = 0.0f; kill_pass(); } }
    sv::Snap s = snapshot(true);
    static double last_dump = 0.0;
    if (g_verbose && now - last_dump > 0.5) {
        last_dump = now;
        int alive = 0; for (int j = 0; j < g_sv.n; ++j) alive += s.member_alive[j];
        int pm = gp_playmode(); int combo = -1, stereo = -1;
        __try { DWORD cd = gp_cache_data(); combo = cd ? (int)RD32(cd + C_COMBOMUSIC) : -1; DWORD sd = data(ambient_sound()); stereo = sd ? (int)RD32(sd + 0xC4) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        gp_log("suave: [%s c %.2f t %.2f] tm %.3f playmode %d input-tick %.2f s ago combo-music %d stereo-track %d diving %d partner %s d %.2f yawerr %.2f ring %d/%d shot %d%s%s%s%s\n",
               sv::phase_name(g_sv.phase), g_sv.c, g_sv.real_t, s.time_mult, pm, g_last_input_tick > 0 ? now - g_last_input_tick : -1.0,
               combo, stereo, s.ram_diving, s.partner_alive ? "alive" : "dead", sv::dist(s.partner, s.ram), s.partner_yaw_err, alive,
               g_sv.n, g_sv.shot, s.cutscene ? " CUTSCENE" : "", s.vehicle ? " VEHICLE" : "", s.paused ? " PAUSED" : "", s.camera_lost ? " CAMERA-LOST" : "");
        who_line("ram", g_ram);
        who_line("partner", g_partner);
        for (int j = 0; j < g_sv.n; ++j) { char tag[16]; sprintf_s(tag, "member%d", j + 1); who_line(tag, g_mem[j]); }
        if (g_shell) shell_line("state");
    }
    sv::Acts a = g_sv.tick(g_c, s, dt);
    if (g_sv.phase != g_last_phase) { gp_log("suave: phase %s (clock %.2f s)\n", sv::phase_name(g_sv.phase), g_sv.c); g_last_phase = g_sv.phase; }
    exec(a);
    if (g_sv.running()) { dive_turn_frame(dt); keep_ai_off(now); ground_snap(); if (g_shell_holder == 3 || g_nextra) strap_frame(); }
    if (g_rs_on) {   // the ramp of Ram's animation speed (real time)
        g_rs_t += dt; float k = g_rs_t / g_rs_len; if (k >= 1.0f) { k = 1.0f; g_rs_on = false; }
        float v = g_rs_from + (g_rs_to - g_rs_from) * k;
        set_speed(g_ram.move, v); set_speed(g_ram.action, v);
    }
    if (g_dive_check > 0 && --g_dive_check == 0) {
        sv::Vec3 p, f; get_pos(g_ram.root, &p); get_fwd(g_ram.root, &f);
        sv::Vec3 m = sv::sub(p, g_dive_from);
        float away = m.x * g_dive_want.x + m.z * g_dive_want.z;
        if (g_debug_shots) gp_log("debug keys: mark burst sv_dive 2 120\n");
        gp_log("suave: dive check: Ram facing (%.2f %.2f), moved (%.2f %.2f %.2f) m, %.2f m away from the partner, MoveState %d\n", f.x, f.z, m.x, m.y, m.z,
               away, (int)RD32(data(g_ram.move) + 0x6C));
    }
}

// ================================================================================================ main loop
void suave_post_input(const BYTE* phys, BYTE* edges, BYTE* state, DWORD* mouse, double now, bool fresh) {
    (void)fresh;
    if (!g_installed) return;
    static bool cheat_held = false;
    if (g_cheat_key >= 0 && g_cheat_key < 256 && phys) {
        bool h = (phys[g_cheat_key] & 0x80) != 0;
        if (h && !cheat_held) InterlockedExchange(&g_cheat_press, 1);
        cheat_held = h;
    }
    static bool dbg_held = false;   // the debug start key reads the game's own state array (a key script taps it there)
    if (g_debug_start_key >= 0 && g_debug_start_key < 256 && state) {
        bool h = (state[g_debug_start_key] & 0x80) != 0 || (phys && (phys[g_debug_start_key] & 0x80) != 0);
        if (h && !dbg_held) InterlockedExchange(&g_debug_start, 1);
        dbg_held = h;
    }
    music_restore_if_due(now);
    suave_frame(now);   // aborts only for real reasons (cut-scene, vehicle, rewind, level change, death; pause holds)
    if (edges && state) {
        __try {
            if (g_input_lock && g_sv.running()) {
                for (int k = 0; k < 256; ++k) {
                    if (k == 0x01 || (k >= 0x3B && k <= 0x44) || k == 0x57 || k == 0x58) continue;   // Escape, F1..F12
                    edges[k] = 0; state[k] = 0;
                }
                if (mouse) for (int b = 0; b < 8; ++b) mouse[b] = 0;
            }
            if (g_autodive_frames > 0) {   // frames 6..1: forward held from the start, the dodge pressed on frame 4
                state[g_dive_key] = 0x80;
                if (g_autodive_frames == 6) edges[g_dive_key] |= 1;
                if (g_autodive_frames <= 4 && g_autodive_frames >= 2) state[g_key_dodge] = 0x80;
                if (g_autodive_frames == 4) edges[g_key_dodge] |= 1;
                if (g_autodive_frames == 1) { edges[g_key_dodge] |= 2; edges[g_dive_key] |= 2; state[g_dive_key] = 0; }
                --g_autodive_frames;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
void suave_publish(TodgpShared* sh) {
    if (!sh) return;
    if (g_installed) sh->flags |= TODGP_F_SUAVE_ENABLED;
    if (g_sv.running()) sh->flags |= TODGP_F_SUAVE_RUNNING;
    sh->suave_phase = (uint32_t)g_sv.phase;
    sh->suave_clock = g_sv.phase == sv::P_FREEZE ? -1.0f : g_sv.c;
    sh->suave_phase_t = g_sv.phase_t;
    sh->suave_phase_len = g_sv.running() ? g_sv.phase_len(g_c) : 0.0f;
    sh->suave_dive_at = g_c.dive_t();
    sh->suave_beat = g_sv.c / g_c.beat();
    sh->suave_centre[0] = g_sv.centre.x; sh->suave_centre[1] = g_sv.centre.y; sh->suave_centre[2] = g_sv.centre.z;
    sh->suave_ring_radius = g_c.ring_radius;
    sh->suave_members = (uint32_t)g_sv.n;
    sh->suave_flags = g_sv.running() ? g_flags : 0;
    sh->suave_uses = g_sv.uses; sh->suave_aborts = g_sv.aborts;
    sh->suave_shot = (uint32_t)(g_sv.running() ? g_sv.shot : 0);
}

// ================================================================================================ install
static const char* SV_REFS[] = {
    "SV_USE_GENERIC", "SV_USE_GENERIC2", "SV_SHELL_SHOW", "SV_SHOW_FN", "SV_TURN_LOCK", "F_OCCLUDED", "SV_OCCL_IGNORE", "SV_OCCL_MASK", "SV_CLS_WEAPON", "SV_GUN_DROP", "F_SETFLAGS", "SV_USE_EXIT", "SV_USE_SLOT", "SV_ELIG_CALL", "F_ELIGIBLE", "SV_HUD_START", "SV_GOD_SEND", "SV_GOD_CUT", "SV_MUSIC_A", "SV_MUSIC_B", "SV_MUSIC_C", "H_STOP_SPEAK", "SV_SETSTREAM_REG", "SV_QUERY_A", "SV_QUERY_FREE", "SV_FACTION_A", "SV_FACTION_B", "SV_FACTION_C", "SV_SHAKE", "SV_EXPLOSION", "SV_CLASSOBJ", "SV_CLS_SENTRY", "SV_CLS_ROOT", "SV_CLS_SHELL", "SV_CLS_FACE", "SV_CLS_PLAYERAI", "SV_ATTACH", "SV_CAMNODE", "SV_CAM_RELEASE", "SV_MODELRES", "SV_ANIMDICT", "SV_MOTION", "SV_BEATING", "H_NO_AI", "H_ROOT_HIT", "H_EXPLODE", "H_FACE", "H_CUT_TO", "H_TRANSITION", "F_CMD", "F_NATIVE", "F_TRIGGER", "F_GETPROP", "F_GETMEMBER", "F_QUERY", "F_LISTFREE", "F_GETPOS", "F_L2W", "F_SETPOS", "F_SETROTY", "F_FINDCHILD", "F_SETSTREAM", "F_LAYERSPEED"
};
bool suave_install(bool tick_ok) {
    g_installed = false;
    if (!g_c.enabled) return false;
    if (!tick_ok) { gp_log("suave: the input tick is not installed: OFF\n"); return false; }
    for (const char* r : SV_REFS) if (!gp_ref_ok(r)) { gp_log("suave: reference %s differs: OFF\n", r); return false; }
    if (!(gp_site_ok("USE_GATE_A") && gp_site_ok("USE_GATE_B") && gp_site_ok("USE_GATE_G"))) { gp_log("suave: use-power-up sites differ: OFF\n"); return false; }
    bool ok = gp_patch_call("USE_GATE_A", (void*)&UseGateStub) && gp_patch_call("USE_GATE_B", (void*)&UseGateStub) &&
              gp_patch_call("USE_GATE_G", (void*)&UseGateStub);
    if (!ok) { gp_log("suave: patching the use gates failed\n"); return false; }
    g_installed = true;
    gp_log("suave: installed (slot %d = Suave; grab %.1f m, pull %.1f m, ring %d x %.1f m; camera %d, faces %d, auto dive %d, music %d)\n",
           g_slot, g_c.grab_radius, g_c.pull_radius, g_c.ring_max, g_c.ring_radius, g_c.camera, g_c.faces, g_c.auto_dive, g_c.music);
    return true;
}

// ---- test host only (tests/dll/host.cpp): redirect the gate's two exits to host functions, force "running"
static void __cdecl TestRedirect(DWORD touch_to, DWORD exit_to, DWORD generic_to) { R_TOUCH = touch_to; R_USE_EXIT = exit_to; R_USE_GENERIC = generic_to; }
static void __cdecl TestForceGate(int v) { InterlockedExchange(&g_test_force_gate, v); }
static int  __cdecl TestSlot() { return g_slot; }
static void __cdecl TestForceRunning(int on) { InterlockedExchange(&g_test_force_running, on); }
static int  __cdecl TestInstalled() { return g_installed ? 1 : 0; }
void* suave_test_stub(const char* name) {
    if (!strcmp(name, "use_gate")) return (void*)&UseGateStub;
    if (!strcmp(name, "use_gate_redirect")) return (void*)&TestRedirect;
    if (!strcmp(name, "use_gate_force")) return (void*)&TestForceGate;
    if (!strcmp(name, "suave_slot")) return (void*)&TestSlot;
    if (!strcmp(name, "suave_force_running")) return (void*)&TestForceRunning;
    if (!strcmp(name, "suave_installed")) return (void*)&TestInstalled;
    return nullptr;
}
