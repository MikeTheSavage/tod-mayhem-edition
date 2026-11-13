// gw_game.cpp - game glue of "gang wars" ([gang_wars] in TODGameplay.ini). Compiled into TODGameplay.dll by
// dll/build.bat. Rules as in dll/src/gp_game.cpp: off unless [gang_wars] enabled = 1, every referenced address
// byte-checked first (sites.h GW_* / SM_* / SV_* / F_* REF rows), every game read SEH-guarded, no engine classes, no code
// patched by this module (it runs in the shared input tick).
//
// What it does (logic and the engine rules it uses: gw_logic.h; research and evidence: gang_wars/README.md):
//   * finds the live enemies around Ram (the game's spatial query), sorts the eligible ones into gangs by model;
//   * a gang member with a rival gang member within range is moved into a spare faction of its own (character_root
//     data +18h, with the game's member-write notify first - the same member the game rewrites in its provoke rule);
//     when no rival is near for calm_seconds it gets the game's faction back;
//   * the spare rows / columns of the game's FactionRelations matrix say who is hostile to whom; the TargetDatabase's
//     per-faction lists are marked stale after every change (its own touch_cache), so the game's vision scan, target
//     choice, aiming, firing, hit reactions and kill counting do the rest;
//   * paused (the menu, where the game saves): every moved member gets the game's faction back and the spare cells
//     their shipped values, so a save holds what the game made; play resumes where it was.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../dll/src/gp_glue.h"
#include "gw_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))

// ================================================================================================ game addresses
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764;
static const DWORD A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_PLAYER_HOLDER = 0xA3B590;   // SV_QUERY_A / SV_QUERY_FREE / SV_CAM_RELEASE
static const DWORD A_LT_ROWS = 0xA3B88C, A_LT_INTS = 0xA3BB04;    // list types: list(list(integer)) / list(integer) (GW_GETREL)
static DWORD P_TOUCH = 0x86B4B0, P_GETPOS = 0x88CA80, P_QUERY = 0x486950, P_LISTFREE = 0x8718D0, P_GETPROP = 0x86A6A0;
static const DWORD H_SENTRY_CLS = 0x5C90F0, H_ROOT_CLS = 0x5C3060;   // SV_CLS_SENTRY / SV_CLS_ROOT
static const DWORD C_MISSION = 0x25C, C_TDB = 0x26C, C_FT = 0x270, C_TIMESTAMP = 0x274;   // cache data (G_MISSION, GW_TDB, GW_KILLROW0, SM_CLOCK)
static const DWORD PH_BASECOUNT = 0x258;                            // [[0xA3B590]+18h]+258h (GW_BASECOUNT)
// character_root data: +14h Sub_Type, +18h FactionID (slot 2), +58h AICtrl (SM_RD_ROOT, SM_RD_FACTION, SM_ROOT_AI)
static const DWORD R_SUBTYPE = 0x14, R_FACTION = 0x18, R_AICTRL = 0x58; static const int SLOT_R_FACTION = 2;
// character_sentry_ai data (SM_SLOT_01/02/03/25, GW_PH, GW_MODEL, GW_KILLOWNER, SM_RD_STATE, SM_RD_COMBAT)
static const DWORD S_SUBTYPE = 0x28, S_HEALTH = 0x2C, S_DAMAGE = 0x30, S_ISBUDDY = 0xB8, S_PH = 0xD0, S_MODEL = 0xD4,
                   S_HITOWNER = 0x14C, S_KILLOWNER = 0x178, S_STATE = 0x1D4, S_COMBAT = 0x234;
static const int   PROP_FACTION = 0x2A8, PROP_ISVIP = 642;          // CharacterPlaceHolder builtins (FactionID 680, IsVIP 642)
static const DWORD TDB_VALID = 0x14, M_KILLS = 0x28;                // TargetDatabase cache_valid (GW_TOUCHCACHE); mission EnemiesKilled (G_KILLS_R)

typedef void(__fastcall* Touch_t)(DWORD node, void* edx, int slot);          // thiscall, ret 4
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);            // thiscall, ret 4
typedef void(__fastcall* Get_t)(DWORD node, void* edx, int id, void* out);   // thiscall, ret 8
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);             // thiscall, ret 4
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);       // thiscall, ret 4
typedef void(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);  // Node::SendCommand by global id, ret 8
static DWORD P_CMD = 0x86A340, P_L2W = 0x88C9C0;                            // F_CMD, F_L2W                                               // F_CMD
static const int CMD_ALARM = 378;                                            // alarm(Entity) (GW_ALARM)

// ================================================================================================ config
static gw::Cfg g_c;
static bool  g_installed = false, g_tick_ok = false, g_log_sw = true, g_log_tg = true, g_log_seen = true, g_wake = true, g_status_members = false;
static int   g_split_a = -1, g_split_b = -1, g_split_n = 0;   // [gang_wars] debug_split = <gang> (test copy)
static bool  g_face = false; static float g_face_sign = 1.0f; static double g_last_face = 0.0;
static float g_period = 0.25f, g_discover_s = 0.5f, g_scan_m = 80.0f, g_status_s = 0.0f;
static char  g_pairs_text[256] = "";
static float fcfg(const char* k, float d) { return gp_ini().getf("gang_wars", k, d); }
static bool  bcfg(const char* k, bool d) { return gp_ini().geti("gang_wars", k, d ? 1 : 0) != 0; }
static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static const char* DEFAULT_PAIRS = "cartel-virgilio, cartel-elvez, cartel-gangster, virgilio-gangster, elvez-gangster, army-mexpolice, dea-outlaw, dea-border, outlaw-border";

void gw_load_config() {
    gw::Cfg c;
    c.enabled = bcfg("enabled", false);
    c.range = clampf(fcfg("range", c.range), 3.0f, 70.0f);
    c.hysteresis = clampf(fcfg("hysteresis", c.hysteresis), 0.0f, 40.0f);
    c.calm_s = clampf(fcfg("calm_seconds", c.calm_s), 0.5f, 120.0f);
    c.idle_s = clampf(fcfg("idle_seconds", c.idle_s), 1.0f, 120.0f);
    c.retry_s = clampf(fcfg("retry_seconds", c.retry_s), 0.0f, 300.0f);
    c.provoke = bcfg("provoke", true);
    c.spark = bcfg("spark", false);
    c.spark_radius = clampf(fcfg("spark_radius", c.spark_radius), 5.0f, 150.0f);
    c.boss_health = clampf(fcfg("boss_health", c.boss_health), 1.0f, 1e6f);
    c.enemy_faction = gp_ini().geti("gang_wars", "enemy_faction", gw::FACTION_ENEMY);
    gw::default_groups(c);
    const gp::Ini& ini = gp_ini();
    for (int i = 0; i < ini.n; ++i) {                       // group_<name> = model numbers (adds or replaces a gang)
        const gp::IniEntry& e = ini.e[i];
        if (_stricmp(e.section, "gang_wars") || _strnicmp(e.key, "group_", 6)) continue;
        if (gw::add_group(c, e.key + 6, e.value) < 0) gp_log("gang wars: ini %s = %s ignored (name up to 15 letters, model numbers)\n", e.key, e.value);
    }
    const char* pairs = ini.get("gang_wars", "factions");
    if (!pairs || !*pairs) pairs = DEFAULT_PAIRS;
    strncpy_s(g_pairs_text, pairs, sizeof g_pairs_text - 1);
    char bad[128];
    gw::parse_pairs(c, pairs, bad, sizeof bad);
    if (bad[0] && c.enabled) gp_log("gang wars: factions: not understood (unknown gang or not a pair): %s\n", bad);
    if (const char* sp = ini.get("gang_wars", "spare_factions")) {
        int v[gw::MAX_SPARES]; int n = gw::parse_ints(sp, v, gw::MAX_SPARES), k = 0;
        for (int i = 0; i < n; ++i) if (v[i] > 0 && v[i] < 19 && v[i] != c.enemy_faction) c.spares[k++] = v[i];
        c.nspares = k;
    }
    g_c = c;
    g_log_sw = bcfg("log_switches", true);
    g_log_tg = bcfg("log_targets", true);
    g_log_seen = bcfg("log_found", true);
    g_wake = bcfg("wake", true);
    g_status_members = bcfg("status_members", false);
    g_face = bcfg("debug_face_war", false);
    g_face_sign = gp_ini().getf("gang_wars", "debug_turn_sign", -1.0f) < 0 ? -1.0f : 1.0f;   // measured: -1 (run p3)
    // debug_split = <gang>: a test-copy demonstration aid - every second member of that gang joins a copy of it
    // ("<gang>_b", no models of its own), hostile to it, so one gang standing in one place fights itself
    g_split_a = g_split_b = -1;
    if (const char* sg = gp_ini().get("gang_wars", "debug_split")) {
        int a = gw::find_group(g_c, sg);
        char nb[gw::NAME_LEN]; sprintf_s(nb, "%.13s_b", sg);
        if (a >= 0 && g_c.ngroups < gw::MAX_GROUPS) {
            int b = g_c.ngroups++;
            memset(&g_c.groups[b], 0, sizeof g_c.groups[b]);
            strncpy_s(g_c.groups[b].name, nb, _TRUNCATE);
            g_c.hostile[a][b] = g_c.hostile[b][a] = true;
            g_split_a = a; g_split_b = b;
            gp_log("gang wars: debug_split: every second %s member joins %s (hostile) - a demonstration aid, not the game's own gangs\n", sg, nb);
        }
    }
    g_period = clampf(fcfg("tick_seconds", 0.25f), 0.05f, 2.0f);
    g_discover_s = clampf(fcfg("discover_seconds", 0.5f), 0.1f, 10.0f);
    g_scan_m = clampf(fcfg("scan_radius", 80.0f), 20.0f, 200.0f);
    g_status_s = clampf(fcfg("status_seconds", 0.0f), 0.0f, 600.0f);
}
bool gw_enabled() { return g_c.enabled; }
bool gw_wants_tick() { return g_c.enabled; }

// ================================================================================================ memory helpers
static DWORD sdata(DWORD node) { __try { return node ? RD32(node + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_of(DWORD node) {   // node +4 type -> +28h script class object -> +60h handler (SV_CLASSOBJ)
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
struct Cur { DWORD a, b, c; };   // the script globals the engine calls below may change (restored as smart_game.cpp does)
static Cur cur_save() { Cur k; k.a = RD32(A_CUR_NODE); k.b = RD32(A_CUR_2); k.c = RD32(A_CUR_3); return k; }
static void cur_restore(const Cur& k) { if (RD32(A_CUR_NODE) != k.a) WR32(A_CUR_NODE, k.a); if (RD32(A_CUR_2) != k.b) WR32(A_CUR_2, k.b); if (RD32(A_CUR_3) != k.c) WR32(A_CUR_3, k.c); }
static void touch(DWORD node, int slot) { __try { ((Touch_t)(uintptr_t)P_TOUCH)(node, nullptr, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static bool get_pos(DWORD node, float* p) {
    float v[3] = {0, 0, 0}; bool ok = true;
    __try { ((Vec_t)(uintptr_t)P_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok || !(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    return true;
}
static bool get_prop(DWORD node, int id, int* out) {
    int v = 0; bool ok = true;
    __try { ((Get_t)(uintptr_t)P_GETPROP)(node, nullptr, id, &v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (ok) *out = v;
    return ok;
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int base_count() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? (int)RD32(d + PH_BASECOUNT) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static bool game_ms(uint32_t* ms) {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try { DWORD n = RD32(cd + C_TIMESTAMP); DWORD d = n ? RD32(n + 0x18) : 0; if (!d) return false; *ms = RD32(d + 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int mission_now() {
    DWORD cd = gp_cache_data(); if (!cd) return -1;
    __try { DWORD m = RD32(cd + C_MISSION); DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + 0x38) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int mission_kills() {
    DWORD cd = gp_cache_data(); if (!cd) return -1;
    __try { DWORD m = RD32(cd + C_MISSION); DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + M_KILLS) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static void invalidate_targets() {   // the game's own touch_cache: every faction's hostile target list is rebuilt
    DWORD cd = gp_cache_data(); if (!cd) return;
    __try { DWORD t = RD32(cd + C_TDB); DWORD td = t ? RD32(t + 0x18) : 0; if (td) WR32(td + TDB_VALID, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================================================ the relation matrix
enum { MAXF = 64 };
struct Mat { DWORD list, rows; int n, so, si; };
static int g_van[MAXF][MAXF]; static DWORD g_van_list = 0; static bool g_van_ok = false;
static gw::Design g_d;
static DWORD g_cells_written = 0, g_matrix_writes = 0;
static bool mat_open(Mat* m) {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try {
        DWORD ft = RD32(cd + C_FT); DWORD fd = ft ? RD32(ft + 0x18) : 0; if (!fd) return false;
        m->list = RD32(fd + 4); if (!m->list) return false;
        m->so = (int)RD32(RD32(A_LT_ROWS) + 0x24); m->si = (int)RD32(RD32(A_LT_INTS) + 0x24);
        if (m->so <= 0 || m->so > 8 || m->si <= 0 || m->si > 8) return false;
        m->n = (int)(RD32(m->list + 8) / (DWORD)m->so);
        DWORD a = RD32(m->list + 4); m->rows = a ? RD32(a + 4) : 0;
        if (!m->rows || m->n < 19 || m->n > MAXF) return false;
        for (int r = 0; r < m->n; ++r) {
            DWORD row = RD32(m->rows + (DWORD)(r * m->so * 4));
            if (!row || (int)(RD32(row + 8) / (DWORD)m->si) < m->n) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static volatile DWORD* cellp(const Mat& m, int r, int c) {
    DWORD row = RD32(m.rows + (DWORD)(r * m.so * 4));
    DWORD e = RD32(RD32(row + 4) + 4);
    return (volatile DWORD*)(uintptr_t)(e + (DWORD)(c * m.si * 4));
}
static int cell(const Mat& m, int r, int c) { __try { return (int)*cellp(m, r, c); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static bool spare_id(int f) { return gw::is_spare(g_c, g_d, f); }
static int vanilla_fn(void* ctx, int r, int c) {
    const Mat& m = *(const Mat*)ctx;
    if (spare_id(r) || spare_id(c)) return g_van[r][c];
    return cell(m, r, c);
}
static void capture(const Mat& m) {   // the game's values, before we write anything (a new list = a new table)
    for (int r = 0; r < m.n; ++r) for (int c = 0; c < m.n; ++c) g_van[r][c] = cell(m, r, c);
    g_van_list = m.list; g_van_ok = true;
    int base = g_d.base, cnt = 0;
    for (int i = 0; i < g_c.nspares; ++i) for (int c = 0; c < m.n; ++c) if (g_van[g_c.spares[i]][c] != g_van[g_c.spares[i] % base][c % base]) ++cnt;
    gp_log("gang wars: relation matrix %d x %d captured (base %d; Ram row: enemy %d police %d; enemy row: police %d friend %d; spare rows %s)\n",
           m.n, m.n, base, g_van[0][g_c.enemy_faction], g_van[0][14 < m.n ? 14 : 0], g_van[g_c.enemy_faction][14 < m.n ? 14 : 0],
           g_van[g_c.enemy_faction][10 < m.n ? 10 : 0], cnt ? "differ from their base rows" : "as shipped");
}
// write the spare rows / columns: what the design asks (restore = the captured values); returns cells written
static int apply_matrix(bool restore) {
    Mat m; if (!mat_open(&m)) return -1;
    if (!g_van_ok || m.list != g_van_list) capture(m);
    int w = 0;
    __try {
        for (int r = 0; r < m.n; ++r) {
            bool rs = spare_id(r);
            for (int c = 0; c < m.n; ++c) {
                if (!rs && !spare_id(c)) continue;
                int want = restore ? g_van[r][c] : gw::desired_cell(g_c, g_d, r, c, vanilla_fn, &m);
                if (want < 0) continue;
                volatile DWORD* p = cellp(m, r, c);
                if ((int)*p != want) { *p = (DWORD)want; ++w; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("gang wars: matrix write exception\n"); }
    if (w) { g_cells_written += w; ++g_matrix_writes; invalidate_targets(); }
    return w;
}

// ================================================================================================ the registry
enum { MAXE = 40 };
struct Ent {
    bool  used, static_ok, was_alive, dead_logged;
    DWORD root, rootd, ai, aid, ph;
    int   model, sub, rsub, vip;
    float hp, last_dmg;
    DWORD last_target; double last_target_log;
    const char* why_not;
    gw::Member m;
};
static Ent g_e[MAXE];
static int g_serial = 0, g_mission = -2;
static double g_cost_sum = 0.0, g_cost_max = 0.0; static unsigned g_cost_n = 0;
static uint32_t g_last_gms = 0; static double g_last_real = 0.0;
static double g_last_tick = 0.0, g_last_discover = -100.0, g_last_status = 0.0;
static gw::Spark g_sp;
static DWORD g_switches = 0, g_found = 0, g_rival_kills = 0, g_repairs = 0;
static bool g_suspended = false;

static const char* gname(int g) { return g >= 0 && g < g_c.ngroups ? g_c.groups[g].name : "-"; }
static Ent* find_root(DWORD root) { for (Ent& e : g_e) if (e.used && e.root == root) return &e; return nullptr; }
static bool known(DWORD root, DWORD rootd, DWORD ai) { for (Ent& e : g_e) if (e.used && e.root == root && e.rootd == rootd && e.ai == ai) return true; return false; }
static void write_faction(Ent& e, int f, const char* why) {
    int before = -1;
    __try { before = (int)RD32(e.rootd + R_FACTION); touch(e.root, SLOT_R_FACTION); WR32(e.rootd + R_FACTION, (DWORD)f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("gang wars: faction write %08X failed\n", e.root); return; }
    e.m.faction = f; ++g_switches;
    if (g_log_sw) gp_log("gang wars: %08X (%s) faction %d -> %d: %s\n", e.root, gname(e.m.group), before, f, why);
}
static bool our_faction(int f) { return spare_id(f); }

static const char* eligibility(const Ent& e) {
    if (e.m.orig != g_c.enemy_faction) return "not an enemy faction";
    if (e.m.group < 0) return "no gang for its model";
    if (e.sub == 4 || e.sub == 7 || e.sub == 8 || e.sub == 9) return "special sub type (escort / player / never aggressive / civilian)";
    if (e.rsub == 7 || e.rsub == 8 || e.rsub == 9) return "special root sub type";
    if (e.hp >= g_c.boss_health) return "boss / special (health)";
    if (e.vip) return "VIP placement";
    return nullptr;
}
static bool add_ent(DWORD root, DWORD rootd, DWORD ai, DWORD aid) {
    Ent* e = nullptr;
    for (Ent& x : g_e) if (!x.used) { e = &x; break; }
    if (!e) return false;
    memset(e, 0, sizeof *e);
    e->used = true; e->root = root; e->rootd = rootd; e->ai = ai; e->aid = aid;
    int buddy = 0, rf = -1;
    __try {
        e->ph = RD32(aid + S_PH); e->model = (int)RD32(aid + S_MODEL); e->sub = (int)RD32(aid + S_SUBTYPE);
        e->hp = RDF(aid + S_HEALTH); e->last_dmg = RDF(aid + S_DAMAGE); buddy = (int)RD32(aid + S_ISBUDDY);
        e->rsub = (int)RD32(rootd + R_SUBTYPE); rf = (int)RD32(rootd + R_FACTION);
    } __except (EXCEPTION_EXECUTE_HANDLER) { e->used = false; return false; }
    int pf = -1, vip = 0;
    if (e->ph) { if (!get_prop(e->ph, PROP_FACTION, &pf)) pf = -1; if (!get_prop(e->ph, PROP_ISVIP, &vip)) vip = 0; }
    e->vip = vip != 0;
    e->m.id = ++g_serial; e->m.group = gw::group_of_model(g_c, e->model);
    if (g_split_a >= 0 && e->m.group == g_split_a && (g_split_n++ & 1)) e->m.group = g_split_b;   // debug_split: every 2nd one
    e->m.orig = pf >= 0 ? pf : rf; e->m.faction = rf; e->m.state = gw::ST_CALM;
    e->why_not = buddy ? "buddy" : eligibility(*e);
    e->static_ok = e->why_not == nullptr;
    // a root that still holds one of our spare factions (a save from an older build, a retry from the rewind buffer)
    if (rf != e->m.orig && our_faction(rf) && e->m.orig == g_c.enemy_faction) { write_faction(*e, e->m.orig, "repaired (held a gang-war faction at first sight)"); ++g_repairs; }
    ++g_found;
    if (g_log_seen) gp_log("gang wars: found %08X model %d gang %s faction %d (placement %d) hp %.0f sub %d/%d%s%s\n", root, e->model, gname(e->m.group),
                           rf, pf, e->hp, e->sub, e->rsub, e->static_ok ? "" : ": left alone, ", e->static_ok ? "" : e->why_not);
    return true;
}
static void discover() {
    DWORD ram = ram_root(); float rp[3];
    if (!ram || !get_pos(ram, rp)) return;
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp[0], rp[1], rp[2], g_scan_m, 0x40, 0};
    DWORD qt = 0; __try { qt = RD32(A_QUADTREE); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!qt) return;
    __try { ((Query_t)(uintptr_t)P_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    DWORD L = q.out;
    if (!L) return;
    __try {
        DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0;
        DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
        int n = k ? (int)(RD32(L + 8) / k) : 0;
        for (int i = 0; i < n && i < 256 && el; ++i) {
            DWORD r = RD32(el + (DWORD)i * 4 * k);
            if (!r || r == ram || cls_of(r) != H_ROOT_CLS) continue;
            DWORD rd = sdata(r); DWORD ai = rd ? RD32(rd + R_AICTRL) : 0;
            if (!ai || cls_of(ai) != H_SENTRY_CLS) continue;
            DWORD d = sdata(ai);
            if (!d || known(r, rd, ai)) continue;
            int st = (int)RD32(d + S_STATE);
            if (st == -1) continue;                                   // in the pool
            if (Ent* old = find_root(r)) old->used = false;          // the pool reused this root for another placement
            add_ent(r, rd, ai, d);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((Free_t)(uintptr_t)P_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================================================ tick
static void forget_all(const char* why) {
    int n = 0; for (Ent& e : g_e) if (e.used) ++n;
    if (n) gp_log("gang wars: %s: %d characters forgotten (nothing written)\n", why, n);
    memset(g_e, 0, sizeof g_e);
    g_d = gw::Design(); g_sp = gw::Spark();
}
static void release_all() {   // paused: the game's faction on every moved member, the shipped spare cells
    int n = 0;
    for (Ent& e : g_e) {
        if (!e.used || !e.m.alive) continue;
        if (e.m.faction != e.m.orig && our_faction(e.m.faction)) {
            __try { touch(e.root, SLOT_R_FACTION); WR32(e.rootd + R_FACTION, (DWORD)e.m.orig); ++n; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    int w = apply_matrix(true);
    invalidate_targets();
    if (n || w > 0) gp_log("gang wars: paused: %d members back to their own faction, %d matrix cells shipped values (a save holds the game's own)\n", n, w);
}
static void resume_all() {    // after the pause: the members at war / provoked get their faction again
    int n = 0;
    for (Ent& e : g_e) {
        if (!e.used || !e.m.alive || e.m.group < 0) continue;
        int f = e.m.state == gw::ST_WAR ? g_d.alloc[e.m.group] : e.m.state == gw::ST_PROVOKED && g_d.alloc[e.m.group] >= 0 ? g_d.alloc[e.m.group] + g_d.base : -1;
        if (f < 0) continue;
        __try { if ((int)RD32(e.rootd + R_FACTION) == e.m.orig) { touch(e.root, SLOT_R_FACTION); WR32(e.rootd + R_FACTION, (DWORD)f); e.m.faction = f; ++n; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    apply_matrix(false);
    invalidate_targets();
    if (n) gp_log("gang wars: resumed: %d members back at war\n", n);
}
static void describe_target(DWORD t, DWORD ram, char* out, int cap, const float* from) {
    if (!t) { strncpy_s(out, cap, "nobody", _TRUNCATE); return; }
    if (t == ram) { float p[3]; if (from && get_pos(ram, p)) sprintf_s(out, cap, "RAM %.1f m", gw::dist(from, p)); else strncpy_s(out, cap, "RAM", _TRUNCATE); return; }
    if (Ent* o = find_root(t)) { sprintf_s(out, cap, "%08X (%s, faction %d) %.1f m", t, gname(o->m.group), o->m.faction, from && o->m.has_pos ? gw::dist(from, o->m.pos) : -1.0f); return; }
    sprintf_s(out, cap, "%08X (other)", t);
}
static void tick_body(float gdt) {
    DWORD ram = ram_root();
    gw::Member mem[MAXE]; int map[MAXE]; DWORD tg[MAXE]; int n = 0;
    // ---- read
    for (int i = 0; i < MAXE; ++i) {
        Ent& e = g_e[i];
        if (!e.used) continue;
        if (cls_of(e.ai) != H_SENTRY_CLS || sdata(e.ai) != e.aid || sdata(e.root) != e.rootd) { e.used = false; continue; }
        int st = -1; float hp = 0, dmg = 0; DWORD ph = 0, owner = 0, tgt = 0; int fac = -1;
        __try {
            st = (int)RD32(e.aid + S_STATE); hp = RDF(e.aid + S_HEALTH); dmg = RDF(e.aid + S_DAMAGE); ph = RD32(e.aid + S_PH);
            owner = RD32(e.aid + S_HITOWNER); fac = (int)RD32(e.rootd + R_FACTION);
            DWORD cc = RD32(e.aid + S_COMBAT); DWORD cd = cc ? RD32(cc + 0x18) : 0; if (cd) tgt = RD32(cd + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) { e.used = false; continue; }
        if (st == -1 || ph != e.ph) { e.used = false; continue; }    // back in the pool / reused
        bool alive = st != 28 && hp - dmg > 0.0f;
        e.m.alive = alive; e.m.faction = fac;
        e.m.has_pos = get_pos(e.root, e.m.pos) || e.m.has_pos;
        e.m.eligible = e.static_ok && st != 2 && st != 3 && st != 12 && st != 31;   // no AI control / mounted gun / priority target / buddy
        e.m.ram_hit = alive && dmg > e.last_dmg + 0.01f && owner && owner == ram;
        e.last_dmg = dmg;
        if (e.was_alive && !alive && !e.dead_logged) {                // a death: who killed it, does the mission count it
            e.dead_logged = true;
            DWORD ko = 0; __try { ko = RD32(e.aid + S_KILLOWNER); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            Ent* k = ko ? find_root(ko) : nullptr;
            const char* who = !ko ? "nobody (scripted)" : ko == ram ? "RAM" : k ? gname(k->m.group) : "another character";
            bool rival = k && k->m.group >= 0 && e.m.group >= 0 && g_c.hostile[k->m.group][e.m.group];
            if (rival) ++g_rival_kills;
            gp_log("gang wars: %08X (%s, faction %d) killed by %s%s%08X; mission EnemiesKilled now %d\n", e.root, gname(e.m.group), fac, who,
                   rival ? " (rival gang) " : " ", ko, mission_kills());
        }
        e.was_alive = alive;
        if (g_log_tg && alive && e.m.group >= 0 && tgt != e.last_target) {
            char a[96], b[96];
            describe_target(e.last_target, ram, a, sizeof a, e.m.has_pos ? e.m.pos : nullptr);
            describe_target(tgt, ram, b, sizeof b, e.m.has_pos ? e.m.pos : nullptr);
            gp_log("gang wars: target switch %08X (%s, %s, faction %d, state %d): %s -> %s\n", e.root, gname(e.m.group), gw::state_name(e.m.state), fac, st, a, b);
            e.last_target = tgt;
        }
        mem[n] = e.m; map[n] = i; tg[n] = alive ? tgt : 0; ++n;
    }
    for (int k = 0; k < n; ++k) {                                     // is its own target a rival gang member?
        mem[k].target_rival = false;
        if (!tg[k] || mem[k].group < 0) continue;
        for (int j = 0; j < n; ++j)
            if (g_e[map[j]].root == tg[k] && mem[j].group >= 0 && g_c.hostile[mem[k].group][mem[j].group]) { mem[k].target_rival = true; break; }
    }
    // ---- decide
    gw::Out out[MAXE];
    int b = base_count(); if (b >= 19 && b < 40) g_d.base = b;
    int alloc_before[gw::MAX_GROUPS]; memcpy(alloc_before, g_d.alloc, sizeof alloc_before);
    gw::step(g_c, g_d, mem, n, g_sp, gdt, out);
    for (int g = 0; g < gw::MAX_GROUPS; ++g)
        if (g_d.alloc[g] != alloc_before[g])
            gp_log("gang wars: gang %s %s faction %d\n", gname(g), g_d.alloc[g] >= 0 ? "takes spare" : "gives back spare", g_d.alloc[g] >= 0 ? g_d.alloc[g] : alloc_before[g]);
    // ---- write: the matrix first (so a moved member never holds a faction without its relations), then the members
    apply_matrix(false);
    bool moved = false;
    for (int k = 0; k < n; ++k) {
        Ent& e = g_e[map[k]];
        e.m = mem[k];
        if (out[k].changed && out[k].want_faction < 0 && g_log_sw)
            gp_log("gang wars: %08X (%s) is %s: %s\n", e.root, gname(e.m.group), gw::state_name(e.m.state), out[k].why);
        if (out[k].want_faction >= 0) {
            char why[160];
            if (out[k].rival >= 0) sprintf_s(why, "%s (%s): rival %s %08X at %.1f m", gw::state_name(e.m.state), out[k].why, gname(mem[out[k].rival].group), g_e[map[out[k].rival]].root, out[k].rival_d);
            else sprintf_s(why, "%s (%s)", gw::state_name(e.m.state), out[k].why);
            write_faction(e, out[k].want_faction, why);
            moved = true;
            // a member still idling (passive states 4..8: guard, patrol, walkaround) gets the game's own alarm, as a hit
            // civilian alarms its friends: the passive state's handler sets Alarm_pos and goes aggressive (state 13),
            // whose vision then picks the rival (the gang's new relations)
            int st = -1; __try { st = (int)RD32(e.aid + S_STATE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (g_wake && (e.m.state == gw::ST_WAR || e.m.state == gw::ST_PROVOKED) && st >= 4 && st <= 8 && out[k].rival >= 0) {
                DWORD rv = g_e[map[out[k].rival]].ai; bool ok = true;
                __try { ((Cmd_t)(uintptr_t)P_CMD)(e.ai, nullptr, CMD_ALARM, &rv); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                int st2 = -1; __try { st2 = (int)RD32(e.aid + S_STATE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                if (g_log_sw) gp_log("gang wars: %08X (%s) was idle (state %d): the game's alarm sent%s, state now %d\n", e.root, gname(e.m.group), st, ok ? "" : " (FAILED)", st2);
            }
        }
    }
    if (moved) invalidate_targets();
}
// [gang_wars] debug_face_war (test copy, for the proof clips): Ram turns toward the fight with the game's own yaw path
// (the mouse-turn accumulator, [controls] mouse_turn_fix), at most 30 degrees per second
static void face_war(double now) {
    if (now - g_last_face < 0.5) return;
    g_last_face = now;
    float c[3] = {0, 0, 0}; int k = 0;
    for (Ent& e : g_e) if (e.used && e.m.alive && e.m.has_pos && (e.m.state == gw::ST_WAR || e.m.state == gw::ST_PROVOKED)) { c[0] += e.m.pos[0]; c[2] += e.m.pos[2]; ++k; }
    if (k < 2) return;
    c[0] /= k; c[2] /= k;
    DWORD ram = ram_root(); float rp[3]; if (!ram || !get_pos(ram, rp)) return;
    float b[6] = {0, 0, 0, 0, 0, 1};
    __try { ((Vec_t)(uintptr_t)P_L2W)(ram, nullptr, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    float fx = b[0], fz = b[2], dx = c[0] - rp[0], dz = c[2] - rp[2];
    float fl = sqrtf(fx * fx + fz * fz), dl = sqrtf(dx * dx + dz * dz);
    if (fl < 1e-4f || dl < 2.0f) return;
    float a = atan2f(fx * dz - fz * dx, fx * dx + fz * dz) * 57.29578f;   // signed angle facing -> fight
    if (fabsf(a) < 8.0f) return;
    float step = a > 15.0f ? 15.0f : (a < -15.0f ? -15.0f : a);
    bool ok = gp_yaw_add(step * g_face_sign);
    static int nlog = 0; if (nlog++ < 40) gp_log("gang wars: debug_face_war: fight %.1f m away, %.0f deg off: turn %.0f%s\n", dl, a, step * g_face_sign, ok ? "" : " (no mouse-turn path)");
}
void gw_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed || !g_tick_ok) return;
    if (now - g_last_tick < g_period && now >= g_last_tick) return;
    g_last_tick = now;
    int pm = gp_playmode();
    if (pm == 2) {
        if (!g_suspended) { Cur k = cur_save(); __try { release_all(); } __except (EXCEPTION_EXECUTE_HANDLER) {} cur_restore(k); g_suspended = true; }
        return;
    }
    if (pm == 3) return;                            // rewinding: the game restores its own records
    uint32_t gms;
    if (!game_ms(&gms)) return;
    Cur k = cur_save();
    int mission = mission_now();
    if (mission != g_mission || (g_last_gms && gms + 2000 < g_last_gms)) {
        if (g_mission != -2) forget_all(mission != g_mission ? "mission changed" : "game clock went back");
        g_mission = mission; g_last_gms = 0; g_last_real = 0.0; g_suspended = false;
        __try { apply_matrix(false); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // the spare cells as shipped (nobody allocated)
    }
    if (g_suspended) { g_suspended = false; __try { resume_all(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    // the war / calm timers run on real seconds while the game plays (pause and rewind return above); the game-ms node
    // advanced far slower than real time in the MM075 / MM120 test runs (8 s of calm took 60+ s)
    float gdt = (g_last_real > 0.0 && now >= g_last_real) ? (float)(now - g_last_real) : 0.0f;
    if (gdt > 1.0f) gdt = 1.0f;
    g_last_real = now; g_last_gms = gms;
    double t_in = gp_now();
    if (now - g_last_discover >= g_discover_s || now < g_last_discover) { g_last_discover = now; __try { discover(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    __try { tick_body(gdt); } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("gang wars: tick exception: registry cleared\n"); memset(g_e, 0, sizeof g_e); }
    if (g_face) face_war(now);
    { double ms = (gp_now() - t_in) * 1000.0; g_cost_sum += ms; ++g_cost_n; if (ms > g_cost_max) g_cost_max = ms; }
    if (g_status_s > 0.0f && now - g_last_status >= g_status_s) {
        g_last_status = now;
        int cnt[4] = {0, 0, 0, 0}, alive = 0;
        for (Ent& e : g_e) if (e.used && e.m.alive) { ++alive; if (e.m.group >= 0) ++cnt[e.m.state & 3]; }
        gp_log("gang wars: status: %d alive tracked, gang members calm %d war %d provoked %d released %d; switches %u, rival kills %u, matrix cells written %u\n",
               alive, cnt[0], cnt[1], cnt[2], cnt[3], g_switches, g_rival_kills, g_cells_written);
        if (g_cost_n) gp_log("gang wars: cost: %u ticks (every %.2f s), %.3f ms average, %.3f ms max per tick\n", g_cost_n, g_period, g_cost_sum / g_cost_n, g_cost_max);
        g_cost_sum = g_cost_max = 0.0; g_cost_n = 0;
        if (g_status_members) {
            DWORD ram = ram_root(); float rp[3]; bool hr = ram && get_pos(ram, rp);
            for (Ent& e : g_e) {
                if (!e.used || !e.m.alive) continue;
                int st = -1; __try { st = (int)RD32(e.aid + S_STATE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                char t[96]; describe_target(e.last_target, ram, t, sizeof t, e.m.has_pos ? e.m.pos : nullptr);
                gp_log("gang wars:   %08X %-9s %-8s faction %2d state %2d %s, Ram %.1f m, target %s\n", e.root, gname(e.m.group), gw::state_name(e.m.state),
                       e.m.faction, st, e.static_ok ? "eligible" : e.why_not, hr && e.m.has_pos ? gw::dist(rp, e.m.pos) : -1.0f, t);
            }
        }
    }
    cur_restore(k);
}

// ================================================================================================ install
static const char* const GW_REFS[] = {
    "GW_GETREL", "GW_BASECOUNT", "GW_TOUCHCACHE", "GW_TDB", "GW_TDB_QUERY", "GW_HIT_REL", "GW_KILLROW0", "GW_MODEL",
    "GW_MODELIDX", "GW_PH", "GW_KILLOWNER", "G_KILLS_R", "G_MISSION", "SM_CLOCK", "F_TOUCH", "F_GETPOS", "F_GETPROP",
    "F_QUERY", "F_LISTFREE", "SV_QUERY_A", "SV_QUERY_FREE", "SV_CLASSOBJ", "SV_CLS_SENTRY", "SV_CLS_ROOT", "SV_CAM_RELEASE",
    "SM_ROOT_AI", "SM_RD_ROOT", "SM_RD_FACTION", "SM_RD_STATE", "SM_RD_COMBAT", "SM_SLOT_01", "SM_SLOT_02", "SM_SLOT_03",
    "SM_SLOT_25", "GW_ALARM", "F_CMD", "F_L2W",
};
bool gw_install(bool tick_ok) {
    g_installed = false;
    if (!gw_enabled()) return false;
    for (const char* r : GW_REFS) if (!gp_ref_ok(r)) { gp_log("gang wars: reference %s differs: OFF\n", r); return false; }
    if (!tick_ok) { gp_log("gang wars: the input tick is not installed: OFF\n"); return false; }
    if (!g_c.nspares) { gp_log("gang wars: no spare faction: OFF\n"); return false; }
    g_tick_ok = true; g_installed = true;
    char sp[64] = ""; for (int i = 0; i < g_c.nspares; ++i) { char t[8]; sprintf_s(t, "%s%d", i ? "," : "", g_c.spares[i]); strcat_s(sp, t); }
    int pairs = 0; for (int a = 0; a < g_c.ngroups; ++a) for (int b = a + 1; b < g_c.ngroups; ++b) if (g_c.hostile[a][b]) ++pairs;
    gp_log("gang wars: installed - %d gangs, %d rival pairs (%s), range %.0f m (+%.0f), calm %.1f s, provoke %d, spark %d, spare factions %s\n",
           g_c.ngroups, pairs, g_pairs_text, g_c.range, g_c.hysteresis, g_c.calm_s, g_c.provoke, g_c.spark, sp);
    return true;
}

// ---- test host only
static int __cdecl TestInstalled() { return g_installed ? 1 : 0; }
static int __cdecl TestCfg(int which) { return which == 0 ? g_c.ngroups : which == 1 ? g_c.nspares : which == 2 ? (g_c.provoke ? 1 : 0) : (int)(g_c.range * 10.0f); }
void* gw_test_stub(const char* name) {
    if (!strcmp(name, "gw_installed")) return (void*)&TestInstalled;
    if (!strcmp(name, "gw_cfg")) return (void*)&TestCfg;
    return nullptr;
}
