// gp_bigfights.cpp - "Bigger Fights" ([bigger_fights] in TODGameplay.ini), game side. Rules as in gp_game.cpp: off unless
// enabled = 1, every referenced address byte-checked first (sites.h BF_* rows and the DF_ / F_ / SV_ rows it shares), every
// game read SEH-guarded, no engine classes. Experimental: it goes with the data option bigger_fights, which breaks saves.
//
// What it does (rules: gp_bigfights_logic.h; data: data/gameplay/bigger_fights.py; docs/mods/bigger_fights.md):
//   * separate saves: with the feature on the game saves to BFSlotNN instead of SlotNN (five call sites of the slot name
//     format), so an old save is never offered to data it does not fit;
//   * bigger pools: the data adds character, weapon and wear shells; the live caps follow on BRUTAL / PSYCHO, always below
//     the shell count (the game's rule: a spare shell must exist);
//   * extra enemies: the mission blocks hold dormant copies of plain gunmen. When a donor is live near Ram its copy gets
//     a free spot near the donor (a line test against the world) and is switched on with the game's SetFlags; from there
//     it is a normal placement: the game's resource controller gives it a shell and the AI runs it.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gp_glue.h"
#include "gp_bigfights_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))

// ================================================================================================ game addresses
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_PLAYER_HOLDER = 0xA3B590;
static const DWORD A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_SCENE = 0xA3DCBC;
static const DWORD C_BULLETDROP = 0x2C0, C_MISSION_NODE = 0x25C, C_TIMESTAMP = 0x274;
static const DWORD N_NEXT = 0x34, N_PARENT = 0x40, N_CHILD = 0x44, N_NAME = 0x4C, N_FLAGS = 0x28;
static const DWORD H_PH_CLS = 0x5A3100;
static const DWORD P_SPRINTF = 0x415300, P_GETPOS = 0x88CA80, P_SETFLAGS = 0x500BE0, P_SENDCMD = 0x86A340,
                   P_GETPROP = 0x86A6A0, P_QUERY = 0x486950, P_LISTFREE = 0x8718D0, P_OCCLUDED = 0x91D020, P_FINDCHILD = 0x88EB60;
static const DWORD PH_MASK = 0x80, CH_MASK = 0x40;
static const int CMD_TRIG = 0x19F, PROP_FACTION = 0x2A8;
static const DWORD M_KILLS = 0x28;                  // mission data +28h EnemiesKilledOnCurrentMission (G_KILLS_R)
static const DWORD PH_RESOURCE = 0x14;              // placeholder data +14h: the pooled root, 0 = none (DF_PH_RES)
static const DWORD RC_MAX = 0x18, RC_WANTS = 0x48, RC_HAS = 0x4C;   // resource sub-controller data (DF_RC_MAX6, diag_resource_ctrl)
static const DWORD H_TP_CLS = 0x655480, TP_NEXT = 0x1C, TP_PREV = 0x20;   // database_traffic_point: class handler (BF_CLS_TRAFFIC), +1Ch NextPointList, +20h PreviousPointList
static const DWORD P_SETWPOS = 0x88E600, V_STOPPED = 0x100, V_RESOURCE = 0x20;   // vehicle_placeholder data: StoppedByAI, the pooled car
static const int CMD_VEH_DRIVE = 413;

typedef void*(__cdecl* Fmt_t)(void* out, const char* fmt, int v);
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);
typedef void(__fastcall* Set1_t)(DWORD node, void* edx, int v);
typedef char(__fastcall* SendCmd_t)(DWORD node, void* edx, int id, void* args);
typedef void(__fastcall* Prop_t)(DWORD node, void* edx, int id, void* v);
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);
typedef void(__fastcall* Find_t)(DWORD parent, void* edx, void* q);
typedef BYTE(__fastcall* Occl_t)(void* self, void* edx, const float* from, const float* to);

// ================================================================================================ state
static bf::Cfg g_c;
static bool g_installed = false, g_tick_ok = false, g_refs_ok = false, g_ray_ok = true;
static int  g_data = -1;                // -1 unknown, 0 no data, 1 the bigger pools are in overdose.main
static int  g_shells = 9;
enum { MAXC = 160 };
enum { DORMANT, AWAKE, GONE };
struct Clone { DWORD node, folder, donor; int tier, serial, state, tries; bool told, woke; double next_t; };
static Clone g_cl[MAXC];
static int  g_ncl = 0, g_mission = -2, g_woken = 0, g_nospot = 0, g_scans = 0;
static bool g_friendly = false;
static double g_next_tick = 0.0, g_next_scan = 0.0, g_next_stat = 0.0, g_tick_ms = 0.0, g_tick_max = 0.0;
static uint32_t g_last_ms = 0;
static int  g_peak_alive = 0, g_peak_clones = 0, g_kills0 = -1; static bool g_first_blood = false;
static double g_fr_last = 0.0, g_fr_sum = 0.0, g_fr_max = 0.0; static int g_fr_n = 0;   // ms per frame, for the status line
static int  g_wx_written = 0, g_wx_base = 0, g_wr_written = 0, g_wr_base = 0;

static float fcfg(const char* k, float d) { return gp_ini().getf("bigger_fights", k, d); }
static int   icfg(const char* k, int d) { return gp_ini().geti("bigger_fights", k, d); }
static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

void bf_load_config() {
    bf::Cfg c;
    c.enabled = icfg("enabled", 0) != 0;
    c.separate_saves = icfg("separate_saves", 1) != 0;
    c.clones = icfg("extra_enemies", 1) != 0;
    c.squads = icfg("heat_vehicles", 1) != 0;
    c.log = icfg("log", 1) != 0;
    c.cap_brutal = clampf(fcfg("cap_scale_brutal", c.cap_brutal), 1.0f, 3.0f);
    c.cap_psycho = clampf(fcfg("cap_scale_psycho", c.cap_psycho), 1.0f, 3.0f);
    c.wake_share_psycho = clampf(fcfg("psycho_wake_share", c.wake_share_psycho), 0.0f, 1.0f);
    c.wake_m = clampf(fcfg("wake_radius", c.wake_m), 20.0f, 200.0f);
    c.join_m = clampf(fcfg("join_radius", c.join_m), 0.0f, 80.0f);
    c.wakes_per_tick = icfg("wakes_per_second", c.wakes_per_tick);
    c.weapon_add = icfg("extra_weapon_props", c.weapon_add);
    c.wear_add = icfg("extra_wear_props", c.wear_add);
    c.force_tier = icfg("force_tier", -1);
    c.squads_in_missions = icfg("heat_vehicles_in_missions", 1) != 0;
    c.squad_every_s = clampf(fcfg("vehicle_every_seconds", c.squad_every_s), 5.0f, 600.0f);
    c.squad_min_m = clampf(fcfg("vehicle_stop_min", c.squad_min_m), 6.0f, 60.0f);
    c.squad_max_m = clampf(fcfg("vehicle_stop_max", c.squad_max_m), c.squad_min_m + 2.0f, 80.0f);
    c.squad_far_m = clampf(fcfg("vehicle_stop_far", c.squad_far_m), c.squad_max_m, 200.0f);
    c.squad_run_m = clampf(fcfg("vehicle_drive_in", c.squad_run_m), 45.0f, 200.0f);
    c.squad_ttl_s = clampf(fcfg("vehicle_seconds", c.squad_ttl_s), 30.0f, 600.0f);
    c.squad_cars_l3 = icfg("vehicles_heat3", c.squad_cars_l3);
    c.squad_cars_l4 = icfg("vehicles_heat4", c.squad_cars_l4);
    c.squad_cars_l5 = icfg("vehicles_heat5", c.squad_cars_l5);
    c.force_heat = icfg("force_heat", 0);
    c.debug_stats = icfg("debug_stats", 0) != 0;
    g_c = c;
    gp_log("bigger fights: %s (separate saves %d, extra enemies %d, heat vehicles %d; live cap x%.2f BRUTAL / x%.2f PSYCHO, "
           "PSYCHO wakes %.0f %% of the extra enemies)\n", c.enabled ? "on" : "off", c.separate_saves, c.clones, c.squads,
           c.cap_brutal, c.cap_psycho, c.wake_share_psycho * 100.0f);
}
bool bf_enabled() { return g_c.enabled; }
bool bf_wants_tick() { return g_c.enabled; }

// ================================================================================================ helpers
struct Cur { DWORD a, b, c; };
static Cur cur_save() { Cur k; k.a = RD32(A_CUR_NODE); k.b = RD32(A_CUR_2); k.c = RD32(A_CUR_3); return k; }
static void cur_restore(const Cur& k) {
    if (RD32(A_CUR_NODE) != k.a) WR32(A_CUR_NODE, k.a);
    if (RD32(A_CUR_2) != k.b) WR32(A_CUR_2, k.b);
    if (RD32(A_CUR_3) != k.c) WR32(A_CUR_3, k.c);
}
static DWORD sdata(DWORD n) { __try { return n ? RD32(n + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_of(DWORD node) {
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static const char* name_of(DWORD n) { __try { const char* s = (const char*)(uintptr_t)RD32(n + N_NAME); return s ? s : ""; } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; } }
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool get_pos(DWORD node, float* p) {
    if (!node) return false;
    float v[3] = {0, 0, 0}; bool ok = true;
    Cur k = cur_save();
    __try { ((Vec_t)(uintptr_t)P_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    if (!ok || !(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    return true;
}
static float dist3(const float* a, const float* b) { float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }
static int mission_now() {
    DWORD cd = gp_cache_data(); if (!cd) return -1;
    __try { DWORD m = RD32(cd + C_MISSION_NODE); DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + 0x38) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int kills_now() {
    DWORD cd = gp_cache_data(); if (!cd) return -1;
    __try { DWORD m = RD32(cd + C_MISSION_NODE); DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + M_KILLS) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static bool game_ms(uint32_t* ms) {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try { DWORD n = RD32(cd + C_TIMESTAMP); DWORD d = n ? RD32(n + 0x18) : 0; if (!d) return false; *ms = RD32(d + 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD find_child(DWORD parent, const char* name) {
    if (!parent) return 0;
    struct { DWORD out; const char* name; } q = {0, name};
    Cur k = cur_save();
    __try { ((Find_t)(uintptr_t)P_FINDCHILD)(parent, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { q.out = 0; }
    cur_restore(k);
    return q.out;
}
static int list_count(DWORD L) { __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; return (L && k) ? (int)(RD32(L + 8) / k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD list_at(DWORD L, int i) {
    __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
            return el ? RD32(el + (DWORD)i * 4 * k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD query(const float* p, float radius, int mask, int mode) {
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, p[0], p[1], p[2], radius, mask, mode};
    DWORD qt = 0; __try { qt = RD32(A_QUADTREE); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!qt) return 0;
    Cur k = cur_save(); bool ok = true;
    __try { ((Query_t)(uintptr_t)P_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok ? q.out : 0;
}
static void query_free(DWORD L) {
    if (!L) return;
    Cur k = cur_save();
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((Free_t)(uintptr_t)P_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}
static DWORD sp_mask(DWORD n) {   // spatial type mask: node +30h -> +1Ch (DF_SPMASK_GET); a dormant node may have none
    __try { DWORD s = RD32(n + 0x30); return s ? RD32(s + 0x1C) : 0xFFFFFFFF; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}
static bool hidden(DWORD n) { __try { DWORD f = RD32(n + N_FLAGS); return ((f | (f >> 16)) & 1) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return true; } }
// the line test of Suave and PSYCHO (Navigator::IsPointOccluded over the scene probe); true = something solid in between
static bool blocked(const float* a, const float* b) {
    if (!g_ray_ok) return true;
    static BYTE self[0x80];
    float f[4] = {a[0], a[1], a[2], 0.0f}, t[4] = {b[0], b[1], b[2], 0.0f};
    BYTE h = 0;
    Cur k = cur_save();
    __try { if (!RD32(A_SCENE) || !RD32(RD32(A_SCENE) + 0xA8)) { cur_restore(k); return true; } h = ((Occl_t)(uintptr_t)P_OCCLUDED)(self, nullptr, f, t); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_ray_ok = false; gp_log("bigger fights: line test faulted: no spot checks, no new extra enemies\n"); return true; }
    cur_restore(k);
    return h != 0;
}

// ================================================================================================ separate saves
static void* __cdecl slot_format_stub(void* out, const char* fmt, int index) {
    return ((Fmt_t)(uintptr_t)P_SPRINTF)(out, bf::slot_format(index, fmt, true), index);
}

// ================================================================================================ pools
static DWORD resource_ctrl(const char* which) {   // MasterCtrlAux/ResourceCtrl/<which> (see gp_difficulty.cpp)
    DWORD cd = gp_cache_data(); if (!cd) return 0;
    DWORD bd = 0, aux = 0; __try { bd = RD32(cd + C_BULLETDROP); aux = bd ? RD32(bd + 0x40) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return find_child(find_child(aux, "ResourceCtrl"), which);
}
static DWORD g_char_ctrl = 0, g_weap_ctrl = 0, g_wear_ctrl = 0, g_veh_ctrl = 0;
static DWORD top_node() {
    DWORD n = g_char_ctrl;
    __try { for (int i = 0; n && i < 64; ++i) { DWORD p = RD32(n + N_PARENT); if (!p) break; n = p; } } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}
static void check_data() {
    if (g_data >= 0) return;
    if (!g_char_ctrl) g_char_ctrl = resource_ctrl("CharacterResourceCtrl");
    if (!g_weap_ctrl) g_weap_ctrl = resource_ctrl("WeaponResourceCtrl");
    if (!g_wear_ctrl) g_wear_ctrl = resource_ctrl("WearResourceCtrl");
    if (!g_veh_ctrl) g_veh_ctrl = resource_ctrl("VehicleResourceCtrl");
    DWORD top = top_node();
    if (!top) return;
    DWORD s = find_child(top, "AICharacter14");
    g_data = s ? 1 : 0;
    g_shells = s ? 14 : 9;
    if (s) gp_log("bigger fights: data found: 14 pooled characters (AICharacter14 = %08X); top node %08X\n", s, top);
    else gp_log("bigger fights: NOT INSTALLED RIGHT: [bigger_fights] enabled = 1 but the data zip has no AICharacter14 (data option bigger_fights = on): no extra enemies, no vehicles; saves still go to BFSlotNN\n");
}
int bf_pool_shells() { return g_data == 1 ? g_shells : 9; }
int bf_live_cap(int level, int game_value) {
    if (!g_c.enabled || g_data != 1) return 0;
    return bf::live_cap(level, game_value, g_shells, g_c);
}
bool bf_cap_wanted() { return g_c.enabled && g_data == 1; }
static void raise_prop_cap(DWORD ctrl, int add, int* written, int* base, bool on, const char* what) {
    DWORD d = sdata(ctrl); if (!d || add <= 0) return;
    __try {
        int cur = (int)RD32(d + RC_MAX);
        if (on) {
            if (cur != *written) *base = cur;
            int t = *base + add;
            if (cur != t) { WR32(d + RC_MAX, t); if (g_c.log) gp_log("bigger fights: %s props handed out %d -> %d\n", what, cur, t); }
            *written = t;
        } else if (*written) {
            if (cur == *written && *base > 0) WR32(d + RC_MAX, *base);
            *written = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================================================ the clones
static DWORD ph_child(DWORD folder) {
    __try { for (DWORD c = RD32(folder + N_CHILD), k = 0; c && k < 64; c = RD32(c + N_NEXT), ++k) if (cls_of(c) == H_PH_CLS) return c; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static DWORD donor_of(DWORD folder, DWORD ph) {   // same parent, same place: a clone stands on its donor until it wakes
    float cp[3], dp[3];
    if (!get_pos(ph, cp)) return 0;
    __try {
        DWORD par = RD32(folder + N_PARENT);
        for (DWORD s = RD32(par + N_CHILD), k = 0; s && k < 2048; s = RD32(s + N_NEXT), ++k) {
            const char* nm = name_of(s);
            if (s == folder || (nm[0] == 'B' && nm[1] == 'F')) continue;
            DWORD p = ph_child(s);
            if (p && get_pos(p, dp) && dist3(cp, dp) < 0.05f) return p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static void squads_seen_reset();
static void add_squad_nodes(DWORD folder, const char* name);
static void add_gunman(DWORD n, const char* name);
static void squads_seen_done(double now);
static const int WALK_BUDGET = 400000;   // scene-graph walks: nodes at most (hang guard, as the other walks)
static void scan() {
    static DWORD stack[32768];
    Clone old[MAXC]; int nold = g_ncl; memcpy(old, g_cl, sizeof(Clone) * (size_t)nold);
    g_ncl = 0; g_friendly = false;
    squads_seen_reset();
    DWORD top = top_node(); if (!top) return;
    int sp = 0, seen = 0;
    double t0 = gp_now();
    stack[sp++] = top;
    __try {
        while (sp) {
            // a loop in the scene graph (a node relinked while the walk runs) would never empty the stack: the game
            // thread would hang. A level has ~17,000 nodes; 400,000 is never a real tree.
            if (seen >= WALK_BUDGET) { gp_log("bigger fights: tree walk stopped after %d nodes (a loop in the scene graph?)\n", seen); break; }
            DWORD n = stack[--sp]; ++seen;
            const char* s = (const char*)(uintptr_t)RD32(n + N_NAME);
            int tier = 0, serial = 0;
            if (s && s[0] == 'B' && s[1] == 'F' && s[2] == 'S') add_squad_nodes(n, s);
            else if (s && s[0] == 'B' && s[1] == 'F' && s[2] == 'G') add_gunman(n, s);
            else if (s && s[0] == 'B' && s[1] == 'F' && bf::parse_clone_folder(s, &tier, &serial) && g_ncl < MAXC) {
                DWORD ph = ph_child(n);
                if (ph) {
                    Clone c{}; c.node = ph; c.folder = n; c.tier = tier; c.serial = serial; c.state = DORMANT;
                    if (tier == bf::TIER_FRIEND) g_friendly = true;
                    for (int i = 0; i < nold; ++i) if (old[i].node == ph) { c = old[i]; break; }
                    if (!c.donor) c.donor = donor_of(n, ph);
                    if (c.state == DORMANT && !((RD32(ph + N_FLAGS)) & 1)) c.state = AWAKE;   // from a save
                    else if (c.state == DORMANT && c.tries == 0 && c.woke) c.state = GONE;
                    g_cl[g_ncl++] = c;
                }
            }
            for (DWORD c = RD32(n + N_CHILD), k = 0; c && k < 16384; c = RD32(c + N_NEXT), ++k) if (sp < 32768) stack[sp++] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("bigger fights: tree walk faulted after %d nodes\n", seen); }
    ++g_scans;
    squads_seen_done(gp_now());
    if (g_c.log && (g_ncl != nold || g_scans == 1)) {
        int d = 0; for (int i = 0; i < g_ncl; ++i) if (g_cl[i].donor) ++d;
        gp_log("bigger fights: mission %d: %d extra placements in the block (%d with their donor found%s), %d nodes walked in %.2f ms\n", g_mission, g_ncl, d, g_friendly ? ", buddy or escort block" : "", seen, (gp_now() - t0) * 1000.0);
    }
}
// is this spot a place to stand: no wall from the donor to it, floor under it, room above, nobody on it
static bool spot_free(const float* from, const float* c) {
    float a[3] = {from[0], from[1] + 1.2f, from[2]}, b[3] = {c[0], c[1] + 1.2f, c[2]};
    if (blocked(a, b)) return false;
    float hi[3] = {c[0], c[1] + 0.9f, c[2]}, lo[3] = {c[0], c[1] - 1.3f, c[2]};
    if (!blocked(hi, lo)) return false;                    // no floor
    float top[3] = {c[0], c[1] + 1.8f, c[2]}, knee[3] = {c[0], c[1] + 0.4f, c[2]};
    if (blocked(top, knee)) return false;                  // in a wall or under something low
    DWORD L = query(c, 1.0f, PH_MASK, 1); int n = list_count(L); query_free(L);
    if (n > 0) return false;
    L = query(c, 1.0f, CH_MASK, 0); n = list_count(L); query_free(L);
    return n == 0;
}
static bool track_spot(DWORD donor, const float* dp, float* out) {   // the end of the donor's own path: authored walkable ground
    __try {
        DWORD folder = RD32(donor + N_PARENT);
        for (DWORD s = RD32(folder + N_CHILD), k = 0; s && k < 32; s = RD32(s + N_NEXT), ++k) {
            const char* nm = name_of(s);
            if (!strstr(nm, "Path")) continue;
            DWORD last = 0;
            for (DWORD p = RD32(s + N_CHILD), j = 0; p && j < 64; p = RD32(p + N_NEXT), ++j) { float q[3]; if (get_pos(p, q) && dist3(q, dp) > 1.5f && dist3(q, dp) < 40.0f) { last = p; memcpy(out, q, 12); } }
            if (last) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static bool wake(Clone& c, const float* dp, const float* rp) {
    float spot[3]; bool found = false; const char* how = "";
    if (track_spot(c.donor, dp, spot) && spot_free(dp, spot)) { found = true; how = "the donor's path"; }
    for (int i = 0; !found && i < 24; ++i) {
        float dx, dz; if (!bf::ring_spot(i, c.serial, &dx, &dz)) break;
        spot[0] = dp[0] + dx; spot[1] = dp[1]; spot[2] = dp[2] + dz;
        if (spot_free(dp, spot)) { found = true; how = "beside the donor"; }
    }
    if (!found) { ++g_nospot; return false; }
    bool ok = true;
    Cur k = cur_save();
    __try {
        float v[3] = {spot[0], spot[1], spot[2]};              // world: a donor may stand on a moving parent (the train of MM120)
        ((Vec_t)(uintptr_t)P_SETWPOS)(c.node, nullptr, v);
        DWORD fl = RD32(c.node + N_FLAGS);
        ((Set1_t)(uintptr_t)P_SETFLAGS)(c.node, nullptr, (int)(fl & 0xFFE));
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    if (ok) { c.state = AWAKE; c.woke = true; ++g_woken; }
    if (g_c.log) gp_log("bigger fights: extra enemy BF%d_%03d %s at (%.1f, %.1f, %.1f), %s, %.1f m from its donor, %.1f m from Ram\n", c.tier, c.serial, ok ? "woke" : "FAILED to wake", spot[0], spot[1], spot[2], how, dist3(spot, dp), dist3(spot, rp));
    return ok;
}
static bool has_shell(DWORD ph) { DWORD d = sdata(ph); __try { return d && RD32(d + PH_RESOURCE) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static void tell_to_fight(Clone& c) {
    DWORD r = ram_root(); if (!r) return;
    struct { DWORD who; DWORD zero; } a = {r, 0};
    Cur k = cur_save();
    __try { ((SendCmd_t)(uintptr_t)P_SENDCMD)(c.node, nullptr, CMD_TRIG, &a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
    c.told = true;
}
static void clone_tick(const float* rp, double now, int level) {
    int budget = g_c.wakes_per_tick, shells = 0;
    for (int i = 0; i < g_ncl; ++i) {
        Clone& c = g_cl[i];
        if (c.state == GONE) continue;
        if (c.state == AWAKE) {
            DWORD m = sp_mask(c.node);
            (void)m;
            if (hidden(c.node)) { c.state = GONE; continue; }                         // killed: the death hides the placement
            if (has_shell(c.node)) {
                ++shells;
                if (!c.told) { float p[3]; if (get_pos(c.node, p) && dist3(p, rp) < g_c.join_m) tell_to_fight(c); }
            }
            continue;
        }
        if (budget <= 0 || now < c.next_t) continue;
        if (!c.donor || !bf::clone_wakes(c.tier, c.serial, level, g_c)) continue;
        DWORD dm = sp_mask(c.donor);
        if (dm == 0xFFFFFFFF || !(dm & PH_MASK) || hidden(c.donor)) continue;        // not in play yet, or dead
        if (cls_of(c.donor) != H_PH_CLS || hidden(c.folder)) continue;
        float dp[3];
        if (!get_pos(c.donor, dp) || dist3(dp, rp) > g_c.wake_m) continue;
        --budget;
        if (!wake(c, dp, rp)) { c.next_t = now + 8.0; if (++c.tries >= 4) c.state = GONE; }
    }
    g_peak_clones = shells > g_peak_clones ? shells : g_peak_clones;
}
static void fight_stats(double now) {
    DWORD d = sdata(g_char_ctrl); if (!d) return;
    int has = 0, wants = 0, mx = 0, hostile = 0;
    __try {
        DWORD H = RD32(d + RC_HAS); has = list_count(H); wants = list_count(RD32(d + RC_WANTS)); mx = (int)RD32(d + RC_MAX);
        Cur k = cur_save();
        for (int i = 0; i < has && i < 32; ++i) {
            DWORD ph = list_at(H, i); int fac = -1;
            if (ph && cls_of(ph) == H_PH_CLS) { ((Prop_t)(uintptr_t)P_GETPROP)(ph, nullptr, PROP_FACTION, &fac); if (fac == 9 || fac == 14 || fac == 28 || fac == 33) ++hostile; }
        }
        cur_restore(k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    int kills = kills_now();
    if (g_kills0 < 0) g_kills0 = kills;
    if (kills != g_kills0 && !g_first_blood) {
        g_first_blood = true;
        gp_log("bigger fights: mission %d: FIGHT START PEAK %d hostile enemies alive at once before the first kill (cap %d, shells %d)\n", g_mission, g_peak_alive, mx, g_shells);
    }
    if (!g_first_blood && hostile > g_peak_alive) g_peak_alive = hostile;
    if (g_c.log && now >= g_next_stat && has > 0) {
        g_next_stat = now + 5.0;
        int awake = 0, inpool = 0;
        for (int i = 0; i < g_ncl; ++i) if (g_cl[i].state == AWAKE) ++awake;
        __try { DWORD H = RD32(d + RC_HAS); for (int i = 0; i < has && i < 32; ++i) { DWORD ph = list_at(H, i); for (int j = 0; j < g_ncl; ++j) if (g_cl[j].node == ph) ++inpool; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        double fr = g_fr_n ? g_fr_sum / g_fr_n : 0.0;
        gp_log("bigger fights: mission %d: %d in the pool (%d hostile, %d of them extra), %d waiting, cap %d; extra enemies awake %d, with a body %d; kills %d; tick %.3f ms avg, %.3f max; frame %.1f ms avg, %.1f max\n", g_mission, has, hostile, inpool, wants, mx, awake, g_peak_clones, kills, g_tick_ms, g_tick_max, fr, g_fr_max);
        g_peak_clones = 0; g_fr_sum = 0.0; g_fr_n = 0; g_fr_max = 0.0;
    }
}

// ================================================================================================ heat squads
// The data holds three dormant cars per block (Folder BFS<i>: vehicle_placeholder BFV<i> with three gunmen BFG<i>_<j> and
// a Track "Vehicle_track" of eight points). The road route comes from the game's traffic graph (database_traffic_point:
// data +1Ch NextPointList, +20h PreviousPointList), the driving, stopping and getting out are the game's own: id 413 on the
// placeholder starts it on the track (without a shell it moves along as a ghost; the vehicle controller gives it a shell
// near the camera), past the last point it brakes and the passengers get out (Stay_In_Vehicle false).
struct Tp { DWORD node; float p[3]; };
enum { MAXTP = 6000 };
static Tp g_tp[MAXTP]; static int g_ntp = 0;
struct Squad { DWORD folder, car, track, pts[8], men[4]; int npts, nmen, state, uses, stops; double t0, next; bool seen, kicked; DWORD told, out, gone; };
enum { Q_IDLE, Q_ARMED, Q_DRIVING, Q_ENDED };
static Squad g_q[3]; static int g_nq = 0;
static double g_next_squad = 0.0;
static int g_started = 0;
static float g_route[8][3];

static void squads_seen_reset() { for (int i = 0; i < 3; ++i) g_q[i].seen = false; }
static void squads_seen_done(double now) {
    g_nq = 0;
    for (int i = 0; i < 3; ++i) {
        Squad& q = g_q[i];
        if (!q.seen) { memset(&q, 0, sizeof q); continue; }
        g_nq = i + 1;
        if (q.state == Q_IDLE && !hidden(q.folder)) { q.state = Q_DRIVING; q.t0 = now; q.next = now + 1.0; }   // from a save
    }
}
static void add_squad_nodes(DWORD folder, const char* name) {
    int i = name[3] - '0';
    if (i < 0 || i > 2 || name[4]) return;
    Squad& q = g_q[i];
    Squad old = q;
    memset(&q, 0, sizeof q);
    q.folder = folder; q.seen = true;
    if (old.folder == folder) { q.state = old.state; q.t0 = old.t0; q.next = old.next; q.uses = old.uses; q.told = old.told; q.out = old.out; q.gone = old.gone; q.stops = old.stops; q.kicked = old.kicked; }
    __try {
        for (DWORD c = RD32(folder + N_CHILD), k = 0; c && k < 16; c = RD32(c + N_NEXT), ++k) {
            const char* nm = name_of(c);
            if (nm[0] == 'B' && nm[1] == 'F' && nm[2] == 'V') q.car = c;
            else if (!strcmp(nm, "Vehicle_track")) {
                q.track = c;
                for (DWORD p = RD32(c + N_CHILD), j = 0; p && j < 16; p = RD32(p + N_NEXT), ++j) {
                    const char* pn = name_of(p);
                    if (pn[0] == 'P' && pn[1] >= '0' && pn[1] <= '7' && !pn[2]) { q.pts[pn[1] - '0'] = p; ++q.npts; }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void add_gunman(DWORD n, const char* name) {   // BFG<i>_<j>
    int i = name[3] - '0';
    if (i < 0 || i > 2 || g_q[i].nmen >= 4 || cls_of(n) != H_PH_CLS) return;
    g_q[i].men[g_q[i].nmen++] = n;
}
// the traffic points of what is loaded now: only when a squad is about to start
static void collect_traffic() {
    static DWORD stack[32768];
    g_ntp = 0;
    DWORD top = top_node(); if (!top) return;
    int sp = 0, seen = 0; stack[sp++] = top;
    __try {
        while (sp) {
            if (++seen > WALK_BUDGET) { gp_log("bigger fights: traffic walk stopped after %d nodes (a loop in the scene graph?)\n", WALK_BUDGET); break; }
            DWORD n = stack[--sp];
            if (cls_of(n) == H_TP_CLS && g_ntp < MAXTP) { Tp& t = g_tp[g_ntp]; if (get_pos(n, t.p)) { t.node = n; ++g_ntp; } }
            for (DWORD c = RD32(n + N_CHILD), k = 0; c && k < 16384; c = RD32(c + N_NEXT), ++k) if (sp < 32768) stack[sp++] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int tp_index(DWORD node) { for (int i = 0; i < g_ntp; ++i) if (g_tp[i].node == node) return i; return -1; }
// a road route that ends near Ram: walk the graph backwards from a point around him
static float g_near_tp = 0.0f; static int g_route_tries = 0, g_route_short = 0, g_route_close = 0;
static bool make_route(const float* rp, uint32_t seed) {
    // the road points around Ram, nearest first: as close as squad_min_m, and when he is off the roads the nearest road there is
    int picks[32], n = 0; float pd[32]; g_near_tp = 1e9f; g_route_tries = g_route_short = g_route_close = 0;
    for (int i = 0; i < g_ntp; ++i) {
        float d = dist3(g_tp[i].p, rp);
        if (d < g_near_tp) g_near_tp = d;
        if (d < g_c.squad_min_m || d > g_c.squad_far_m) continue;
        int at = n < 32 ? n++ : 31;
        if (at == 31 && n == 32 && d >= pd[31]) continue;
        while (at > 0 && pd[at - 1] > d) { pd[at] = pd[at - 1]; picks[at] = picks[at - 1]; --at; }
        pd[at] = d; picks[at] = i;
    }
    int near_n = 0; while (near_n < n && pd[near_n] <= g_c.squad_max_m) ++near_n;
    if (near_n == 0) near_n = n < 6 ? n : 6;
    for (int tries = 0; tries < n && tries < 24; ++tries) {
        ++g_route_tries;
        int cur = picks[tries < near_n ? (seed + (uint32_t)tries) % (uint32_t)near_n : tries];   // the near ones in a random order, then the far ones
        int path[32], np = 0; float len = 0.0f;
        path[np++] = cur;
        for (int dir = 0; dir < 2 && len < g_c.squad_run_m; ++dir) {      // against the traffic's direction first, so the car drives with it
            cur = path[0]; np = 1; len = 0.0f;
            while (np < 32 && len < g_c.squad_run_m) {
                DWORD d = sdata(g_tp[cur].node), L = 0; __try { L = d ? RD32(d + (dir ? TP_NEXT : TP_PREV)) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                int cnt = list_count(L); if (cnt <= 0) break;
                int nx = tp_index(list_at(L, (int)(seed % (uint32_t)cnt)));
                if (nx < 0) break;
                bool again = false; for (int j = 0; j < np; ++j) if (path[j] == nx) again = true;
                if (again) break;
                len += dist3(g_tp[cur].p, g_tp[nx].p);
                path[np++] = nx; cur = nx;
            }
            if (len >= 35.0f) break;
        }
        if (len < 35.0f) { ++g_route_short; continue; }
        if (dist3(g_tp[cur].p, rp) < 30.0f) { ++g_route_close; continue; }
        // eight points evenly along the walk, driving order: far end first
        float seg[32]; float tot = 0.0f;
        for (int j = np - 1; j > 0; --j) { seg[j] = dist3(g_tp[path[j]].p, g_tp[path[j - 1]].p); tot += seg[j]; }
        int j = np - 1; float walked = 0.0f;
        for (int k = 0; k < 8; ++k) {
            float want = tot * (float)k / 7.0f;
            while (j > 1 && walked + seg[j] < want) { walked += seg[j]; --j; }
            float t = seg[j] > 0.01f ? (want - walked) / seg[j] : 0.0f; if (t > 1.0f) t = 1.0f;
            const float* a = g_tp[path[j]].p; const float* b = g_tp[path[j - 1]].p;
            for (int c = 0; c < 3; ++c) g_route[k][c] = a[c] + (b[c] - a[c]) * t;
        }
        return true;
    }
    return false;
}
static void send_cmd(DWORD node, int id, void* args) {
    Cur k = cur_save();
    __try { ((SendCmd_t)(uintptr_t)P_SENDCMD)(node, nullptr, id, args); } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("bigger fights: command %d faulted\n", id); }
    cur_restore(k);
}
static void set_pos(DWORD n, const float* p) {
    float v[3] = {p[0], p[1], p[2]};
    Cur k = cur_save();
    __try { ((Vec_t)(uintptr_t)P_SETWPOS)(n, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}
static void set_flags(DWORD n, DWORD fl) {
    Cur k = cur_save();
    __try { ((Set1_t)(uintptr_t)P_SETFLAGS)(n, nullptr, (int)fl); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}
// a gunman had a body and now has none and his placement is hidden: dead (the spatial bit 80h says nothing here: it is off
// while he has a body, 0x5A37A0 sets it only to ask for one)
static int live_gunmen(Squad& q) {
    int n = 0;
    for (int i = 0; i < q.nmen; ++i) {
        DWORD bit = 1u << i;
        if (has_shell(q.men[i])) q.out |= bit;
        else if ((q.out & bit) && hidden(q.men[i])) q.gone |= bit;
        if (!(q.gone & bit)) ++n;
    }
    return n;
}
// any gunman fighting near him? (the release of a car takes all of its passengers along, on foot too)
static bool gunmen_near(Squad& q, const float* rp) {
    for (int i = 0; i < q.nmen; ++i) { float g[3]; if (!(q.gone & (1u << i)) && has_shell(q.men[i]) && get_pos(q.men[i], g) && dist3(g, rp) < 70.0f) return true; }
    return false;
}
static int cars_busy() {
    DWORD d = sdata(g_veh_ctrl); if (!d) return 4;
    __try { return list_count(RD32(d + RC_HAS)); } __except (EXCEPTION_EXECUTE_HANDLER) { return 4; }
}
static bool start_squad(Squad& q, const float* rp, double now) {
    collect_traffic();
    if (!make_route(rp, (uint32_t)(now * 13.0))) {
        if (g_c.log) gp_log("bigger fights: no road route near Ram (%d traffic points loaded, the nearest %.0f m away; %d tried, %d too short, %d start too close): no vehicle this time\n", g_ntp, g_near_tp, g_route_tries, g_route_short, g_route_close);
        return false;
    }
    if (q.uses > 0) for (int i = 0; i < q.nmen; ++i) df_rearm_placeholder(q.men[i]);     // the same three go out again
    ++q.uses; q.told = 0;
    for (int i = 0; i < 8; ++i) if (q.pts[i]) set_pos(q.pts[i], g_route[i]);
    set_pos(q.car, g_route[0]);
    DWORD d = sdata(q.car);
    __try { if (d) { WR32(d + V_STOPPED, 0); WR32(d + 0xF4, 0); WR32(d + 0xF8, 0); } } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD fl = 0; __try { fl = RD32(q.folder + N_FLAGS); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    set_flags(q.folder, fl & 0xFFE);
    q.state = Q_ARMED; q.t0 = now; q.next = now + 0.9; ++g_started; q.stops = 0; q.out = q.gone = 0; q.kicked = false;
    if (g_c.log) gp_log("bigger fights: heat vehicle %d: route (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f), %.0f m from Ram, %d gunmen\n", (int)(&q - g_q), g_route[0][0], g_route[0][1], g_route[0][2],
                        g_route[7][0], g_route[7][1], g_route[7][2], dist3(g_route[7], rp), q.nmen);
    return true;
}
static void squad_tick(const float* rp, double now) {
    int alive = 0;
    for (int i = 0; i < g_nq; ++i) {
        Squad& q = g_q[i];
        if (!q.folder || q.state == Q_IDLE) continue;
        ++alive;
        if (now < q.next) continue;
        DWORD d = sdata(q.car);
        if (q.state == Q_ARMED) {
            struct { int evil, passive; float dist, radius; } a = {1, 0, 10.0f, 1.0f};
            send_cmd(q.car, CMD_VEH_DRIVE, &a);
            q.state = Q_DRIVING; q.next = now + 1.0;
            if (g_c.log) gp_log("bigger fights: heat vehicle %d sent on its way (shell %08X, car flags %03X, mask %06X)\n", i, d ? RD32(d + V_RESOURCE) : 0, RD32(q.car + N_FLAGS) & 0xFFF, sp_mask(q.car));
        } else if (q.state == Q_DRIVING) {
            int men = live_gunmen(q); float p[3] = {0, 0, 0}; get_pos(q.car, p);
            DWORD shell = 0, stopped = 0; __try { if (d) { shell = RD32(d + V_RESOURCE); stopped = RD32(d + V_STOPPED); } } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (shell && !q.kicked) {          // the takeover hands over the pose only: the car's own AI needs the order again
                struct { int evil, passive; float dist, radius; } a = {1, 0, 10.0f, 1.0f};
                send_cmd(q.car, CMD_VEH_DRIVE, &a);
                q.kicked = true;
                if (g_c.log) gp_log("bigger fights: heat vehicle %d got its body after %.0f s at (%.0f, %.0f, %.0f), %.0f m from Ram: drive order repeated\n", i, now - q.t0, p[0], p[1], p[2], dist3(p, rp));
            }
            if (g_c.log && (int)((now - q.t0)) % 5 == 0) {
                gp_log("bigger fights: heat vehicle %d: %.0f s, %s at (%.0f, %.0f, %.0f), %.0f m from Ram, stopped %d, gunmen left %d\n", i, now - q.t0, shell ? "has its body" : "ghost", p[0], p[1], p[2], dist3(p, rp), (int)stopped, men);
                for (int m = 0; m < q.nmen; ++m) { float g[3] = {0, 0, 0}; get_pos(q.men[m], g); gp_log("bigger fights:   gunman %d: %s, mask %06X, flags %03X, at (%.0f, %.0f, %.0f), %.0f m from Ram\n", m, has_shell(q.men[m]) ? "body" : "no body", sp_mask(q.men[m]), RD32(q.men[m] + N_FLAGS) & 0xFFF, g[0], g[1], g[2], dist3(g, rp)); }
            }
            // out of the car: on Ram at once, like an extra enemy that joins (each gunman once)
            static float last[3][3]; float moved = dist3(p, last[i]); memcpy(last[i], p, 12);
            if (moved < 0.3f) ++q.stops; else q.stops = 0;
            if (q.stops >= 3) for (int m = 0; m < q.nmen; ++m) if (!(q.told & (1u << m)) && has_shell(q.men[m])) {
                DWORD r = ram_root(); struct { DWORD who; DWORD zero; } a = {r, 0};
                if (r) { send_cmd(q.men[m], CMD_TRIG, &a); q.told |= 1u << m; if (g_c.log) gp_log("bigger fights: heat vehicle %d: gunman %d out, told to fight Ram\n", i, m); }
            }
            bool stuck_ghost = !shell && q.stops >= 8 && !q.out;        // parked at the end of its route with no car for it: try again later
            if (men == 0 || stuck_ghost || (now - q.t0 > g_c.squad_ttl_s && !gunmen_near(q, rp))) { q.state = Q_ENDED; q.next = now + 1.0; }
        } else if (q.state == Q_ENDED) {
            DWORD shell = 0; __try { if (d) shell = RD32(d + V_RESOURCE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            float p[3]; if (shell && get_pos(q.car, p) && dist3(p, rp) < 40.0f) { q.next = now + 3.0; continue; }   // nobody takes it away in view
            DWORD fl = 0; __try { fl = RD32(q.folder + N_FLAGS); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            set_flags(q.folder, (fl & 0xFFF) | 1);
            q.state = Q_IDLE; --alive; q.told = 0;
            if (g_c.log) gp_log("bigger fights: heat vehicle %d finished after %.0f s\n", i, now - q.t0);
        }
    }
    int heat = g_c.force_heat;
    if (!heat) { HeatDrive h = heat_drive(); heat = h.level; }
    if (heat < 3 || now < g_next_squad) return;
    if (g_mission < 0) return;
    static double next_why = 0.0;
    bool roam = bf::is_free_roam(g_mission);
    if (!bf::squad_may_start(heat, alive, cars_busy(), !roam && (df_friend_mission() || g_friendly), gp_cutscene(), !roam, g_c)) {
        if (g_c.log && now >= next_why) {
            next_why = now + 20.0;
            gp_log("bigger fights: no vehicle now: heat %d, %d of mine out, %d cars in the game's slots (ctrl %08X), buddies %d/%d, %d vehicle folders in the block\n", heat, alive, cars_busy(), g_veh_ctrl, df_friend_mission() ? 1 : 0, g_friendly ? 1 : 0, g_nq);
        }
        return;
    }
    for (int i = 0; i < g_nq; ++i) {
        Squad& q = g_q[i];
        if (q.folder && q.car && q.state == Q_IDLE && q.npts == 8 && q.nmen > 0) {
            g_next_squad = now + (start_squad(q, rp, now) ? g_c.squad_every_s : 10.0);
            return;
        }
    }
    if (g_c.log && now >= next_why) { next_why = now + 20.0; gp_log("bigger fights: no free vehicle folder (%d in the block, first: car %08X, %d points, %d gunmen)\n", g_nq, g_q[0].car, g_q[0].npts, g_q[0].nmen); }
}

// ================================================================================================ tick
void bf_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed || !g_tick_ok) return;
    if (g_fr_last > 0.0) { double d = (now - g_fr_last) * 1000.0; if (d < 2000.0) { g_fr_sum += d; ++g_fr_n; if (d > g_fr_max) g_fr_max = d; } }
    g_fr_last = now;
    if (now < g_next_tick) return;
    g_next_tick = now + 1.0;
    double t0 = gp_now();
    check_data();
    int m = mission_now();
    uint32_t ms = 0; bool have_ms = game_ms(&ms);
    bool jumped = have_ms && ms + 2000 < g_last_ms;       // a load or a retry
    if (have_ms) g_last_ms = ms;
    if (m != g_mission || jumped) {
        g_mission = m; g_ncl = 0; g_next_scan = 0.0;
        g_peak_alive = 0; g_first_blood = false; g_kills0 = -1;
    }
    if (g_data != 1) { if (g_c.debug_stats) fight_stats(now); return; }     // test copy: the same count on the game's own data
    int level = difficulty_level();
    bool on = level >= bf::BRUTAL || g_c.force_tier > 0;
    raise_prop_cap(g_weap_ctrl, g_c.weapon_add, &g_wx_written, &g_wx_base, on, "weapon");
    raise_prop_cap(g_wear_ctrl, g_c.wear_add, &g_wr_written, &g_wr_base, on, "wear");
    if (now >= g_next_scan) { scan(); g_next_scan = now + (g_ncl ? 30.0 : 8.0); }
    float rp[3]; DWORD r = ram_root();
    if (r && get_pos(r, rp) && !gp_cutscene() && gp_playmode() != 2) {
        if (g_c.clones) clone_tick(rp, now, level);
        if (g_c.squads && on) squad_tick(rp, now);
    }
    fight_stats(now);
    double dt = (gp_now() - t0) * 1000.0;
    g_tick_ms = g_tick_ms * 0.9 + dt * 0.1;
    if (dt > g_tick_max) g_tick_max = dt;
}

// test copy only (debug_crash_log = 1): an access violation in the game's code goes to the log with the stack's code addresses
static LONG CALLBACK crash_log(EXCEPTION_POINTERS* x) {
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    DWORD ip = x->ContextRecord->Eip;
    if (ip < 0x400000 || ip > 0xB00000) return EXCEPTION_CONTINUE_SEARCH;
    static int n = 0; if (++n > 40) return EXCEPTION_CONTINUE_SEARCH;
    gp_log("bigger fights: AV at %08X ecx %08X eax %08X esp %08X\n", ip, x->ContextRecord->Ecx, x->ContextRecord->Eax, x->ContextRecord->Esp);
    __try {
        DWORD* s = (DWORD*)(uintptr_t)x->ContextRecord->Esp; int shown = 0;
        for (int i = 0; i < 400 && shown < 14; ++i) { DWORD v = s[i]; if (v >= 0x401000 && v < 0x9A0000) { gp_log("bigger fights:   stack +%03X: %08X\n", i * 4, v); ++shown; } }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_CONTINUE_SEARCH;
}

// ================================================================================================ install
static const char* REFS[] = {"F_CMD", "F_SETFLAGS", "F_SETPOS", "BF_CLS_TRAFFIC", "BF_VEH_DRIVE", "F_OCCLUDED", "F_FINDCHILD", "SV_CLASSOBJ", "DF_GETPOS", "DF_SENDCMD", "DF_SPMASK_GET", "DF_PH_QUERY", "DF_PH_KILLMASK",
                             "DF_PH_RES", "DF_RC_MAX6", "DF_RC_MAX8", "BF_RC_WEAPON", "BF_RC_WEAR", "BF_SPRINTF", "G_MISSION", "G_MISSION_CM", "G_KILLS_R"};
bool bf_install(bool tick_ok) {
    if (!g_c.enabled) return false;
    if (gp_ini().geti("bigger_fights", "debug_crash_log", 0)) AddVectoredExceptionHandler(1, crash_log);
    bool refs = true;
    for (const char* id : REFS) if (!gp_ref_ok(id)) { gp_log("bigger fights: reference %s differs\n", id); refs = false; }
    g_refs_ok = refs;
    if (!refs) { gp_log("bigger fights: OFF (this is not the exe it was built for)\n"); return false; }
    if (g_c.separate_saves) {
        static const char* SLOTS[] = {"BF_SLOT1", "BF_SLOT2", "BF_SLOT3", "BF_SLOT4", "BF_SLOT5"};
        bool all = true;
        for (const char* s : SLOTS) if (!gp_site_ok(s)) all = false;
        if (!all) { gp_log("bigger fights: OFF: the save slot sites differ\n"); return false; }
        for (const char* s : SLOTS) gp_patch_call(s, (void*)&slot_format_stub);
        gp_log("bigger fights: saves go to BFSlot00..; Slot00.. stay untouched and are not offered while this is on; Slot11 (settings) is shared\n");
    }
    g_tick_ok = tick_ok;
    g_installed = true;
    if (!tick_ok) gp_log("bigger fights: the input tick is off: no extra enemies, no vehicles\n");
    return true;
}

// ================================================================================================ test host
void* bf_test_stub(const char* name) {
    if (!strcmp(name, "bf_installed")) return (void*)&g_installed;
    if (!strcmp(name, "bf_slot_stub")) return (void*)&slot_format_stub;
    return nullptr;
}
