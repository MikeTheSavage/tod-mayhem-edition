// drops_game.cpp - game glue of [drops] (TODGameplay.dll): enemies leave more than vanilla, on every difficulty.
//   * ammo: the BulletDrop bands (cache +2C0h) x ammo_scale. With [difficulty] tuning on, gp_difficulty.cpp writes them
//     with loot_ammo_factor() in place of its own factor; without it this file writes them (same capture / base rules).
//   * health and power-ups: a dead hostile enemy may leave one of the game's own goodies (medium / large health, a power-up
//     whose slot is not full), as a real pooled goodie prop of DefaultGoodieRessources: model, glow, rotation, trigger,
//     activate_goodie, HUD effect and sound are the game's. The same take-without-placeholder path as PSYCHO's elite drops
//     (psycho/code/psycho_drops.inl), with ResourceStatus 3 (elite drops use 2, the game 0 / 1: nobody frees another's).
// The pure decisions (roll, footprint height, book) are drops_logic.cpp, unit tested by test_drops.cpp.
// Rules as in dll/src/gp_game.cpp: every address byte-checked (sites.h REF rows), every game read SEH-guarded, no code site
// of its own (the input tick and the post-input hook other features already use).
// Evidence: docs/mods/drops.md and dll/HOOKS.md section "[drops]".
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../dll/src/gp_glue.h"
#include "drops_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v)  (*(volatile float*)(uintptr_t)(a) = (float)(v))

// ================================================================================================ game addresses
static const DWORD A_PLAYER_HOLDER = 0xA3B590, A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_LIST_INT = 0xA3BB04;
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_SCENE = 0xA3DCBC, A_GAME_MS = 0xA3DCD4;
static const DWORD A_LIST_LISTS = 0xA3BDF0, A_LIST_RES = 0xA3BCE4;        // the list types of ResourceList (0x75FA60)
static const DWORD P_SENDCMD = 0x86A340, P_GETPROP = 0x86A6A0, P_GETMEMBER = 0x869E80, P_QUERY = 0x486950, P_LISTFREE = 0x8718D0,
                   P_GETPOS = 0x88CA80, P_L2W = 0x88C9C0, P_SETPOS = 0x88E600, P_FINDCHILD = 0x88EB60, P_OCCLUDED = 0x91D020,
                   P_TOUCH = 0x86B4B0;
// script cache data
static const DWORD C_MASTER = 0x254, C_MISSION = 0x25C, C_FACTIONS = 0x278, C_BULLETDROP = 0x2C0;
// character_root data
static const DWORD R_SUBTYPE = 0x14, R_PLACEHOLDER = 0x24, R_ANIM = 0x50, R_AI = 0x58, R_WEAPON = 0x6C, R_POWERUP = 0x8C;
static const DWORD H_SENTRY = 0x5C90F0, H_ROOT = 0x5C3060, H_PLAYER_AI = 0x5ACE10, H_WEAPON = 0x665630;
static const int   PROP_FACTION = 0x2A8;
static const DWORD S_SUBTYPE = 0x28;           // sentry data: 4 escort, 7 player, 8 never aggressive, 9 civilian
static const int   CMD_TAKE_A_HIT = 215;       // character_root take_a_hit (global command id, as psycho's debug_ram_dies)
// resource pool / goodie props (psycho_drops.inl, the same REF rows)
static const DWORD M_RESOURCE = 0x40, RC_SUCCESS = 0x0C, RC_AVAILABLE = 0x10, RC_LIST = 0x14;
static const int   ST_MINE = 3;                // ResourceStatus of a [drops] pickup
static const int   RES_GOODIE = 5;
static const int   MSG_RES_FREE = -10, MSG_RES_REQUEST = -13, MSG_PROP_RESET = -4;
static const int   CMD_GIVEUP_RESOURCE = 0x2AB;
static const DWORD P_LOCKED = 0x18, P_PLACEHOLDER = 0x1C, P_CTRL = 0x24, P_AI = 0x2C, P_STATUS = 0x38, P_SUBTYPE = 0x5C;
static const int   SLOT_P_STATUS = 12, SLOT_P_SUBTYPE = 0x15, SLOT_AI_TIMER = 0;
static const DWORD PA_TIMER = 0x04;            // prop_ai_ctrl data: AutoRemoveTimer
static const DWORD G_ORGPOS = 0x38;            // Goodie_Default data: OrgPos (vec, touch slot 0Ch), set by command_reset (REF DR_GOODIE_ORGPOS)
static const int   SLOT_G_ORGPOS = 0x0C;
// weapons / missions
static const DWORD W_MAXDROP = 0x7C, W_TODROP = 0x80;   // default_weapon data: MaxBulletsToDrop / BulletsToDrop (REF DR_WPN_BULLETS)
static const DWORD MD_NO_COMBO = 0x78;                  // mission data: != 0 -> no combo kill count / awards (REF DR_COMBO_BLOCK)

typedef char(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);
typedef void(__fastcall* Get_t)(DWORD node, void* edx, int id, void* out);
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);
typedef void(__fastcall* Find_t)(DWORD node, void* edx, void* q);
typedef BYTE(__fastcall* Occl_t)(void* self, void* edx, const float* from, const float* to);
typedef void(__fastcall* Touch_t)(DWORD node, void* edx, int slot);
typedef void(__fastcall* Flags_t)(DWORD node, void* edx, DWORD flags);

// ================================================================================================ config
static dr::Cfg g_c;
static bool  g_installed = false, g_verbose = false, g_marks = false, g_bd_write = true;
static bool  g_fix_orgpos = false;             // test: OrgPos = the drop's spot (the level's goodies keep the reset's 0,0,0)
static int   g_excluded_slot = -1;             // Suave on the Pinata's slot 10
// PSYCHO elite drops ([psycho] elite_drops): an elite that leaves its loco move gets no power-up of ours
static bool  g_ps_drops = false, g_ps_bosses = false;
static float g_ps_elite = 200.0f, g_ps_boss = 1000.0f;
// test copy only
static int   g_probe_key = gp::KEY_NONE, g_test_key = gp::KEY_NONE;
static float g_probe_every = 0.0f, g_kill_every = 0.0f, g_kill_range = 40.0f, g_kill_stop = 0.0f;
static int   g_test_goodie = dr::G_MEDIUM_HEALTH;
static bool  g_walk_goodie = false;            // Ram turns to the nearest level goodie and walks up to it
static float g_walk_stop = 3.0f;
static float g_perf_log = 30.0f;

static bool  b_cfg(const char* k, bool d) { return gp_ini().geti("drops", k, d ? 1 : 0) != 0; }
static float f_cfg(const char* k, float d) { return gp_ini().getf("drops", k, d); }
void loot_load_config() {
    dr::Cfg c;
    c.enabled = b_cfg("enabled", false);
    c.ammo_scale = f_cfg("ammo_scale", c.ammo_scale);
    c.keep_difficulty_ammo = b_cfg("keep_difficulty_ammo", c.keep_difficulty_ammo);
    c.health_chance = f_cfg("health_chance", c.health_chance);
    c.health_hurt_bonus = f_cfg("health_hurt_bonus", c.health_hurt_bonus);
    c.large_health_share = f_cfg("large_health_share", c.large_health_share);
    c.powerup_chance = f_cfg("powerup_chance", c.powerup_chance);
    for (int i = 0; i < dr::N_POWERUPS; ++i) { char k[40]; _snprintf(k, sizeof k, "weight_%s", dr::POWERUP_NAME[i]); k[sizeof k - 1] = 0; c.weight[i] = gp_ini().geti("drops", k, c.weight[i]); }
    c.elite_skip_powerup = b_cfg("elite_skip_powerup", c.elite_skip_powerup);
    c.drop_seconds = f_cfg("drop_seconds", c.drop_seconds);
    c.max_on_ground = gp_ini().geti("drops", "max_on_ground", c.max_on_ground);
    c.track_radius = f_cfg("track_radius", c.track_radius);
    c.settle = f_cfg("settle_seconds", c.settle);
    c.body_reach = f_cfg("body_reach", c.body_reach); c.body_drop = f_cfg("body_drop", c.body_drop);
    c.give_up = f_cfg("give_up_seconds", c.give_up);
    c.steal_distance = f_cfg("steal_distance", c.steal_distance);
    c.gun_gap = f_cfg("gun_gap", c.gun_gap);
    c.lift = f_cfg("lift", c.lift); c.footprint = f_cfg("footprint", c.footprint); c.max_step = f_cfg("max_step", c.max_step);
    c.calibrate_max = f_cfg("calibrate_max", c.calibrate_max);
    c.clamp();
    g_c = c;
    g_verbose = b_cfg("log_verbose", false);
    g_bd_write = b_cfg("write_bulletdrop", true);
    g_fix_orgpos = b_cfg("debug_fix_orgpos", false);
    g_excluded_slot = (suave_enabled() && gp_ini().geti("suave", "slot", 11) == 10) ? 10 : -1;
    g_ps_drops = gp_ini().geti("psycho", "enemy_loco_moves", 0) != 0 && gp_ini().geti("psycho", "elite_drops", 1) != 0;
    g_ps_elite = gp_ini().getf("psycho", "elite_health", 200.0f); g_ps_boss = gp_ini().getf("psycho", "boss_health", 1000.0f);
    g_ps_bosses = gp_ini().geti("psycho", "bosses", 0) != 0;
    g_probe_key = gp_key_cfg("drops", "debug_probe_key", "NONE");
    g_test_key = gp_key_cfg("drops", "debug_drop_key", "NONE");
    g_probe_every = f_cfg("debug_probe_seconds", 0.0f);
    g_kill_every = f_cfg("debug_kill_seconds", 0.0f); g_kill_range = f_cfg("debug_kill_range", 40.0f); g_kill_stop = f_cfg("debug_kill_stop", 0.0f);
    g_test_goodie = gp_ini().geti("drops", "debug_drop_goodie", dr::G_MEDIUM_HEALTH);
    g_walk_goodie = b_cfg("debug_walk_to_goodie", false); g_walk_stop = f_cfg("debug_walk_stop", 3.0f);
    g_marks = b_cfg("debug_marks", false);
    g_perf_log = f_cfg("perf_log_seconds", 30.0f);
}
bool loot_enabled() { return g_c.enabled; }
float loot_ammo_factor(float difficulty_factor) { return g_installed ? dr::ammo_factor(g_c, difficulty_factor) : difficulty_factor; }

// ================================================================================================ engine calls
struct Cur { DWORD a, b, c; };
static Cur cur_save() { Cur k; k.a = RD32(A_CUR_NODE); k.b = RD32(A_CUR_2); k.c = RD32(A_CUR_3); return k; }
static void cur_restore(const Cur& k) { WR32(A_CUR_NODE, k.a); WR32(A_CUR_2, k.b); WR32(A_CUR_3, k.c); }
static DWORD data(DWORD node) { __try { return node ? RD32(node + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_handler(DWORD node) {
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool send(DWORD node, int id, void* args) { return node && gp_send(node, id, args); }
static bool command(DWORD node, int id, void* args) {
    if (!node) return false;
    Cur k = cur_save(); bool ok = true;
    __try { ((Cmd_t)(uintptr_t)P_SENDCMD)(node, nullptr, id, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
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
static bool get_pos(DWORD node, dr::Vec3* p) {
    if (!node) return false;
    float v[3];
    __try { ((Vec_t)(uintptr_t)P_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (int i = 0; i < 3; ++i) if (!(v[i] == v[i]) || fabsf(v[i]) > 1.0e5f) return false;   // NaN / inf / garbage
    p->x = v[0]; p->y = v[1]; p->z = v[2]; return true;
}
static bool get_fwd(DWORD node, dr::Vec3* f) {
    if (!node) return false;
    float b[6] = {0, 0, 0, 0, 0, 1};
    __try { ((Vec_t)(uintptr_t)P_L2W)(node, nullptr, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float l = sqrtf(b[0] * b[0] + b[2] * b[2]);
    if (!(l > 1e-4f)) return false;
    f->x = b[0] / l; f->y = 0; f->z = b[2] / l; return true;
}
static bool set_pos(DWORD node, const dr::Vec3& p) {
    if (!node) return false;
    float v[3] = {p.x, p.y, p.z};
    __try { ((Vec_t)(uintptr_t)P_SETPOS)(node, nullptr, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void touch(DWORD node, int slot) { __try { ((Touch_t)(uintptr_t)P_TOUCH)(node, nullptr, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static DWORD find_child(DWORD parent, const char* name) {
    if (!parent || !name || !*name) return 0;
    struct { DWORD out; const char* name; } q = {0, name};
    __try { ((Find_t)(uintptr_t)P_FINDCHILD)(parent, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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
static DWORD mission_data() { DWORD cd = gp_cache_data(); __try { DWORD m = cd ? RD32(cd + C_MISSION) : 0; return m ? RD32(m + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int mission_index() { DWORD md = mission_data(); __try { return md ? (int)RD32(md + 0x38) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static int combo_blocked() { DWORD md = mission_data(); __try { return md ? (RD32(md + MD_NO_COMBO) ? 1 : 0) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static bool game_seconds(double* t) { __try { *t = (double)(DWORD)RD32(A_GAME_MS) / 1000.0; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static dr::Vec3 up(dr::Vec3 p, float dy) { p.y += dy; return p; }

// ---- line test (Navigator::IsPointOccluded over the scene probe, as Suave / PSYCHO)
static bool g_ray_ok = true;
static bool blocked(const dr::Vec3& a, const dr::Vec3& b, DWORD ignore) {
    if (!g_ray_ok) return false;
    static BYTE self[0x80];
    *(DWORD*)(self + 0x70) = ignore;
    float f[4] = {a.x, a.y, a.z, 0.0f}, t[4] = {b.x, b.y, b.z, 0.0f};
    BYTE h = 0;
    Cur k = cur_save();
    __try { if (!RD32(A_SCENE) || !RD32(RD32(A_SCENE) + 0xA8)) { cur_restore(k); return false; } h = ((Occl_t)(uintptr_t)P_OCCLUDED)(self, nullptr, f, t); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_ray_ok = false; gp_log("drops: line test faulted: line tests off\n"); }
    cur_restore(k);
    return h != 0;
}
// where a line straight down from 1 m above the spot first hits (3 m below it at most), 1.6 cm steps
static bool ground_y(const dr::Vec3& at, float* y, DWORD ignore) {
    dr::Vec3 top = up(at, 1.0f), bot = up(at, -3.0f);
    if (!blocked(top, bot, ignore)) return false;
    float lo = bot.y, hi = top.y;
    for (int i = 0; i < 8; ++i) { float mid = (lo + hi) * 0.5f; dr::Vec3 m = at; m.y = mid; if (blocked(top, m, ignore)) lo = mid; else hi = mid; }
    *y = hi; return true;
}
// the floor under a pickup's footprint: the centre and four points footprint m around it (dr::rest_y picks the height)
static int footprint(const dr::Vec3& p, float r, DWORD ignore, float* g, bool* hit) {
    static const float D[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int n = r > 0.01f ? 5 : 1;
    for (int i = 0; i < n; ++i) { dr::Vec3 q = p; q.x += D[i][0] * r; q.z += D[i][1] * r; hit[i] = ground_y(q, &g[i], ignore); }
    return n;
}

// ---- hostility (the Tornado's faction test: relations row 0 = the player's, 4 = hostile)
static DWORD relations() {
    DWORD cd = gp_cache_data(); if (!cd) return 0;
    DWORD rel = 0;
    __try { DWORD ft = RD32(cd + C_FACTIONS); DWORD fd = ft ? RD32(ft + 0x18) : 0; DWORD tab = fd ? RD32(fd + 0x20) : 0; if (!get_member(tab, 0, &rel)) rel = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { rel = 0; }
    return rel;
}
static bool hostile_to_player(DWORD root, DWORD rel_list) {
    int fac = -1;
    if (!get_prop(root, PROP_FACTION, &fac) || fac < 0 || fac > 255) return false;
    DWORD row = list_at(rel_list, 0, A_LIST_ENTITY);
    if (!row) return false;
    __try {
        DWORD k = list_stride(A_LIST_INT); DWORD a = RD32(row + 4); DWORD d = a ? RD32(a + 4) : 0;
        if (!k || !d || fac >= (int)RD32(row + 8)) return false;
        return (int)RD32(d + (DWORD)fac * 4 * k) == 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Ram
static bool ram_health(DWORD ram, float* hp, float* dmg) {
    DWORD ai = sub(ram, R_AI);
    if (cls_handler(ai) != H_PLAYER_AI) return false;
    __try { DWORD a = data(ai); if (!a) return false; *hp = RDF(a + 0x1C); *dmg = RDF(a + 0x20); return *hp > 0.0f; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Ram's power-up counts and maxima by slot (character_powerup_ctrl data +14h -> {counts, maxima}, 0x542DF0)
static bool powerups(DWORD ram, int* counts, int* maxima, int n) {
    for (int i = 0; i < n; ++i) counts[i] = maxima[i] = -1;
    __try {
        DWORD pd = data(sub(ram, R_POWERUP)); DWORD p = pd ? RD32(pd + 0x14) : 0; if (!p) return false;
        DWORD k = list_stride(A_LIST_INT); if (!k) return false;
        DWORD L0 = RD32(p), L1 = RD32(p + 4);
        int c0 = list_count(L0), c1 = list_count(L1);
        DWORD d0 = RD32(RD32(L0 + 4) + 4), d1 = RD32(RD32(L1 + 4) + 4);
        for (int i = 0; i < n; ++i) {
            if (i < c0) counts[i] = (int)RD32(d0 + (DWORD)i * 4 * k);
            if (i < c1) maxima[i] = (int)RD32(d1 + (DWORD)i * 4 * k);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD gun_of(DWORD root) {
    DWORD wd = data(sub(root, R_WEAPON)), gun = 0;
    __try { gun = wd ? RD32(wd + 0x20) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { gun = 0; }
    return gun && cls_handler(gun) == H_WEAPON ? gun : 0;
}
static bool gun_bullets(DWORD gun, int* to_drop, int* max_drop) {
    DWORD d = data(gun); if (!d || cls_handler(gun) != H_WEAPON) return false;
    __try { *to_drop = (int)RD32(d + W_TODROP); *max_drop = (int)RD32(d + W_MAXDROP); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ the goodie pool
static DWORD resource_ctrl() {
    DWORD cd = gp_cache_data();
    __try { DWORD m = cd ? RD32(cd + C_MASTER) : 0; DWORD md = m ? RD32(m + 0x18) : 0; return md ? RD32(md + M_RESOURCE) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool node_show(DWORD node) {            // the game's show (0x62E110): SetFlags with bit 0 clear, refused while holder +88h is set
    __try {
        DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0;
        if (!d || !node || RD32(d + 0x88)) return false;
        ((Flags_t)(uintptr_t)RD32(RD32(node) + 0x10))(node, nullptr, RD32(node + 0x28) & 0xFFE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int pool_props(DWORD* out, int cap) {
    int n = 0;
    __try {
        DWORD rd = data(resource_ctrl()); if (!rd) return 0;
        DWORD tl = RD32(A_LIST_LISTS), tr = RD32(A_LIST_RES); if (!tl || !tr) return 0;
        DWORD k1 = RD32(tl + 0x24), k2 = RD32(tr + 0x24); if (!k1 || !k2) return 0;
        DWORD L = RD32(rd + RC_LIST); DWORD a = L ? RD32(L + 4) : 0; DWORD d = a ? RD32(a + 4) : 0; if (!d) return 0;
        DWORD inner = RD32(d + k1 * RES_GOODIE * 4); if (!inner) return 0;
        int c = (int)(RD32(inner + 8) / k2);
        DWORD b = RD32(inner + 4); DWORD e = b ? RD32(b + 4) : 0; if (!e) return 0;
        for (int i = 0; i < c && n < cap; ++i) { DWORD p = RD32(e + k2 * (DWORD)i * 4); if (p && RD32(p)) out[n++] = RD32(p); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}
static bool prop_state(DWORD prop, int* status, DWORD* placeholder) {
    __try { DWORD d = data(prop); if (!d) return false; *status = (int)RD32(d + P_STATUS); *placeholder = RD32(d + P_PLACEHOLDER); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int prop_subtype(DWORD prop) { __try { DWORD d = data(prop); return d ? (int)RD32(d + P_SUBTYPE) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static bool free_prop(DWORD prop) { DWORD p = prop; return send(resource_ctrl(), MSG_RES_FREE, &p); }
static DWORD goodie_data(DWORD prop) { DWORD d = data(prop); DWORD c = 0; __try { c = d ? RD32(d + P_CTRL) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { c = 0; } return data(c); }
static bool orgpos(DWORD prop, dr::Vec3* o) {
    DWORD g = goodie_data(prop); if (!g) return false;
    __try { o->x = RDF(g + G_ORGPOS); o->y = RDF(g + G_ORGPOS + 4); o->z = RDF(g + G_ORGPOS + 8); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// a level goodie on the ground: status 1 with a placeholder (the game's command_allocate_resource)
static bool level_goodie(DWORD prop, DWORD* ph) {
    int st = 0; DWORD p = 0;
    if (!prop_state(prop, &st, &p) || st != 1 || !p) return false;
    if (ph) *ph = p; return true;
}

// the pool's five props: level goodies first, nearest placements first. None free: the level goodie furthest from Ram (at
// least steal_distance) gives its prop back, what the pool itself does to a prop out of range (0x75EC9C); its placement gets
// one again when a pickup ends
static bool steal_prop(const dr::Vec3& ram) {
    DWORD props[16], best = 0, bph = 0; float bd = g_c.steal_distance; int n = pool_props(props, 16);
    for (int i = 0; i < n; ++i) {
        DWORD ph = 0; dr::Vec3 p;
        if (!level_goodie(props[i], &ph) || !get_pos(props[i], &p)) continue;
        __try { if (RD32(data(props[i]) + P_LOCKED)) continue; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        float d = dr::dist_xz(p, ram);
        if (d > bd) { bd = d; best = props[i]; bph = ph; }
    }
    if (!best) return false;
    command(bph, CMD_GIVEUP_RESOURCE, nullptr);
    bool ok = free_prop(best);
    gp_log("drops: goodie pool empty: the level goodie %.0f m from Ram lends its prop %08X%s\n", bd, best, ok ? "" : " (FAILED)");
    return ok;
}

static DWORD spawn_prop(int goodie, const dr::Vec3& p, float seconds, dr::Vec3* org_before) {
    DWORD rc = resource_ctrl(), rd = data(rc), prop = 0, pd = 0, ai = 0;
    if (!rd) return 0;
    int type = RES_GOODIE;
    if (!send(rc, MSG_RES_REQUEST, &type)) return 0;
    __try { if (!RD32(rd + RC_SUCCESS)) return 0; prop = RD32(rd + RC_AVAILABLE); pd = data(prop); ai = pd ? RD32(pd + P_AI) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!pd || !ai) return 0;
    // taken, shown, status 3 (never the game's 1: a status 1 prop without a placeholder is freed by the pool within a second)
    __try { touch(prop, SLOT_P_STATUS); WR32(pd + P_STATUS, ST_MINE); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    node_show(prop);
    __try { touch(prop, SLOT_P_SUBTYPE); WR32(pd + P_SUBTYPE, goodie); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // the spot first: Goodie_Default command_reset stores the prop's position of that moment as OrgPos (0x68AC8B)
    set_pos(prop, p);
    send(prop, MSG_PROP_RESET, nullptr);
    set_pos(prop, p);
    dr::Vec3 o = {0, 0, 0}; bool oo = orgpos(prop, &o);
    if (org_before) *org_before = oo ? o : p;
    if (g_fix_orgpos && oo && (fabsf(o.x - p.x) > 0.01f || fabsf(o.y - p.y) > 0.01f || fabsf(o.z - p.z) > 0.01f)) {
        DWORD g = goodie_data(prop);
        __try { touch(sub(prop, P_CTRL), SLOT_G_ORGPOS); WRF(g + G_ORGPOS, p.x); WRF(g + G_ORGPOS + 4, p.y); WRF(g + G_ORGPOS + 8, p.z); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try { touch(ai, SLOT_AI_TIMER); WRF(data(ai) + PA_TIMER, seconds); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return prop;
}

// ================================================================================================ state
struct Drop { bool used, taken; DWORD prop; uint32_t key; int goodie, slot, count0; float dmg_last; dr::Vec3 pos; double t0, t_end, t_taken; };
static dr::Book g_book;
static dr::Rng  g_rng;
static Drop     g_drops[5];
static DWORD    g_ram0 = 0; static int g_mission0 = -2;
static double   g_scan_t = 0, g_sweep_t = 0, g_clock = -1, g_pu_t = 0, g_bd_t = 0, g_kill_t = 0, g_kill_t0 = 0, g_probe_t = 0, g_sum_t = 0;
static bool     g_back = false;
static int      g_pu_last[12];
static bool     g_pu_known = false;
struct Stats { int deaths, bullets, guns, health_m, health_l, powerups, collected, expired, waited_out, nothing, elite_skips; };
static Stats    g_s, g_s_mission;
static volatile LONG g_probe_req = 0, g_test_req = 0;
static int      g_walk_frames = 0, g_mark_n = 0;
static double   g_perf_t0 = 0, g_perf_sum = 0, g_perf_max = 0; static int g_perf_n = 0;
// BulletDrop written here when [difficulty] does not tune it
static float    g_bd_base[10], g_bd_written[10];
static bool     g_bd_captured = false, g_bd_refused = false;
static int      g_bd_tries = 0;

static void mark(const char* tag) { if (g_marks) gp_log("debug keys: mark shot dr%02d_%s\n", g_mark_n++, tag); }
static int active_drops() { int n = 0; for (const Drop& d : g_drops) if (d.used && !d.taken) ++n; return n; }

// ================================================================================================ dropped ammo
static void bulletdrop_tick(double t) {
    if (!g_bd_write || g_bd_refused || t < g_bd_t) return;
    g_bd_t = t + 2.0;
    if (difficulty_owns_ammo()) return;        // gp_difficulty.cpp writes the bands with loot_ammo_factor()
    DWORD cd = gp_cache_data(); DWORD bd = data(cd ? RD32(cd + C_BULLETDROP) : 0);
    if (!bd) return;
    float f = dr::ammo_factor(g_c, 1.0f);
    int changed = 0, adopted = 0;
    __try {
        if (!g_bd_captured) {
            if (++g_bd_tries > 20) { g_bd_refused = true; return; }
            if (fabsf(RDF(bd + 0x0C) - 10.0f) > 1e-4f || fabsf(RDF(bd + 0x30) - 250.0f) > 1e-4f) {
                gp_log("drops: BulletDrop does not hold the vanilla bands (%.1f .. %.1f): dropped ammo left alone\n", RDF(bd + 0x0C), RDF(bd + 0x30));
                g_bd_refused = true; return;
            }
            for (int i = 0; i < 10; ++i) g_bd_base[i] = g_bd_written[i] = RDF(bd + 0x0C + (DWORD)i * 4);
            g_bd_captured = true;
        }
        for (int i = 0; i < 10; ++i) {
            DWORD a = bd + 0x0C + (DWORD)i * 4; float cur = RDF(a);
            if (cur != g_bd_written[i]) { g_bd_base[i] = cur; ++adopted; }
            float nv = g_bd_base[i] * f;
            if (nv != cur) { WRF(a, nv); ++changed; }
            g_bd_written[i] = nv;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_bd_refused = true; return; }
    if (changed) gp_log("drops: dropped ammo x%.2f written to the BulletDrop bands (%d values%s; [difficulty] tuning is off)\n", f, changed, adopted ? ", some re-read" : "");
}

// ================================================================================================ the enemies
static bool special_enemy(DWORD ai) {
    __try {
        DWORD a = data(ai); if (!a) return true;
        int sbt = (int)RD32(a + S_SUBTYPE); if (sbt == 4 || sbt == 7 || sbt == 8 || sbt == 9) return true;   // escort / player / never aggressive / civilian
        return RD32(a + 0xB8) != 0;                                                                            // a buddy
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
// hostile living enemies around Ram (the quadtree query PSYCHO / Smart AI use, mask 40h = characters)
static void book_update(DWORD ram, const dr::Vec3& rp, double t) {
    DWORD rel = relations(); if (!rel) return;
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp.x, rp.y, rp.z, g_c.track_radius, 0x40, 0};
    Cur k = cur_save();
    __try { ((Query_t)(uintptr_t)P_QUERY)(RD32(A_QUADTREE), nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { q.out = 0; }
    cur_restore(k);
    DWORD L = q.out;
    for (int i = 0, c = list_count(L); i < c; ++i) {
        DWORD r = list_at(L, i, A_LIST_ENTITY);
        if (!r || r == ram || cls_handler(r) != H_ROOT) continue;
        DWORD d = data(r), ai = 0, ph = 0; float hp = 0, dmg = 0; int st = -1;
        if (!d) continue;
        __try {
            ai = RD32(d + R_AI); ph = RD32(d + R_PLACEHOLDER);
            DWORD sbt = RD32(d + R_SUBTYPE); if (sbt == 7 || sbt == 8) continue;   // not killable
            if (!ai || cls_handler(ai) != H_SENTRY) continue;
            DWORD a = data(ai); hp = RDF(a + 0x2C); dmg = RDF(a + 0x30); st = (int)RD32(a + 0x1D4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (dr::life_of(true, hp, dmg, st) != dr::LIFE_ALIVE || special_enemy(ai) || !hostile_to_player(r, rel)) continue;
        uint32_t key = ph ? ph : r;
        dr::Body* b = g_book.find(key);
        if (b && (b->root != r || b->ai != ai || b->done || b->dead)) {   // the placement is back (a re-arm / heat wave): a new life
            if (b->dead && !b->done) continue;                             // its last life still waits for its pickup
            *b = dr::Body(); b->used = true; b->key = key; b->t_seen = t;
        }
        if (!b) { b = g_book.add(key, t); if (!b) continue; }
        b->root = r; b->ai = ai; b->ph = ph; b->health = hp; b->alive_seen = true; b->t_seen = t;
        b->elite = g_ps_drops && difficulty_enemy_loco_moves() && hp >= g_ps_elite && (hp < g_ps_boss || g_ps_bosses);
        DWORD gun = gun_of(r); if (gun) b->gun = gun;
        dr::Vec3 p; if (get_pos(r, &p)) b->last_alive = p;
    }
    k = cur_save();
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (L && lt) ((Free_t)(uintptr_t)P_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}

static void book_watch(double t, int pm, bool after_gap) {
    for (dr::Body& b : g_book.b) {
        if (!b.used || !b.alive_seen || b.dead) continue;
        float hp = 0, dmg = 0; int st = -1; bool same = false;
        __try {
            same = sub(b.root, R_AI) == b.ai && sub(b.root, R_PLACEHOLDER) == b.ph;
            DWORD a = data(b.ai); if (same && a) { hp = RDF(a + 0x2C); dmg = RDF(a + 0x30); st = (int)RD32(a + 0x1D4); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
        int life = dr::life_of(same, hp, dmg, st);
        if (life == dr::LIFE_GONE) { b.alive_seen = false; continue; }   // the shell went to another placement: nothing died
        if (life == dr::LIFE_ALIVE) { dr::Vec3 p; if (get_pos(b.root, &p)) b.last_alive = p; DWORD g = gun_of(b.root); if (g) b.gun = g; continue; }
        b.dead = true; b.t_dead = t; b.rest_ok = false; b.next_try = 0; b.goodie = -2;
        b.no_drop = gp_cutscene() || pm == 3 || after_gap;
        DWORD anim = sub(b.root, R_ANIM);
        b.pelvis = anim ? find_path(anim, "Bip/Pelvis") : 0;
        int to = -1, mx = -1; bool gb = b.gun && gun_bullets(b.gun, &to, &mx);
        ++g_s.deaths; ++g_s_mission.deaths;
        if (gb) { g_s.bullets += to; g_s_mission.bullets += to; ++g_s.guns; ++g_s_mission.guns; }
        gp_log("drops: enemy %08X (placement %06X, hp %.0f) died: its gun drops %d bullets (max %d)%s%s\n", b.root, (b.ph ? RD32(b.ph + 0x14) : RD32(b.root + 0x14)) >> 8, hp,
               gb ? to : -1, gb ? mx : -1, b.elite ? ", an elite (PSYCHO elite drops)" : "", b.no_drop ? ", no pickup (cut-scene / rewind)" : "");
    }
}

// the spot for this body's pickup: beside the body (the gun lies on it) toward Ram, off walls, on the floor under its footprint
static bool place_for(const dr::Body& b, const dr::Vec3& ram, bool ram_ok, dr::Vec3* out, bool* on_body, float* centre_y, float* cal) {
    dr::Vec3 body = b.rest; float y = 0.0f;
    { float gs = 0; bool h = ground_y(b.last_alive, &gs, b.root); *cal = dr::floor_offset(g_c, b.last_alive.y, gs, h); }   // where it stood
    bool body_ok = b.rest_ok && ground_y(body, &y, b.root);
    if (body_ok) body.y = y;
    // the line test only matters when the body lies within reach of where it stood (dr::drop_spot); a long line test
    // through the level (a body flung far, a bad position) is skipped instead of run
    bool in_reach = body_ok && dr::dist_xz(b.last_alive, body) <= g_c.body_reach && b.last_alive.y - body.y <= g_c.body_drop;
    bool clear = in_reach && !blocked(up(b.last_alive, 0.6f), up(body, 0.6f), b.root);
    dr::Vec3 p = dr::drop_spot(g_c, b.last_alive, body, body_ok, clear, on_body);
    if (!*on_body) { if (ground_y(p, &y, b.root)) p.y = y; else return false; }
    if (ram_ok) {
        dr::Vec3 q = dr::beside(p, ram, g_c.gun_gap);
        if (!blocked(up(p, 0.4f), up(q, 0.4f), b.root) && ground_y(q, &y, b.root) && fabsf(y - p.y) < 0.5f) { p = q; p.y = y; }
    }
    static const float DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    dr::Vec3 c = up(p, 0.4f); float px = 0.0f, pz = 0.0f;
    for (const float* d : DIRS) { dr::Vec3 t = c; t.x += d[0] * 0.5f; t.z += d[1] * 0.5f; if (blocked(c, t, b.root)) { px -= d[0] * 0.4f; pz -= d[1] * 0.4f; } }
    if (px != 0.0f || pz != 0.0f) {
        dr::Vec3 t = c; t.x += px; t.z += pz;
        if (!blocked(c, t, b.root) && ground_y(t, &y, b.root) && fabsf(y - p.y) < 0.5f) { p.x = t.x; p.z = t.z; p.y = y; }
    }
    float g[5]; bool h[5]; int n = footprint(p, g_c.footprint, b.root, g, h);
    float ry = dr::rest_y(g, h, n, g_c.lift, g_c.max_step);
    if (!(ry == ry)) return false;
    *centre_y = h[0] ? g[0] : p.y;
    p.y = ry + *cal;
    *out = p; return true;
}

static bool make_drop(uint32_t key, int goodie, const dr::Vec3& at, double t, const dr::Vec3& ram, bool ram_ok, const char* why) {
    Drop* d = nullptr;
    for (Drop& x : g_drops) { if (!x.used) { d = &x; break; } }
    if (!d) for (Drop& x : g_drops) if (x.taken && (!d || x.t_taken < d->t_taken)) d = &x;
    if (!d || active_drops() >= g_c.max_on_ground) return false;
    dr::Vec3 org = at;
    DWORD prop = spawn_prop(goodie, at, g_c.drop_seconds, &org);
    if (!prop && ram_ok && steal_prop(ram)) prop = spawn_prop(goodie, at, g_c.drop_seconds, &org);
    if (!prop) return false;
    DWORD ram_r = ram_root(); float hp = 0, dmg = 0; ram_health(ram_r, &hp, &dmg);
    int counts[12], maxima[12]; powerups(ram_r, counts, maxima, 12);
    int slot = dr::slot_of_goodie(goodie);
    d->used = true; d->taken = false; d->prop = prop; d->key = key; d->goodie = goodie; d->slot = slot;
    d->count0 = slot >= 0 ? counts[slot] : -1; d->dmg_last = dmg; d->pos = at; d->t0 = t; d->t_end = t + g_c.drop_seconds; d->t_taken = 0;
    if (goodie == dr::G_MEDIUM_HEALTH) { ++g_s.health_m; ++g_s_mission.health_m; }
    else if (goodie == dr::G_LARGE_HEALTH) { ++g_s.health_l; ++g_s_mission.health_l; }
    else { ++g_s.powerups; ++g_s_mission.powerups; }
    dr::Vec3 now = at; get_pos(prop, &now);
    gp_log("drops: PICKUP %s (%s) at (%.2f %.2f %.2f): prop %08X, stays %.0f s (%d on the ground; reset OrgPos (%.2f %.2f %.2f)%s)\n", dr::goodie_name(goodie), why,
           at.x, at.y, at.z, prop, g_c.drop_seconds, active_drops(), org.x, org.y, org.z,
           fabsf(org.y - at.y) > 0.01f || fabsf(org.x - at.x) > 0.01f ? (g_fix_orgpos ? ", put on the spot" : ", NOT the spot") : "");
    mark("pickup");
    return true;
}

static void bodies_tick(double t, int pm, const dr::Vec3& ram, bool ram_ok) {
    bool can = g_c.enabled && pm != 2 && pm != 3 && !gp_cutscene();
    for (dr::Body& b : g_book.b) {
        if (!b.used || !b.dead || b.done || b.no_drop) continue;
        if (sub(b.root, R_AI) == b.ai && sub(b.root, R_PLACEHOLDER) == b.ph) {   // the shell not reused yet: follow the body (ragdoll)
            dr::Vec3 p;
            if (get_pos(b.pelvis ? b.pelvis : b.root, &p)) {
                float dx = p.x - b.rest.x, dy = p.y - b.rest.y, dz = p.z - b.rest.z;
                if (!b.rest_ok || dx * dx + dy * dy + dz * dz > 0.0004f) { b.t_rest = t; b.rest = p; b.rest_ok = true; }
            }
        }
        if (!can || !dr::body_settled(g_c, b, t) || t < b.next_try) continue;
        if (b.goodie == -2) {                  // roll once per death
            DWORD r = ram_root(); float hp = 0, dmg = 0; int counts[12], maxima[12];
            dr::RollIn in;
            if (ram_health(r, &hp, &dmg)) in.ram_health_frac = (hp - dmg) / hp;
            if (powerups(r, counts, maxima, 12)) { in.counts = counts; in.maxima = maxima; }
            in.elite_drop = b.elite; in.excluded_slot = g_excluded_slot;
            dr::Roll ro = dr::roll(g_c, g_rng, in);
            b.goodie = ro.goodie;
            if (b.elite && g_c.elite_skip_powerup) ++g_s.elite_skips;
            if (ro.goodie < 0) { ++g_s.nothing; ++g_s_mission.nothing; b.done = true; if (g_verbose) gp_log("drops: %08X leaves nothing more (%s; health %.2f, power-up %.2f)\n", b.root, ro.why, ro.p_health, ro.p_powerup); continue; }
            if (g_verbose) gp_log("drops: %08X rolled %s (%s; health %.2f, power-up %.2f; Ram %.0f%%)\n", b.root, dr::goodie_name(ro.goodie), ro.why, ro.p_health, ro.p_powerup, in.ram_health_frac * 100.0f);
        }
        if (t > b.t_dead + g_c.give_up) {
            b.done = true; ++g_s.waited_out;
            gp_log("drops: pickup of %08X given up after %.0f s: no goodie prop came free\n", b.root, g_c.give_up);
            continue;
        }
        dr::Vec3 at; bool on_body = false; float cy = 0, cal = 0;
        if (!place_for(b, ram, ram_ok, &at, &on_body, &cy, &cal)) { b.done = true; gp_log("drops: pickup of %08X: no floor under the body or where it stood: none\n", b.root); continue; }
        if (make_drop(b.key, b.goodie, at, t, ram, ram_ok, on_body ? "beside the body" : "where it stood")) {
            b.done = true;
            if (g_verbose) gp_log("drops: ... %.2f s after the death; line test floor %.2f, + %.2f (it stood at %.2f), rests at %.2f\n", t - b.t_dead, cy, cal, b.last_alive.y, at.y);
        } else {
            b.next_try = t + 0.5;
            if (g_verbose) gp_log("drops: pickup of %08X waits: no goodie prop free or %d on the ground\n", b.root, g_c.max_on_ground);
        }
    }
}

static void drops_watch(double t, DWORD ram, const dr::Vec3& rp, bool ram_ok) {
    float hp = 0, dmg = 0; bool hk = ram_health(ram, &hp, &dmg);
    int counts[12], maxima[12]; powerups(ram, counts, maxima, 12);
    for (Drop& d : g_drops) {
        if (!d.used) continue;
        if (d.taken) { if (t > d.t_taken + 15.0) d.used = false; continue; }
        int st = 0; DWORD ph = 0;
        if (!prop_state(d.prop, &st, &ph)) { d.used = false; continue; }
        if (st == ST_MINE && ph == 0 && t < d.t_end) {
            if (d.slot >= 0 && counts[d.slot] >= 0 && counts[d.slot] < d.count0) d.count0 = counts[d.slot];   // a charge spent meanwhile
            if (hk) d.dmg_last = dmg;
            continue;
        }
        if (st == ST_MINE && ph == 0) {        // its time is up
            free_prop(d.prop); ++g_s.expired; ++g_s_mission.expired;
            gp_log("drops: PICKUP %s (prop %08X) stayed %.0f s: gone\n", dr::goodie_name(d.goodie), d.prop, g_c.drop_seconds);
            d.used = false; continue;
        }
        bool got = d.slot >= 0 ? (counts[d.slot] > d.count0) : ((hk && dmg < d.dmg_last - 0.5f) || (ram_ok && dr::dist_xz(rp, d.pos) < 2.5f));
        if (got) {
            ++g_s.collected; ++g_s_mission.collected; d.taken = true; d.t_taken = t;
            if (d.slot >= 0) gp_log("drops: PICKUP %s COLLECTED after %.1f s: Ram holds %d (was %d)\n", dr::goodie_name(d.goodie), t - d.t0, counts[d.slot], d.count0);
            else gp_log("drops: PICKUP %s COLLECTED after %.1f s: Ram's damage %.0f -> %.0f of %.0f\n", dr::goodie_name(d.goodie), t - d.t0, d.dmg_last, dmg, hp);
            mark("collected");
        } else {
            ++g_s.expired; ++g_s_mission.expired;
            gp_log("drops: PICKUP %s gone after %.1f s of %.0f (its own timer, or the game took the prop)\n", dr::goodie_name(d.goodie), t - d.t0, g_c.drop_seconds);
            d.used = false;
        }
    }
}

// the clock went back: a load / retry forgets everything (the sweep frees props a save brought back); a rewind undoes every
// death after the new time (its pickup goes), and a pickup made after it is made again for a body that stays dead
static void clock_back(double t, double back) {
    if (dr::clock_jump_is_load(back)) {
        if (g_book.count() || active_drops()) gp_log("drops: the clock jumped %.0f s (load / retry): %d enemies and %d pickups forgotten\n", back, g_book.count(), active_drops());
        g_book.clear(); for (Drop& d : g_drops) d.used = false;
        return;
    }
    int counts[12], maxima[12]; powerups(ram_root(), counts, maxima, 12);
    for (Drop& d : g_drops) {
        if (!d.used || !d.taken || t >= d.t_taken) continue;
        int st = 0; DWORD ph = 0;
        if (prop_state(d.prop, &st, &ph) && st == ST_MINE && !ph) { d.taken = false; d.count0 = d.slot >= 0 ? counts[d.slot] : -1; gp_log("drops: rewind before a pickup: it lies there again\n"); }
        else d.used = false;
    }
    for (dr::Body& b : g_book.b) {
        if (!b.used || !b.dead || t >= b.t_dead) continue;
        b.dead = false; b.done = false; b.goodie = -2; b.rest_ok = false;
        for (Drop& d : g_drops) if (d.used && d.key == b.key && d.t0 >= b.t_dead) { if (!d.taken) free_prop(d.prop); d.used = false; }
        gp_log("drops: rewind before the death of %08X: it lives again%s\n", b.root, "");
    }
    for (Drop& d : g_drops) {
        if (!d.used || d.taken || t >= d.t0) continue;
        free_prop(d.prop); d.used = false;
        dr::Body* b = g_book.find(d.key); if (b) { b->done = false; b->next_try = 0; }
        gp_log("drops: rewind before a pickup of %08X: made again\n", d.key);
    }
}

static void sweep(double t) {                  // a status 3 prop that is not one of ours: a save brought it back
    if (t < g_sweep_t) return;
    g_sweep_t = t + 1.0;
    DWORD props[16]; int n = pool_props(props, 16);
    for (int i = 0; i < n; ++i) {
        int st = 0; DWORD ph = 0;
        if (!prop_state(props[i], &st, &ph) || st != ST_MINE || ph) continue;
        bool mine = false; for (Drop& d : g_drops) if (d.used && !d.taken && d.prop == props[i]) mine = true;
        if (mine) continue;
        free_prop(props[i]);
        gp_log("drops: goodie prop %08X was a pickup of an earlier session (a save): given back to the pool\n", props[i]);
    }
}

static void summary(const char* why, const Stats& s) {
    gp_log("drops: %s: %d enemy deaths, guns dropped %d bullets (%.1f a gun), pickups: %d medium health, %d large health, %d power-ups "
           "(%d collected, %d gone), %d deaths left nothing more%s\n", why, s.deaths, s.bullets, s.guns ? (double)s.bullets / s.guns : 0.0,
           s.health_m, s.health_l, s.powerups, s.collected, s.expired, s.nothing, g_c.enabled ? "" : " ([drops] off: the log only)");
}

// ---- power-ups Ram holds (the Pinata is slot 10): every change logged, with the mission's combo flag
static void powerup_log(DWORD ram, double t) {
    if (t < g_pu_t) return;
    g_pu_t = t + 0.5;
    int counts[12], maxima[12];
    if (!powerups(ram, counts, maxima, 12)) return;
    static const char* const SLOT[12] = {"slot0", "rewind", "golden_gun", "tornado", "toro", "slot5", "sombrero", "slot7", "wrestler", "mariachi", "pinata", "slot11"};
    if (g_pu_known) for (int i = 0; i < 12; ++i)
        if (counts[i] != g_pu_last[i]) gp_log("drops: Ram's %s %d -> %d (max %d)\n", SLOT[i], g_pu_last[i], counts[i], maxima[i]);
    memcpy(g_pu_last, counts, sizeof g_pu_last); g_pu_known = true;
}

// ================================================================================================ test copy helpers
static void probe(DWORD ram, const dr::Vec3& rp, double t) {
    DWORD props[16]; int n = pool_props(props, 16);
    float gr = 0; bool hr = ground_y(rp, &gr, ram);
    gp_log("drops: probe at %.2f s: Ram (%.2f %.2f %.2f), line test floor under him %.2f, mission %d (combo awards %s), goodie pool %d props\n", t, rp.x, rp.y, rp.z,
           hr ? gr : NAN, mission_index(), combo_blocked() == 1 ? "BLOCKED" : combo_blocked() == 0 ? "on" : "?", n);
    for (int i = 0; i < n; ++i) {
        int st = 0; DWORD ph = 0; dr::Vec3 p = {0, 0, 0}, pp = {0, 0, 0}, o = {0, 0, 0};
        prop_state(props[i], &st, &ph); get_pos(props[i], &p); if (ph) get_pos(ph, &pp); orgpos(props[i], &o);
        float g[5]; bool h[5]; int m = footprint(p, 0.25f, props[i], g, h);
        float gy = h[0] ? g[0] : NAN, gmax = dr::rest_y(g, h, m, 0.0f, 0.35f);
        DWORD gd = goodie_data(props[i]);
        float maxy = 0, ang = 0, rot = 0;
        __try { if (gd) { maxy = RDF(gd + 0x44); ang = RDF(gd + 0x48); rot = RDF(gd + 0x4C); } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        gp_log("drops:   prop %08X status %d ph %08X sub %d flags %03X pos (%.2f %.2f %.2f) %.1f m from Ram | placeholder y %.2f | OrgPos (%.2f %.2f %.2f) "
               "MaxY %.2f angle %.2f rot %.2f | floor under it %.2f, footprint max %.2f -> prop is %.2f m above the floor\n",
               props[i], st, ph, prop_subtype(props[i]), RD32(props[i] + 0x28) & 0xFFF, p.x, p.y, p.z, dr::dist_xz(p, rp), ph ? pp.y : 0.0f, o.x, o.y, o.z,
               maxy, ang, rot, gy, gmax, h[0] ? p.y - gy : NAN);
    }
}
// a test pickup beside the nearest level goodie (the side toward Ram, 1.2 m), else 3 m ahead of Ram
static void test_drop(DWORD ram, const dr::Vec3& rp, double t) {
    DWORD props[16], best = 0; int n = pool_props(props, 16); float bd = 1e9f; dr::Vec3 bp = rp;
    for (int i = 0; i < n; ++i) { dr::Vec3 p; if (level_goodie(props[i], nullptr) && get_pos(props[i], &p)) { float d = dr::dist_xz(p, rp); if (d < bd) { bd = d; best = props[i]; bp = p; } } }
    dr::Vec3 at; dr::Vec3 f = {0, 0, 1}; get_fwd(ram, &f);
    if (best && bd < 30.0f) {
        dr::Vec3 dir = {rp.x - bp.x, 0, rp.z - bp.z}; float l = sqrtf(dir.x * dir.x + dir.z * dir.z);
        if (l < 1e-3f) { dir = f; l = 1; }
        dr::Vec3 side = {-dir.z / l, 0, dir.x / l};   // beside it, across Ram's view: both in the shot
        at = bp; at.x += side.x * 1.2f; at.z += side.z * 1.2f;
    } else { at = rp; at.x += f.x * 3.0f; at.z += f.z * 3.0f; }
    float g[5]; bool h[5]; int m = footprint(at, g_c.footprint, ram, g, h);
    float ry = dr::rest_y(g, h, m, g_c.lift, g_c.max_step);
    if (!(ry == ry)) { gp_log("drops: debug_drop_key: no floor at (%.2f %.2f %.2f)\n", at.x, at.y, at.z); return; }
    float gr = 0; bool hr = ground_y(rp, &gr, ram); float cal = dr::floor_offset(g_c, rp.y, gr, hr);   // Ram stands on the floor
    gp_log("drops: debug_drop_key: %s %s; line test floor %.2f, footprint max %.2f, + %.2f (Ram at %.2f, line test under him %.2f), rests at %.2f\n",
           dr::goodie_name(g_test_goodie), best && bd < 30.0f ? "1.2 m beside the nearest level goodie" : "3 m ahead of Ram (no level goodie within 30 m)",
           h[0] ? g[0] : NAN, ry - g_c.lift, cal, rp.y, hr ? gr : NAN, ry + cal);
    ry += cal;
    at.y = ry;
    if (!make_drop(0, g_test_goodie, at, t, rp, true, "test")) gp_log("drops: debug_drop_key: no prop\n");
    probe(ram, rp, t);
}
static void walk_to_goodie(DWORD ram, const dr::Vec3& rp) {
    if (!g_walk_goodie) return;
    DWORD props[16]; int n = pool_props(props, 16); float bd = 1e9f; dr::Vec3 bp = rp;
    for (int i = 0; i < n; ++i) { dr::Vec3 p; if (level_goodie(props[i], nullptr) && get_pos(props[i], &p)) { float d = dr::dist_xz(p, rp); if (d < bd) { bd = d; bp = p; } } }
    if (bd > 150.0f || bd < g_walk_stop) return;
    dr::Vec3 f; if (!get_fwd(ram, &f)) return;
    float want = atan2f(bp.x - rp.x, bp.z - rp.z), have = atan2f(f.x, f.z), a = want - have;
    while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f;
    if (fabsf(a) > 0.02f) gp_yaw_add(a * 57.29578f * 0.25f);
    if (fabsf(a) < 0.5f) g_walk_frames = 3;
}
static void debug_kill(DWORD ram, const dr::Vec3& rp, double t) {
    if (g_kill_every <= 0.0f || t < g_kill_t) return;
    if (g_kill_t0 <= 0) g_kill_t0 = t;
    if (g_kill_stop > 0.0f && t - g_kill_t0 > g_kill_stop) return;
    g_kill_t = t + g_kill_every;
    dr::Body* best = nullptr; float bd = g_kill_range;
    for (dr::Body& b : g_book.b) if (b.used && b.alive_seen && !b.dead) { float d = dr::dist_xz(b.last_alive, rp); if (d < bd) { bd = d; best = &b; } }
    if (!best) return;
    DWORD gun = gun_of(ram);
    struct Hit { float dmg; DWORD owner, inflictor, collvol; float imp[3]; float pos[3]; int type; };
    Hit h = {100000.0f, ram, gun ? gun : ram, 0, {0.0f, 0.0f, 0.0f}, {best->last_alive.x, best->last_alive.y + 1.0f, best->last_alive.z}, 1};
    __try {   // KillOwner / Killer as for Ram's own shot (the combo handler asks IsA(weapon) of the Killer: never 0)
        DWORD sd = data(best->ai);
        if (sd) { WR32(sd + 0x178, ram); WR32(sd + 0x174, gun ? gun : ram); if (!RD32(sd + 0x150)) WR32(sd + 0x150, ram); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    command(best->root, CMD_TAKE_A_HIT, &h);
    if (g_verbose) gp_log("drops: debug_kill_seconds: a hit of 100000 to %08X %.1f m from Ram\n", best->root, bd);
}

// ================================================================================================ tick
static void perf(double t0, double now) {
    double ms = (gp_now() - t0) * 1000.0;
    g_perf_sum += ms; ++g_perf_n; if (ms > g_perf_max) g_perf_max = ms;
    if (g_perf_log > 0.0f && now - g_perf_t0 >= g_perf_log) {
        if (g_perf_n) gp_log("drops: cost %.3f ms avg / %.3f ms max per tick over %d ticks (%d enemies tracked, %d pickups)\n", g_perf_sum / g_perf_n, g_perf_max, g_perf_n, g_book.count(), active_drops());
        g_perf_t0 = now; g_perf_sum = 0; g_perf_max = 0; g_perf_n = 0;
    }
}
static void level_change(DWORD ram, int mis) {
    int n = 0; for (Drop& d : g_drops) { if (d.used && !d.taken) ++n; d.used = false; }
    if (g_s_mission.deaths || n) summary("mission summary", g_s_mission);
    memset(&g_s_mission, 0, sizeof g_s_mission);
    g_book.clear(); g_back = false; g_scan_t = 0; g_sweep_t = 0; g_clock = -1; g_pu_known = false; g_kill_t = 0;
    g_ram0 = ram; g_mission0 = mis;
    int cb = combo_blocked();
    if (mis >= 0) gp_log("drops: mission %d: combo kill awards %s (mission data +78h)\n", mis, cb == 1 ? "BLOCKED in this mission" : cb == 0 ? "on" : "unknown");
}

void loot_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed) return;
    double t0 = gp_now();
    DWORD ram = ram_root();
    int mis = mission_index();
    if (ram != g_ram0 || mis != g_mission0) level_change(ram, mis);
    double t = 0;
    if (!game_seconds(&t)) return;
    int pm = gp_playmode();
    bool after_gap = g_clock >= 0.0 && t > g_clock + 1.0;   // game time passed without the tick: a cut-scene
    if (g_clock >= 0.0 && t < g_clock - 0.001) {
        double back = g_clock - t;
        if (back > 0.05 || pm == 3) g_back = true;
        if (dr::clock_jump_is_load(back)) { clock_back(t, back); g_back = false; }
    }
    g_clock = t;
    bulletdrop_tick(t);
    if (pm == 3) return;                       // a rewind plays: looked at when it is over
    if (g_back) { g_back = false; clock_back(t, 0.0); }
    if (pm == 2) return;
    dr::Vec3 rp = {0, 0, 0}; bool ram_ok = ram && get_pos(ram, &rp);
    if (ram_ok && t >= g_scan_t) { g_scan_t = t + 0.5; book_update(ram, rp, t); }
    book_watch(t, pm, after_gap);
    bodies_tick(t, pm, rp, ram_ok);
    drops_watch(t, ram, rp, ram_ok);
    sweep(t);
    if (ram_ok) powerup_log(ram, t);
    if (now - g_sum_t >= 60.0) { g_sum_t = now; if (g_s.deaths) summary("session so far", g_s); }
    if (ram_ok) {
        if (InterlockedExchange(&g_probe_req, 0) || (g_probe_every > 0.0f && t >= g_probe_t)) { g_probe_t = t + g_probe_every; probe(ram, rp, t); }
        if (InterlockedExchange(&g_test_req, 0)) test_drop(ram, rp, t);
        debug_kill(ram, rp, t);
        walk_to_goodie(ram, rp);
    }
    perf(t0, now);
}

void loot_post_input(BYTE* edges, BYTE* state, double now) {
    (void)edges; (void)now;
    if (!g_installed || !state) return;
    static bool hp = false, ht = false;
    if (g_probe_key >= 0 && g_probe_key < 256) { bool h = (state[g_probe_key] & 0x80) != 0; if (h && !hp) InterlockedExchange(&g_probe_req, 1); hp = h; }
    if (g_test_key >= 0 && g_test_key < 256) { bool h = (state[g_test_key] & 0x80) != 0; if (h && !ht) InterlockedExchange(&g_test_req, 1); ht = h; }
    if (g_walk_frames > 0) { __try { state[0x11 /*W*/] = 0x80; } __except (EXCEPTION_EXECUTE_HANDLER) {} --g_walk_frames; }
}

// ================================================================================================ install
static const char* REFS[] = {
    // shared (the same addresses, the same use as Suave / Smart AI / PSYCHO)
    "F_SENDMSG", "F_CMD", "F_GETPROP", "F_GETMEMBER", "F_QUERY", "F_LISTFREE", "F_GETPOS", "F_L2W", "F_SETPOS", "F_FINDCHILD",
    "F_OCCLUDED", "SV_OCCL_IGNORE", "SV_OCCL_MASK", "SV_CLASSOBJ", "SV_CLS_SENTRY", "SV_CLS_ROOT", "SV_CLS_PLAYERAI", "SV_CLS_WEAPON",
    "SV_QUERY_A", "SV_QUERY_FREE", "SV_FACTION_A", "SV_FACTION_B", "SV_FACTION_C", "SM_ROOT_AI", "PS_GAMEMS", "G_MISSION", "G_MISSION_CM",
    "PS_RAM_HEALTH", "DF_BULLETDROP",
    // the goodie pool (PSYCHO elite drops' rows)
    "PS_RES_REQUEST", "PS_RES_LIST", "PS_RES_LIST2", "PS_RES_DANGLING", "PS_RES_GIVEUP", "PS_PROP_TAKEOVER", "PS_PROP_TIMER", "PS_PROP_TIMER0",
    "PS_SHOW", "PS_SETFLAGS", "PS_POWERUP_COUNT",
    // this feature's own (sites.h section "[drops]")
    "DR_WPN_BULLETS", "DR_COMBO_BLOCK", "DR_GOODIE_ORGPOS"
};
bool loot_install(bool tick_ok) {
    g_installed = false;
    if (!g_c.enabled) return false;
    if (!tick_ok) { gp_log("drops: the input tick is not installed: OFF\n"); return false; }
    for (const char* r : REFS) if (!gp_ref_ok(r)) { gp_log("drops: reference %s differs: OFF\n", r); return false; }
    g_installed = true;
    g_rng.s ^= (uint32_t)GetTickCount() * 2654435761u; if (!g_rng.s) g_rng.s = 1;
    char w[160]; int o = 0;
    for (int i = 0; i < dr::N_POWERUPS && o < (int)sizeof w - 24; ++i) o += _snprintf(w + o, sizeof w - o, "%s%s %d", i ? ", " : "", dr::POWERUP_NAME[i], g_c.weight[i]);
    w[sizeof w - 1] = 0;
    gp_log("drops: on: dropped ammo x%.2f%s, health %.2f per kill (+%.2f x Ram's missing share, %.0f%% of them large), power-up %.2f per kill "
           "(%s)%s%s; pickups stay %.0f s, at most %d at once, lift %.2f m%s\n", g_c.ammo_scale, g_c.keep_difficulty_ammo ? " x the difficulty's factor" : " on every difficulty",
           g_c.health_chance, g_c.health_hurt_bonus, g_c.large_health_share * 100.0f, g_c.powerup_chance, w,
           g_excluded_slot == 10 ? ", no Pinata (Suave has slot 10)" : "", g_ps_drops && g_c.elite_skip_powerup ? ", no power-up from a PSYCHO elite that leaves its move" : "",
           g_c.drop_seconds, g_c.max_on_ground, g_c.lift, g_kill_every > 0 || g_test_key >= 0 || g_probe_key >= 0 || g_walk_goodie ? ", DEBUG HELPERS ON" : "");
    return true;
}

// ---- test host only
static int __cdecl TestInstalled() { return g_installed ? 1 : 0; }
static int __cdecl TestMaxOnGround() { return g_c.max_on_ground; }
static float __cdecl TestAmmo(float f) { return loot_ammo_factor(f); }
void* loot_test_stub(const char* name) {
    if (!strcmp(name, "drops_installed")) return (void*)&TestInstalled;
    if (!strcmp(name, "drops_max_on_ground")) return (void*)&TestMaxOnGround;
    if (!strcmp(name, "drops_ammo")) return (void*)&TestAmmo;
    return nullptr;
}
