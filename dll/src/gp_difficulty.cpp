// gp_difficulty.cpp - [difficulty]: EASY / NORMAL / HARD / BRUTAL chosen in the game's own SETTINGS menu.
// Off unless [difficulty] enabled = 1; every site and every address it relies on byte-checked (sites.h
// MN_* / DF_* rows); nothing is written to the game while it is off.
//
// 1. Menu (dll/src/gp_menu.cpp hooks, option type 3 = the DIFFICULTY LEVEL entry):
//    * the entry gets a fourth value BRUTAL: the text ordinal comes from the data option `difficulty_menu` (it puts the
//      ordinal into the entry's OptionValueType4, a member the code never reads, and replaces uk_Menu #77 "FILL_IN" with
//      "BRUTAL"); the DLL appends it to the entry's value list at run time. Without the DLL the game offers the three
//      vanilla values, so nothing can ever store a value the vanilla menu cannot show.
//    * choosing BRUTAL stores Setting_difficulty = 2 (the game's HARD) and sets the DLL's flag, kept in
//      <Documents>\Total Overdose\savegames\TODGameplay_difficulty.txt next to Slot11. Vanilla / mod-off reads Hard.
//      Choosing EASY / NORMAL / HARD clears the flag. The menu shows BRUTAL while Setting_difficulty = 2 and the flag is set.
// 2. Tuning: the factors of the data option `difficulty` (options.py DIFFICULTY hard / brutal) are applied to the live
//    dictionary values of overdose.main (block 1, never reloaded) whenever the level changes, and back to the vanilla
//    values for EASY / NORMAL. Written without the member-write notify: no save records them, a save never carries them.
//    The static data option must be off (normal) in the data zip: the DLL checks PlayerStatusCtrl and refuses the
//    tuning when it finds a baked level (log line), so nothing is applied twice.
// 3. BRUTAL reinforcements (+50 % enemies): the engine has no spare enemy placements and no way to clone one at run time,
//    and new placements in mission data would shift save node indices. A dead enemy's placeholder is therefore brought
//    back once through its own state: the game's kill (0x5A8950) leaves it dormant with Status 3; the DLL sets Status 0 /
//    Locked 0 with the game's own builtin setters (0x5A18E0 / 0x5A1970, recorded in saves like any game write) and the
//    pool's resource manager gives it a fresh pooled character from the placement, armed as authored. Every counted enemy
//    death adds 0.5 to an accumulator; one placeholder comes back per whole unit. Only while Ram is 18..45 m away (not in
//    his face, inside the pool's placeholder radius), 4 s after the death, never twice, never 1000-hp specials,
//    buddies, civilians or the skip factions (6 civilians, 10 DEA, 13 wrestlers, 14 police). Kill tracking of the missions is untouched: their death
//    conditions fire on the first death as in vanilla; a counter objective may complete early (more kills).
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gp_glue.h"
#include "gp_difficulty_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v)  (*(volatile float*)(uintptr_t)(a) = (float)(v))

// ================================================================================================ game addresses
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_PLAYER_HOLDER = 0xA3B590;
static const DWORD A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_SCENE = 0xA3DCBC;
static const DWORD C_SETTINGS = 0x398, C_PSTATUS = 0x2B0, C_BULLETDROP = 0x2C0, C_WEAPLIST = 0x3BC, C_CHARLIST = 0x3A8,
                   C_MSTATLIST = 0x470, C_TIMESTAMP = 0x274, C_MISSION_NODE = 0x25C;
static const DWORD H_PH_CLS = 0x5A3100, H_ROOT_CLS = 0x5C3060, H_SENTRY_CLS = 0x5C90F0, H_WSUB_CLS = 0x6580F0, H_MSTAT_CLS = 0x649BB0;
static DWORD P_DF_SETPROP = 0x86A6C0, P_DF_GETPROP = 0x86A6A0;   // Node::SetProperty / GetProperty (REF DF_SETPROP / F_GETPROP)
static const int PROP_PH_DAMAGE = 0x287;         // character_placeholder DamageTaken: the AI init copies it into sentry +30h
typedef void(__fastcall* Prop_t)(DWORD node, void* edx, int id, void* v);          // Set / GetProperty, ret 8
static DWORD P_DF_GET = 0x869E80, P_DF_SENDCMD = 0x86A340, P_DF_GETPOS = 0x88CA80, P_DF_QUERY = 0x486950,
             P_DF_LISTFREE = 0x8718D0, P_DF_PHSTATUS = 0x5A18E0, P_DF_PHLOCKED = 0x5A1970, P_DF_SETMASK = 0x48F3A0, P_DF_SETPOS = 0x88D6C0, P_DF_SETFLAGS = 0x500BE0;
typedef void(__fastcall* SetPos_t)(DWORD node, void* edx, const float* v4);       // Node::SetPosition (local), ret 4
static const DWORD PH_MASK = 0x80;               // CharacterResourceCtrl PlaceHolderMask: only placeholders with it are collected
static const int CMD_GOTO_DEATH = 1537;          // character_sentry_ai command_goto_death (global id; debug only)

typedef void(__fastcall* Get_t)(DWORD node, void* edx, int slot, void* out);       // member get, ret 8
typedef char(__fastcall* SendCmd_t)(DWORD node, void* edx, int id, void* args);   // Node::SendCommand, ret 8
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);                 // GetWorldPosition, ret 4
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);                   // quadtree query, ret 4
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);             // list release, ret 4
typedef void(__fastcall* Set1_t)(DWORD node, void* edx, int v);                    // placeholder builtin setter, ret 4

// ================================================================================================ config / state
static bool g_on = false, g_menu = true, g_tuning = true, g_reinf = true, g_verbose = false, g_installed = false;
// [mayhem_heat] drives this reinforcement engine too (waves, comebacks, rooftops first): with [difficulty] off the engine
// alone is installed for it (no menu, no tuning, no cap)
static bool g_heat_reinf = false, g_engine_only = false;
static int g_brutal_text = 0, g_psycho_text = 0, g_force = -1;
static int g_rc_keep = 2;   // pool slots left to the mission's own placements
// [difficulty] max_live_enemies: CharacterResourceCtrl MaxAllocated on BRUTAL / PSYCHO (vanilla 6 or 8, written by
// FUN_00763440 at 0x763694 / 0x7636A6 at init and at every command_new_block_loaded). The pool has 9 character shells
// (DefaultEnemyResources AICharacter1..9): more needs new shells = create records in overdose.main (save break).
static const int POOL_SHELLS = 9;
static int g_max_live = 0, g_mx_base = 0, g_mx_written = 0;
static bool g_cap_with_friends = false;            // [difficulty] max_live_with_buddies (default 0: the game's cap in buddy missions)
static bool g_friend_mission = false;              // a buddy / escort / friend was seen since the mission started
bool df_friend_mission() { return g_friend_mission; }
static bool g_setprop_ok = false;
static bool g_mlog = true;                         // [difficulty] mission_log (see mission_log())
static float g_debug_kill_range = 60.0f;
static float g_debug_kill_s = 0.0f; static bool g_debug_no_damage = false, g_debug_focus = false; static bool g_debug_as_ram = false; static float g_debug_kill_stop = 0.0f; static double g_debug_first_kill = 0.0;
static int g_get_calls = 0;
static df::ReinfCfg g_rc;
static int g_skip[16], g_nskip = 0;
static int g_extra = 0;                            // 0, or the level above HARD: 1 BRUTAL, 2 PSYCHO (sidecar)
static int g_setting = -1, g_level = -1, g_applied = -1;
static char g_sidecar[MAX_PATH] = "";
static bool g_menu_extended = false; static int g_menu_values = 0;

static float fcfg(const char* k, float d) { return gp_ini().getf("difficulty", k, d); }
static bool  bcfg(const char* k, bool d) { return gp_ini().geti("difficulty", k, d ? 1 : 0) != 0; }

void difficulty_load_config() {
    g_on = bcfg("enabled", false);
    g_menu = bcfg("menu", true);
    g_tuning = bcfg("tuning", true);
    g_reinf = bcfg("reinforcements", true);
    g_verbose = bcfg("log_verbose", false);
    g_brutal_text = gp_ini().geti("difficulty", "brutal_text", 0);
    g_psycho_text = gp_ini().geti("difficulty", "psycho_text", 0);
    g_force = df::level_from_name(gp_ini().get("difficulty", "force_level"));
    g_rc.share = fcfg("reinforce_share", 0.5f);
    if (g_rc.share < 0.0f) g_rc.share = 0.0f; if (g_rc.share > 2.0f) g_rc.share = 2.0f;
    g_rc.share_psycho = fcfg("reinforce_share_psycho", g_rc.share_psycho);   // psycho-tune
    if (g_rc.share_psycho < 0.0f) g_rc.share_psycho = 0.0f; if (g_rc.share_psycho > 2.0f) g_rc.share_psycho = 2.0f;
    g_rc.min_distance = fcfg("reinforce_min_distance", g_rc.min_distance);
    g_rc.max_distance = fcfg("reinforce_max_distance", g_rc.max_distance);
    g_rc.delay_s = fcfg("reinforce_delay", g_rc.delay_s);
    g_rc_keep = gp_ini().geti("difficulty", "reinforce_keep_slots", 2);
    g_max_live = gp_ini().geti("difficulty", "max_live_enemies", 0);
    if (g_max_live < 0) g_max_live = 0; if (g_max_live > POOL_SHELLS) g_max_live = POOL_SHELLS;
    g_cap_with_friends = gp_ini().geti("difficulty", "max_live_with_buddies", 0) != 0;
    g_mlog = gp_ini().geti("difficulty", "mission_log", 1) != 0;
    g_debug_kill_range = fcfg("debug_kill_range", 60.0f);    // test copy only: reach of the debug kills
    g_debug_kill_s = fcfg("debug_kill_seconds", 0.0f);   // test copy only: kills the nearest enemy every N s
    g_debug_kill_stop = fcfg("debug_kill_stop", 0.0f);    // test copy only: no more debug kills N s after the first (0 = never stop)
    g_debug_no_damage = bcfg("debug_no_damage", false);   // test copy only: Ram takes no damage (DamageHard 0)
    g_debug_as_ram = bcfg("debug_kill_as_ram", false);  // test copy only: debug kills count as Ram's (KillOwner = Ram)
    g_debug_focus = bcfg("debug_focus", false);           // test copy only: a page open puts the menu focus on DIFFICULTY
    g_heat_reinf = heat_ini_wants_reinforcements();
    const char* sk = gp_ini().get("difficulty", "skip_factions");
    static const int def[] = {6, 10, 13, 14};   // 6 Civilian, 10 DEA (Ram's side), 13 wrestlers, 14 Police
    g_nskip = 0;
    if (!sk) { for (int f : def) g_skip[g_nskip++] = f; }
    else for (const char* p = sk; *p && g_nskip < 16;) {
        while (*p && (*p < '0' || *p > '9')) ++p;
        if (!*p) break;
        g_skip[g_nskip++] = atoi(p);
        while (*p >= '0' && *p <= '9') ++p;
    }
}
bool difficulty_enabled() { return g_on; }
bool difficulty_wants_tick() { return (g_on && (g_tuning || g_reinf || g_force >= 0)) || (g_heat_reinf && g_reinf); }

// ================================================================================================ helpers
static DWORD sdata(DWORD n) { __try { return n ? RD32(n + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_of(DWORD node) {   // node +4 type, +28h script class object, +60h handler (REF SV_CLASSOBJ)
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// the script "current node" globals: game calls made from the tick may change them; put them back afterwards
struct Cur { DWORD a, b, c; };
static Cur cur_save() { Cur k; k.a = RD32(A_CUR_NODE); k.b = RD32(A_CUR_2); k.c = RD32(A_CUR_3); return k; }
static void cur_restore(const Cur& k) {
    if (RD32(A_CUR_NODE) != k.a) WR32(A_CUR_NODE, k.a);
    if (RD32(A_CUR_2) != k.b) WR32(A_CUR_2, k.b);
    if (RD32(A_CUR_3) != k.c) WR32(A_CUR_3, k.c);
}
static bool get_member(DWORD node, int slot, DWORD* out) {
    if (!node) return false;
    Cur k = cur_save(); bool ok = true;
    __try { ((Get_t)(uintptr_t)P_DF_GET)(node, nullptr, slot, out); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static bool get_pos(DWORD node, float* p) {
    if (!node) return false;
    float v[3] = {0, 0, 0}; bool ok = true;
    Cur k = cur_save();
    __try { ((Vec_t)(uintptr_t)P_DF_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    if (!ok || !(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    return true;
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool game_ms(uint32_t* ms) {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try { DWORD n = RD32(cd + C_TIMESTAMP); DWORD d = n ? RD32(n + 0x18) : 0; if (!d) return false; *ms = RD32(d + 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int mission_now() {
    DWORD cd = gp_cache_data(); if (!cd) return -1;
    __try { DWORD m = RD32(cd + C_MISSION_NODE); DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + 0x38) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// list(Entity) member value -> element i (stride [0xA3B8A8]+24h, as the game divides)
static int list_count(DWORD L) { __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; return (L && k) ? (int)(RD32(L + 8) / k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD list_at(DWORD L, int i) {
    __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
            return el ? RD32(el + (DWORD)i * 4 * k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ================================================================================================ sidecar
static void sidecar_path() {
    char docs[MAX_PATH] = "";
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
        sprintf_s(g_sidecar, "%s\\Total Overdose\\savegames\\TODGameplay_difficulty.txt", docs);
}
static void sidecar_load() {
    if (!g_sidecar[0]) return;
    FILE* f = nullptr; if (fopen_s(&f, g_sidecar, "rb") || !f) { gp_log("difficulty: no %s (BRUTAL flag off)\n", g_sidecar); return; }
    char t[512]; size_t n = fread(t, 1, sizeof t - 1, f); fclose(f); t[n] = 0;
    int e = 0;
    if (df::parse_sidecar(t, &e)) g_extra = e;
    gp_log("difficulty: %s: level above HARD = %s\n", g_sidecar, g_extra ? df::level_name(df::HARD + g_extra) : "none");
}
static void sidecar_save() {
    if (!g_sidecar[0]) return;
    char t[256]; int n = df::format_sidecar(g_extra, t, sizeof t);
    FILE* f = nullptr; if (fopen_s(&f, g_sidecar, "wb") || !f) { gp_log("difficulty: could not write %s\n", g_sidecar); return; }
    fwrite(t, 1, n, f); fclose(f);
    gp_log("difficulty: saved level above HARD = %s to %s\n", g_extra ? df::level_name(df::HARD + g_extra) : "none", g_sidecar);
}

// ================================================================================================ menu (option type 3)
static const DWORD H_HUDIM_CLS = 0x6AE490;   // HUD_intermission handler (debug_focus only)
// test copy only ([difficulty] debug_focus = 1): the page open puts EntryFocus (HUD_intermission data +CCh) on this entry's
// index in CurrentEntryList (+B4h), so a key script does not depend on how the injected arrow keys are taken
static void debug_focus(DWORD node, DWORD data) {
    __try {
        DWORD hud = RD32(data + 0x08); DWORD hd = sdata(hud);
        if (!hd || cls_of(hud) != H_HUDIM_CLS) { gp_log("difficulty: debug focus: entry +08h %08X is not the HUD_intermission\n", hud); return; }
        DWORD L = RD32(hd + 0xB4); int n = list_count(L);
        for (int i = 0; i < n && i < 64; ++i)
            if (list_at(L, i) == node) {
                int old = (int)RD32(hd + 0xCC); WR32(hd + 0xCC, i);
                gp_log("difficulty: debug focus: EntryFocus %d -> %d (DIFFICULTY LEVEL, %d entries on the page)\n", old, i, n);
                return;
            }
        gp_log("difficulty: debug focus: the entry is not in CurrentEntryList (%d entries)\n", n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void menu_get(DWORD node, DWORD data) {
    ++g_get_calls;
    if (g_debug_focus && g_get_calls > 1) debug_focus(node, data);
    int n = gp_menu_value_count(data);
    if (!g_menu_extended && n >= 3) {
        // value 4 = BRUTAL (marker OptionValueType4, +38h), value 5 = PSYCHO (marker OptionValueType5, +40h): each marker
        // holds the text ordinal the data option put in place; the code never reads OptionValueType2..5
        static const char* const names[] = {"BRUTAL", "PSYCHO"};
        for (int slot = 4; slot <= 5 && n == slot - 1; ++slot) {
            int marker = 0, vs = 1;
            __try { marker = (int)RD32(data + 0x20 + 8 * (slot - 1)); vs = (int)RD32(data + 0x1C + 8 * (slot - 1)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            int ini = slot == 4 ? g_brutal_text : g_psycho_text;
            int ord = (marker > 0 && marker < 2000 && vs == 0) ? marker : ini;
            if (ord <= 0) { gp_log("difficulty: no %s text (data option difficulty_menu not in the zip): not offered\n", names[slot - 4]); break; }
            n = gp_menu_append_value(data, ord);
            gp_log("difficulty: menu entry %08X: %s (text #%d, %s) appended -> %d values\n", node, names[slot - 4], ord,
                   ord == marker ? "data option difficulty_menu" : "ini", n);
            if (n < 0) break;
        }
        g_menu_extended = true;
    }
    n = gp_menu_value_count(data);
    __try {
        int cur = (int)RD32(data + 0x4C);
        int shown = df::menu_shown_value(cur, g_extra, n);
        if (shown != cur) WR32(data + 0x4C, shown);
        g_menu_values = n;
        if (g_verbose) gp_log("difficulty: menu shows %d (setting %d, extra %d, %d values)\n", shown, cur, g_extra, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void menu_set(int* tv) {
    df::SetResult r = df::menu_chosen(tv[1]);
    if (g_menu_values >= 3 && r.extra > g_menu_values - 3) r.extra = g_menu_values - 3;   // never more than offered
    int before = tv[1];
    tv[1] = r.setting;
    if (r.extra != g_extra) { g_extra = r.extra; sidecar_save(); }
    gp_log("difficulty: menu chose %d -> Setting_difficulty %d, extra %d (%s)\n", before, r.setting, g_extra,
           df::level_name(df::level_from_setting(r.setting, g_extra)));
}

// ================================================================================================ tuning
struct Val { DWORD addr; float base, written; int target; };
static Val g_vals[160];
static int g_nvals = 0;
static bool g_captured = false, g_tuning_refused = false;
static int g_capture_tries = 0;
static void add_val(DWORD addr, int target) {
    if (g_nvals >= 160) return;
    float b = RDF(addr);
    g_vals[g_nvals++] = Val{addr, b, b, target};
}
static bool nearf(float a, float b) { return fabsf(a - b) <= 1e-4f; }
// fills g_vals from the live dictionaries; checks known vanilla values first (proves the offsets on this data)
static bool capture() {
    DWORD cd = gp_cache_data(); if (!cd) return false;
    g_nvals = 0;
    __try {
        DWORD ps = sdata(RD32(cd + C_PSTATUS)), bd = sdata(RD32(cd + C_BULLETDROP));
        DWORD wl = RD32(cd + C_WEAPLIST), cl = RD32(cd + C_CHARLIST), ml = RD32(cd + C_MSTATLIST);
        if (!ps || !bd || !wl || !cl || !ml || list_count(wl) < 10 || list_count(cl) < 8 || list_count(ml) < 8) return false;
        int st = df::static_level(RDF(ps + 0x98), RDF(ps + 0x9C));
        if (st != df::NORMAL) {
            gp_log("difficulty: PlayerStatusCtrl DamageHard %.3f / PlayerDamageMod %.3f: the data zip still has the STATIC "
                   "option difficulty = %s: runtime tuning OFF (rebuild the zip with difficulty = normal)\n",
                   RDF(ps + 0x98), RDF(ps + 0x9C), st >= 0 ? df::level_name(st) : "unknown");
            g_tuning_refused = true; return false;
        }
        if (!nearf(RDF(ps + 0x90), 0.65f) || !nearf(RDF(ps + 0x94), 1.1f) || !nearf(RDF(bd + 0x0C), 10.0f) || !nearf(RDF(bd + 0x30), 250.0f)) {
            gp_log("difficulty: PlayerStatusCtrl / BulletDrop do not hold the known vanilla values: runtime tuning OFF\n");
            g_tuning_refused = true; return false;
        }
        add_val(ps + 0x98, df::T_DAMAGE_TAKEN);
        add_val(ps + 0x9C, df::T_DAMAGE_DEALT);
        for (DWORD o = 0x0C; o <= 0x30; o += 4) add_val(bd + o, df::T_AMMO);
        int nw = 0, nc = 0, nm = 0, bad = 0;
        for (int i = 0, n = list_count(wl); i < n; ++i) {
            DWORD nd = list_at(wl, i); DWORD d = sdata(nd);
            if (!d || cls_of(nd) != H_WSUB_CLS) { ++bad; continue; }
            int id = (int)RD32(d + 0x1C);
            if (id == 6 && !(nearf(RDF(d + 0xC0), 1.4f) && nearf(RDF(d + 0x150), 0.8f) && nearf(RDF(d + 0x154), 2.0f))) { ++bad; continue; }
            if (!df::weapon_row_tuned(id)) continue;
            add_val(d + 0xC0, df::T_SPREAD); add_val(d + 0x150, df::T_PAUSE); add_val(d + 0x154, df::T_PAUSE); ++nw;
        }
        for (int i = 0, n = list_count(cl); i < n; ++i) {
            DWORD d = sdata(list_at(cl, i));
            if (!d) { ++bad; continue; }
            int id = (int)RD32(d + 0x10);
            if (id != i) { ++bad; continue; }                          // the list is indexed by Specific_ID (AISubType)
            if (id == 7 && !(nearf(RDF(d + 0x2C), 1.0f) && nearf(RDF(d + 0x30), 3.0f))) { ++bad; continue; }
            if (!df::chartype_row_tuned(id)) continue;
            add_val(d + 0x2C, df::T_REACT); add_val(d + 0x30, df::T_REACT); ++nc;
        }
        for (int i = 0, n = list_count(ml); i < n; ++i) {
            DWORD nd = list_at(ml, i); DWORD d = sdata(nd);
            if (!d || cls_of(nd) != H_MSTAT_CLS) { ++bad; continue; }
            int t = (int)RD32(d + 0x0C);
            if (!df::modelstat_row_tuned(t)) continue;
            add_val(d + 0x4C, df::T_REACT); add_val(d + 0x50, df::T_REACT); ++nm;
        }
        gp_log("difficulty: tuning captured %d values (PlayerStatus 2, BulletDrop 10, %d firearms, %d behaviour rows, %d model rows; %d rows skipped)\n",
               g_nvals, nw, nc, nm, bad);
        if (nw < 10 || nc < 15 || nm < 20) { gp_log("difficulty: fewer rows than the data has: runtime tuning OFF\n"); g_tuning_refused = true; g_nvals = 0; return false; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_nvals = 0; return false; }
    return true;
}
static void apply(int level) {
    int changed = 0, adopted = 0;
    __try {
        for (int i = 0; i < g_nvals; ++i) {
            Val& v = g_vals[i];
            float cur = RDF(v.addr);
            if (cur != v.written) { v.base = cur; ++adopted; }     // something else changed it: that is the new base
            float nv = v.target == df::T_AMMO ? v.base * loot_ammo_factor(df::factors(level).k[df::T_AMMO])   // [drops] replaces the factor
                                               : df::tuned(v.target, v.base, level);
            if (g_debug_no_damage && v.target == df::T_DAMAGE_TAKEN) nv = 0.0f;   // test copy only
            if (nv != cur) { WRF(v.addr, nv); ++changed; }
            v.written = nv;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("difficulty: tuning write failed\n"); return; }
    bool quiet = g_applied == level && !changed;
    g_applied = level;
    if (quiet) return;
    df::Factors f = df::factors(level);
    gp_log("difficulty: level %s applied: %d values written%s (damage taken x%.2f, dealt x%.2f, spread x%.2f, pause x%.2f, "
           "reaction x%.2f, dropped ammo x%.2f)\n", df::level_name(level), changed, adopted ? " (some base values re-read)" : "",
           f.k[df::T_DAMAGE_TAKEN], f.k[df::T_DAMAGE_DEALT], f.k[df::T_SPREAD], f.k[df::T_PAUSE], f.k[df::T_REACT], loot_ammo_factor(f.k[df::T_AMMO]));
}

// ================================================================================================ reinforcements
enum { MAXP = 64 };
struct Ph {
    bool used, candidate, dead, counted, rearmed, live_now, was_alive, arrived, in_tree, noted; int callbacks, rearms;
    DWORD node; int status, locked; DWORD res, mask;
    df::Seen seen; uint32_t dead_ms, seen_ms; double dead_t; float pos[3]; bool has_pos;
};
static Ph g_p[MAXP];
static df::ReinfState g_rs;
static df::ReinfCfg g_rc_eff;                      // this pass: BRUTAL's share (if BRUTAL) + the heat's share
static HeatDrive g_hd;                             // what [mayhem_heat] asks this pass
static uint32_t g_last_gms = 0; static int g_mission = -2;
static double g_last_reinf = 0.0, g_last_kill = 0.0, g_last_report = 0.0;
static int g_live = 0, g_live_max = 0, g_seen_total = 0, g_arrived = 0;
static DWORD sp_mask(DWORD node) {   // NodeSpatial type mask (node +30h -> +1Ch, low 24 bits; 0x48F390)
    __try { DWORD s = node ? RD32(node + 0x30) : 0; return s ? (RD32(s + 0x1C) & 0xFFFFFF) : 0xFFFFFFFF; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}
static void dump_node(const char* what, DWORD n) {   // log_verbose diagnostics: node flags and its spatial record
    __try {
        DWORD sp = RD32(n + 0x30);
        gp_log("difficulty: diag %s %08X flags28 %08X n2C %08X n34 %08X n38 %08X n3C %08X | sp %08X: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
               what, n, RD32(n + 0x28), RD32(n + 0x2C), RD32(n + 0x34), RD32(n + 0x38), RD32(n + 0x3C), sp,
               sp ? RD32(sp + 0x00) : 0, sp ? RD32(sp + 0x04) : 0, sp ? RD32(sp + 0x08) : 0, sp ? RD32(sp + 0x0C) : 0, sp ? RD32(sp + 0x10) : 0,
               sp ? RD32(sp + 0x14) : 0, sp ? RD32(sp + 0x18) : 0, sp ? RD32(sp + 0x1C) : 0, sp ? RD32(sp + 0x20) : 0, sp ? RD32(sp + 0x24) : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static Ph* ph_slot(DWORD node, bool make) {
    for (Ph& p : g_p) if (p.used && p.node == node) return &p;
    if (!make) return nullptr;
    for (Ph& p : g_p) if (!p.used) { p = Ph{}; p.used = true; p.node = node; p.status = -1; p.locked = -1; return &p; }
    return nullptr;
}
static void forget_all(const char* why) {
    int n = 0; for (Ph& p : g_p) if (p.used) { ++n; p.used = false; }
    if (n || g_rs.deaths) gp_log("difficulty: %s: %d placeholders forgotten (deaths %d, reinforcements %d)\n", why, n, g_rs.deaths, g_rs.rearmed);
    g_rs.reset(); g_seen_total = 0; g_arrived = 0; g_live_max = 0; g_friend_mission = false;
}
static bool read_enemy(DWORD res, df::Seen* s, bool* alive) {
    __try {
        if (!res || cls_of(res) != H_ROOT_CLS) return false;
        DWORD rd = sdata(res); if (!rd) return false;
        DWORD ai = RD32(rd + 0x58); DWORD d = sdata(ai);
        if (!ai || !d || cls_of(ai) != H_SENTRY_CLS) return false;
        s->faction = (int)RD32(rd + 0x18); s->sub_type = (int)RD32(rd + 0x14);
        float hp = RDF(d + 0x2C); if (hp > s->health) s->health = hp;
        s->buddy = RD32(d + 0xB8) != 0;
        *alive = hp > 0.0f && (int)RD32(d + 0x1D4) != 28;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// a dead or hidden placement back in play, the same steps as rearm() below (the DamageTaken reset included): the heat vehicles'
// gunmen go out again with this. false = not a placeholder.
bool df_rearm_placeholder(DWORD node) {
    bool ok = true;
    Cur k = cur_save();
    __try {
        DWORD before = sp_mask(node);
        if (cls_of(node) != H_PH_CLS || before == 0xFFFFFFFF) ok = false;
        else {
            if (g_setprop_ok) { float z = 0.0f; ((Prop_t)(uintptr_t)P_DF_SETPROP)(node, nullptr, PROP_PH_DAMAGE, &z); }
            ((Set1_t)(uintptr_t)P_DF_PHSTATUS)(node, nullptr, 0);
            ((Set1_t)(uintptr_t)P_DF_PHLOCKED)(node, nullptr, 0);
            ((Set1_t)(uintptr_t)P_DF_SETMASK)(node, nullptr, (int)(before | PH_MASK));
            DWORD fl = RD32(node + 0x28);
            if (fl & 1) ((Set1_t)(uintptr_t)P_DF_SETFLAGS)(node, nullptr, (int)(fl & 0xFFE));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static void rearm(Ph& p, float dist, float dy, const char* why) {
    bool ok = true;
    Cur k = cur_save();
    DWORD before = sp_mask(p.node);
    float dmg0 = -1.0f, dmg1 = -1.0f;
    __try {
        if (cls_of(p.node) != H_PH_CLS || before == 0xFFFFFFFF) ok = false;
        else {
            // The dead placement still stores its last enemy's DamageTaken (>= Health) and the AI init 0x5D4190
            // copies it into the new enemy: without this every reinforcement is born dead (ragdolls / Smart AI take them
            // for corpses at once, one hit kills them). Cleared with the game's own property setter before the spawn.
            if (g_setprop_ok) {
                DWORD raw = 0; ((Prop_t)(uintptr_t)P_DF_GETPROP)(p.node, nullptr, PROP_PH_DAMAGE, &raw); memcpy(&dmg0, &raw, 4);
                if (dmg0 != 0.0f) { float z = 0.0f; ((Prop_t)(uintptr_t)P_DF_SETPROP)(p.node, nullptr, PROP_PH_DAMAGE, &z);
                    raw = 0; ((Prop_t)(uintptr_t)P_DF_GETPROP)(p.node, nullptr, PROP_PH_DAMAGE, &raw); memcpy(&dmg1, &raw, 4); }
                else dmg1 = 0.0f;
            }
            if (p.status != 0) ((Set1_t)(uintptr_t)P_DF_PHSTATUS)(p.node, nullptr, 0);
            if (p.locked != 0) ((Set1_t)(uintptr_t)P_DF_PHLOCKED)(p.node, nullptr, 0);
            ((Set1_t)(uintptr_t)P_DF_SETMASK)(p.node, nullptr, (int)(before | PH_MASK));
            // the death also hid the node (flag 1): the spatial query skips hidden nodes. Show it the game's way
            // (show 0x62E110: SetFlags(flags & FFEh), REF SV_SHOW_FN / F_SETFLAGS)
            DWORD fl = RD32(p.node + 0x28);
            if (fl & 1) ((Set1_t)(uintptr_t)P_DF_SETFLAGS)(p.node, nullptr, (int)(fl & 0xFFE));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    p.rearmed = true; ++p.rearms; p.arrived = false;
    gp_log("difficulty: %s reinforcement %d: placeholder %08X (faction %d, hp %.0f) re-armed%s (comeback %d; spatial mask %06X -> %06X, node flags %03X), Ram %.1f m away, "
           "%.1f m above him (enemy deaths %d, accumulator %.2f; stored DamageTaken %.0f -> %.0f)\n", why, g_rs.rearmed, p.node, p.seen.faction, p.seen.health, ok ? "" : " FAILED", p.rearms, before,
           sp_mask(p.node), RD32(p.node + 0x28) & 0xFFF, dist, dy, g_rs.deaths, g_rs.acc, dmg0, dmg1);
}
static void debug_kill(const float* rp) {
    Ph* best = nullptr; float bd = 1e9f;
    for (Ph& p : g_p) {
        if (!p.used || !p.live_now || !p.candidate || !p.has_pos) continue;
        float dx = p.pos[0] - rp[0], dz = p.pos[2] - rp[2]; float d = sqrtf(dx * dx + dz * dz);
        if (d < bd) { bd = d; best = &p; }
    }
    if (!best || bd > g_debug_kill_range) return;
    DWORD rd = sdata(best->res); DWORD ai = 0; __try { ai = rd ? RD32(rd + 0x58) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!ai) return;
    bool ok = true;
    Cur k = cur_save();
    // sentry KillOwner (REF HT_KILLOWNER). A kill counted as Ram's also sends the combo message 0x1FF with the victim's
    // Killer (+174h, the inflictor): the combo handler asks IsA(weapon) of it (0x624C03 -> 0x86A130) without a null
    // check, so an enemy that was never hit (inflictor 0) crashes the game there. A real
    // kill always has an inflictor; the test helper puts Ram's root there when it is empty (IsA answers "no weapon").
    if (g_debug_as_ram) __try {
        DWORD sd = sdata(ai);
        if (sd) {
            DWORD ram = ram_root();
            *(volatile DWORD*)(uintptr_t)(sd + 0x178) = ram;
            if (!*(volatile DWORD*)(uintptr_t)(sd + 0x174)) *(volatile DWORD*)(uintptr_t)(sd + 0x174) = ram;
            if (!*(volatile DWORD*)(uintptr_t)(sd + 0x150)) *(volatile DWORD*)(uintptr_t)(sd + 0x150) = ram;   // the death state asks IsA of this one too (0x5D7776)
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { ((SendCmd_t)(uintptr_t)P_DF_SENDCMD)(ai, nullptr, CMD_GOTO_DEATH, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    gp_log("difficulty: debug kill: command_goto_death to the enemy of placeholder %08X (%.1f m)%s\n", best->node, bd, ok ? "" : " FAILED");
}
static double g_last_diag = 0.0;
static void diag_characters(const float* rp) {   // log_verbose: every character within 60 m and its placeholder
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp[0], rp[1], rp[2], 60.0f, 0x40, 0};
    DWORD qt = 0; __try { qt = RD32(A_QUADTREE); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!qt) return;
    Cur k = cur_save(); bool ok = true;
    __try { ((Query_t)(uintptr_t)P_DF_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    if (!ok || !q.out) return;
    DWORD L = q.out; DWORD ram = ram_root(); int n = 0;
    __try {
        for (int i = 0, c = list_count(L); i < c && i < 64; ++i) {
            DWORD r = list_at(L, i);
            if (!r || r == ram || cls_of(r) != H_ROOT_CLS) continue;
            DWORD rd = sdata(r); DWORD ai = rd ? RD32(rd + 0x58) : 0; DWORD d = sdata(ai);
            DWORD ph = rd ? RD32(rd + 0x24) : 0; DWORD pd = sdata(ph);
            float p[3] = {0, 0, 0}; get_pos(r, p);
            float dx = p[0] - rp[0], dz = p[2] - rp[2];
            gp_log("difficulty: diag character %08X faction %d sub %d hp %.0f state %d %.1f m | placeholder %08X cls %s status %d res %08X\n",
                   r, rd ? (int)RD32(rd + 0x18) : -1, rd ? (int)RD32(rd + 0x14) : -1, d ? RDF(d + 0x2C) : -1.0f, d ? (int)RD32(d + 0x1D4) : -1,
                   sqrtf(dx * dx + dz * dz), ph, cls_of(ph) == H_PH_CLS ? "placeholder" : "other", ph ? (int)((RD32(ph + 0x50) >> 4) & 3) : -1,
                   pd ? RD32(pd + 0x14) : 0);
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    k = cur_save();
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((Free_t)(uintptr_t)P_DF_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
    if (!n) gp_log("difficulty: diag: no character within 60 m\n");
}
// one quadtree query around Ram: mask 40h mode 0 = characters (the Tornado's), mask 80h mode 1 = character placeholders
// (the resource collect's; a placeholder that owns a character is not in it - it is found through the character)
static DWORD query(const float* rp, float radius, int mask, int mode) {
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp[0], rp[1], rp[2], radius, mask, mode};
    DWORD qt = 0; __try { qt = RD32(A_QUADTREE); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!qt) return 0;
    Cur k = cur_save(); bool ok = true;
    __try { ((Query_t)(uintptr_t)P_DF_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok ? q.out : 0;
}
static void query_free(DWORD L) {
    if (!L) return;
    Cur k = cur_save();
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((Free_t)(uintptr_t)P_DF_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}
static void on_died(Ph& p, uint32_t gms, const char* how) {
    if (p.dead) return;
    p.dead = true; p.dead_ms = gms; p.dead_t = gp_now();
    if (!p.rearmed) { g_rs.on_death(g_rc_eff); p.counted = true; }
    int cb = -1; __try { DWORD d = sdata(p.node); cb = d ? list_count(RD32(d + 0x24)) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("difficulty: placeholder %08X: its enemy died (%s; status %d, resource %08X, callbacks %d -> %d)%s - deaths %d, accumulator %.2f\n", p.node, how,
           p.status, p.res, p.callbacks, cb, p.rearmed ? " [a reinforcement: not counted]" : "", g_rs.deaths, g_rs.acc);
}
static DWORD g_folder = 0;
typedef void(__fastcall* Find_t)(DWORD parent, void* edx, void* q);   // find child by name (Suave's P_FINDCHILD), ret 4
static DWORD find_child(DWORD parent, const char* name) {
    if (!parent) return 0;
    struct { DWORD out; const char* name; } q = {0, name};
    Cur k = cur_save();
    __try { ((Find_t)(uintptr_t)0x88EB60)(parent, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { q.out = 0; }
    cur_restore(k);
    return q.out;
}
// GameplayEssentials MasterCtrlAux/ResourceCtrl/CharacterResourceCtrl (MasterCtrlAux = the BulletDrop node's parent)
static DWORD char_resource_ctrl_data() {
    static DWORD cached = 0;
    if (cached) return sdata(cached);
    DWORD cd = gp_cache_data(); if (!cd) return 0;
    DWORD bd = 0, aux = 0; __try { bd = RD32(cd + C_BULLETDROP); aux = bd ? RD32(bd + 0x40) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cached = find_child(find_child(aux, "ResourceCtrl"), "CharacterResourceCtrl");   // overdose.main: stays loaded
    return sdata(cached);
}
// spare pool budget: HasResourcePHList (+4Ch) below MaxAllocated (+18h) minus `keep` slots for the mission's own spawns
static bool pool_has_room(int keep, int* has, int* mx) {
    DWORD d = char_resource_ctrl_data(); *has = -1; *mx = -1;
    if (!d) return true;   // unknown: do not block
    __try { *mx = (int)RD32(d + 0x18); *has = list_count(RD32(d + 0x4C)); } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    return *has + keep < *mx;
}
// a mission death condition registered on the placement (CallbackTrigger_List, data +24h) that is not met yet (+44h
// IsMet) still counts this placement: bringing it back would hand the mission a second, different death to wait for
static int unmet_conditions(DWORD ph) {
    int n = 0;
    __try {
        DWORD d = sdata(ph); DWORD L = d ? RD32(d + 0x24) : 0;
        for (int i = 0, c = list_count(L); i < c && i < 32; ++i) { DWORD cd = sdata(list_at(L, i)); if (cd && !RD32(cd + 0x44)) ++n; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 99; }
    return n;
}
static void diag_resource_ctrl() {   // log_verbose: CharacterResourceCtrl MaxAllocated, Wants / Has lists
    DWORD d = char_resource_ctrl_data();
    if (!d) { gp_log("difficulty: diag: CharacterResourceCtrl not found\n"); return; }
    char buf[600]; int o = 0;
    __try {
        DWORD H = RD32(d + 0x4C), W = RD32(d + 0x48);
        int nh = list_count(H), nw = list_count(W);
        o += sprintf_s(buf + o, sizeof buf - o, "max %d, wants %d, has %d:", (int)RD32(d + 0x18), nw, nh);
        for (int i = 0; i < nh && i < 12 && o < 520; ++i) {
            DWORD ph = list_at(H, i); DWORD pd = sdata(ph);
            Ph* p = nullptr; for (Ph& x : g_p) if (x.used && x.node == ph) p = &x;
            o += sprintf_s(buf + o, sizeof buf - o, " %08X(st %d res %08X%s%s)", ph, (int)((RD32(ph + 0x50) >> 4) & 3), pd ? RD32(pd + 0x14) : 0,
                           p && p->rearmed ? " R" : "", p && p->dead ? " dead" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("difficulty: diag CharacterResourceCtrl %s\n", buf);
}
static void reinf_tick(double now) {
    uint32_t gms = 0;
    if (!game_ms(&gms)) return;
    int m = mission_now();
    if (m != g_mission) { if (g_mission != -2) forget_all("mission changed"); g_mission = m; g_last_kill = gp_now() + 15.0; }   // test copy: no debug kill in a starting mission (the script side is not up)
    DWORD folder = 0; __try { DWORD sc = RD32(A_SCENE); folder = sc ? RD32(sc + 0x1B4) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (folder != g_folder) { if (g_folder) forget_all("the loaded submap changed"); g_folder = folder; }
    if (gms + 2000 < g_last_gms) forget_all("game clock went back (load / retry)");
    g_last_gms = gms;
    DWORD ram = ram_root(); float rp[3];
    if (!ram || !get_pos(ram, rp)) return;
    int live = 0, found = 0;
    bool dump = g_verbose && now - g_last_diag >= 10.0;
    for (Ph& p : g_p) { p.live_now = false; p.in_tree = false; }
    // A. live characters -> their placeholders
    DWORD L = query(rp, 80.0f, 0x40, 0);
    __try {
        for (int i = 0, n = list_count(L); L && i < n && i < 256; ++i) {
            DWORD r = list_at(L, i);
            if (!r || r == ram || cls_of(r) != H_ROOT_CLS) continue;
            DWORD rd = sdata(r); DWORD ph = rd ? RD32(rd + 0x24) : 0; DWORD pd = sdata(ph);
            if (!pd || cls_of(ph) != H_PH_CLS || RD32(pd + 0x14) != r) continue;   // owned through its placeholder only
            Ph* p = ph_slot(ph, true);
            if (!p) continue;
            bool alive = false;
            if (!read_enemy(r, &p->seen, &alive)) continue;
            if (alive && !g_friend_mission && (p->seen.buddy || p->seen.sub_type == 4 || p->seen.faction == 10 || p->seen.faction == 18 || p->seen.faction == 29 || p->seen.faction == 37)) {
                g_friend_mission = true;
                gp_log("difficulty: a buddy / escort / friend (character %08X, faction %d, sub type %d) is in this mission: the live cap stays the game's%s\n",
                       r, p->seen.faction, p->seen.sub_type, g_cap_with_friends ? " (unless max_live_with_buddies = 1: it is)" : "");
            }
            p->seen_ms = gms; p->res = r; p->status = (int)((RD32(ph + 0x50) >> 4) & 3);
            p->has_pos = get_pos(ph, p->pos);
            if (!p->candidate) {
                p->candidate = df::reinforcement_candidate(p->seen, g_rc, g_skip, g_nskip);
                p->callbacks = list_count(RD32(pd + 0x24));   // CallbackTrigger_List: mission conditions listening to this placement
                if (p->candidate) { ++g_seen_total; gp_log("difficulty: enemy %d in this fight: placeholder %08X character %08X faction %d hp %.0f, %d mission callbacks%s\n", g_seen_total, ph, r, p->seen.faction, p->seen.health, p->callbacks, p->rearmed ? " (a reinforcement)" : ""); }
            }
            if (alive) {
                p->live_now = true; if (p->candidate) ++live;
                if (p->rearmed && !p->arrived) {
                    p->arrived = true; ++g_arrived; p->dead = false;
                    float cp[3] = {0, 0, 0}; get_pos(r, cp); float dx = cp[0] - rp[0], dz = cp[2] - rp[2];
                    float hp = -1.0f, taken = -1.0f; const char* fixed = "";
                    __try {
                        DWORD rd2 = sdata(r); DWORD ai2 = rd2 ? RD32(rd2 + 0x58) : 0; DWORD d2 = sdata(ai2);
                        if (d2) { hp = RDF(d2 + 0x2C); taken = RDF(d2 + 0x30); }
                        if (d2 && df::born_dead(hp, taken) && gp_ref_ok("F_TOUCH")) {   // the stored damage came through anyway: the game's member write (slot 3)
                            ((void(__fastcall*)(DWORD, void*, int))(uintptr_t)0x86B4B0)(ai2, nullptr, 3); WRF(d2 + 0x30, 0.0f);
                            fixed = " - it came with a lethal DamageTaken: cleared";
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    gp_log("difficulty: reinforcement arrived: placeholder %08X has a live enemy again (character %08X, hp %.0f, DamageTaken %.0f%s, %.1f m from Ram, %.1f m above him; comeback %d; %d of %d arrived)\n",
                           ph, r, hp, taken, fixed, sqrtf(dx * dx + dz * dz), cp[1] - rp[1], p->rearms, g_arrived, g_rs.rearmed);
                }
            } else if (p->candidate && p->was_alive) on_died(*p, gms, "the character's AI is dying / dead");
            p->was_alive = alive;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    query_free(L);
    // B. placeholders without a character (dormant, dead): status for the known ones
    L = query(rp, 80.0f, 0x80, 1);
    __try {
        for (int i = 0, n = list_count(L); L && i < n && i < 256; ++i) {
            DWORD nd = list_at(L, i);
            if (!nd || cls_of(nd) != H_PH_CLS) continue;
            ++found;
            DWORD w = RD32(nd + 0x50); int status = (int)((w >> 4) & 3), locked = (int)((w >> 8) & 1);
            DWORD d = sdata(nd); DWORD res = d ? RD32(d + 0x14) : 0;
            Ph* p = ph_slot(nd, false);
            if (dump && i < 3) dump_node("in-tree placeholder", nd);
            if (dump) gp_log("difficulty: diag placeholder in the tree %08X status %d locked %d resource %08X mask %06X flags50 %08X tracked %d\n", nd, status, locked, res, sp_mask(nd), w, p ? 1 : 0);
            if (!p) continue;
            if (g_verbose && (status != p->status || locked != p->locked || res != p->res))
                gp_log("difficulty: placeholder %08X status %d locked %d resource %08X -> status %d locked %d resource %08X\n",
                       nd, p->status, p->locked, p->res, status, locked, res);
            p->in_tree = true; p->seen_ms = gms; p->status = status; p->locked = locked; p->res = res;
            p->has_pos = get_pos(nd, p->pos);
            if (p->candidate && !p->dead && status == 3) on_died(*p, gms, "placeholder status 3");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    query_free(L);
    // C. every tracked placeholder read directly: its character released / Status 3 = its enemy died
    for (Ph& p : g_p) {
        if (!p.used) continue;
        int status = -1, locked = -1; DWORD res = 0; bool ok = false;
        __try {
            if (cls_of(p.node) == H_PH_CLS) {
                DWORD w = RD32(p.node + 0x50); status = (int)((w >> 4) & 3); locked = (int)((w >> 8) & 1);
                DWORD d = sdata(p.node); res = d ? RD32(d + 0x14) : 0; ok = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok) { gp_log("difficulty: placeholder %08X is gone (block unloaded?): forgotten\n", p.node); p.used = false; continue; }
        if (g_verbose && (status != p.status || locked != p.locked || res != p.res))
            gp_log("difficulty: placeholder %08X status %d locked %d resource %08X -> status %d locked %d resource %08X (mask %06X flags50 %08X, dead %d re-armed %d)\n",
                   p.node, p.status, p.locked, p.res, status, locked, res, sp_mask(p.node), RD32(p.node + 0x50), p.dead ? 1 : 0, p.rearmed ? 1 : 0);
        if (dump && p.dead) dump_node(p.rearmed ? "re-armed placeholder" : "dead placeholder", p.node);
        DWORD had = p.res;
        p.status = status; p.locked = locked; p.res = res;
        DWORD mask = sp_mask(p.node);
        if (p.candidate && !p.dead && !p.live_now && !res && (had || p.was_alive) && mask != 0xFFFFFFFF && !(mask & PH_MASK))
            on_died(p, gms, "its character is gone and the placeholder left the collect (spatial mask without 80h)");
        p.mask = mask;
        if (!p.live_now) p.was_alive = false;
    }
    g_live = live; if (live > g_live_max) g_live_max = live;
    for (Ph& p : g_p)   // forget placements not seen for 2 minutes unless one is owed
        if (p.used && gms - p.seen_ms > 120000 && !(p.dead && p.counted && !p.rearmed)) p.used = false;
    // bring one back when a whole reinforcement is due (accumulator) or [mayhem_heat] asks for a wave; one per pass (0.5 s),
    // so a wave arrives over a few seconds. Heat may allow a placement 2 or 3 comebacks and asks for placements above Ram
    // first (rooftops, balconies, catwalks: where it died, which is where it comes back).
    bool wave = g_hd.active && g_hd.wave_due > 0;
    int max_rearms = g_hd.active && g_hd.max_rearms > 1 ? g_hd.max_rearms : 1;
    if (g_rs.acc >= 1.0f || wave) {
        Ph* best = nullptr; float best_dist = 0.0f, best_dy = -1e9f;
        for (Ph& p : g_p) {
            // only a dead placement (Status 3) without a character, counted once, with comebacks left
            if (!p.used || !p.dead || p.res || !p.has_pos || !p.counted || p.rearms >= max_rearms) continue;
            if (p.status == 2 || p.mask == 0xFFFFFFFF || (p.mask & PH_MASK)) { if (g_verbose && !p.noted) { p.noted = true; gp_log("difficulty: placeholder %08X dead but status %d mask %06X: not re-armed\n", p.node, p.status, p.mask); } continue; }
            if (gp_now() - p.dead_t < (double)g_rc.delay_s) continue;   // real seconds (the tick does not run in menus)
            float dx = p.pos[0] - rp[0], dy = p.pos[1] - rp[1], dz = p.pos[2] - rp[2]; float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!df::distance_ok(dist, g_rc)) continue;
            int um = unmet_conditions(p.node);
            if (um) { if (!p.noted) { p.noted = true; gp_log("difficulty: placeholder %08X: %d mission death condition(s) not met yet: never re-armed\n", p.node, um); } continue; }
            // prefer: from above (heat levels 3+), then the fewest comebacks, then the farthest (they come at him)
            bool high = g_hd.prefer_high && dy >= g_hd.high_m, best_high = best && g_hd.prefer_high && best_dy >= g_hd.high_m;
            bool better = !best || (high != best_high ? high : (p.rearms != best->rearms ? p.rearms < best->rearms : dist > best_dist));
            if (better) { best = &p; best_dist = dist; best_dy = dy; }
        }
        if (best) {
            int has = 0, mx = 0;
            if (!pool_has_room(g_rc_keep, &has, &mx)) {   // the mission's own placements first
                if (g_verbose) gp_log("difficulty: reinforcement waits: pool %d / %d with %d slots kept for the mission\n", has, mx, g_rc_keep);
            } else if (wave) { heat_wave_spent(1); ++g_rs.rearmed; rearm(*best, best_dist, best_dy, "HEAT WAVE"); }
            else if (g_rs.take()) rearm(*best, best_dist, best_dy, g_on && g_level >= df::BRUTAL ? "BRUTAL" : "HEAT");
        }
    }
    if (g_debug_kill_s > 0.0f && now - g_last_kill >= g_debug_kill_s &&
        !(g_debug_kill_stop > 0.0f && g_debug_first_kill > 0.0 && now - g_debug_first_kill > g_debug_kill_stop)) {
        g_last_kill = now; if (g_debug_first_kill <= 0.0) g_debug_first_kill = now; debug_kill(rp);
    }
    if (g_verbose && now - g_last_diag >= 10.0) { g_last_diag = now; diag_characters(rp); diag_resource_ctrl(); }
    if (now - g_last_report >= 5.0) {
        g_last_report = now;
        if (found || g_rs.deaths)
            gp_log("difficulty: BRUTAL fight: %d live enemies near Ram (max %d), %d placeholders in 80 m; enemies seen %d, deaths %d, "
                   "reinforcements re-armed %d / arrived %d (= %d enemies in this fight), accumulator %.2f\n", live, g_live_max, found,
                   g_seen_total, g_rs.deaths, g_rs.rearmed, g_arrived, g_seen_total + g_arrived, g_rs.acc);
    }
}

// ================================================================================================ live-enemy cap
// written without the member-write notify (no save records it); the game rewrites its 6 / 8 at every block load, the
// tick puts the raised value back; leaving BRUTAL / PSYCHO restores the game's value
static void apply_cap(bool on) {
    DWORD d = char_resource_ctrl_data(); if (!d) return;
    __try {
        int cur = (int)RD32(d + 0x18);
        if (on) {
            if (cur != g_mx_written) g_mx_base = cur;          // the game's own value (block load) or ours lost
            int target = df::live_cap(g_max_live, g_mx_base, POOL_SHELLS, g_friend_mission, g_cap_with_friends);
            int bigger = bf_live_cap(difficulty_level(), g_mx_base);   // [bigger_fights]: more shells in the data, buddies keep theirs
            if (bigger > target) target = bigger;
            if (cur != target) {
                WR32(d + 0x18, target);
                gp_log("difficulty: live-enemy cap (CharacterResourceCtrl MaxAllocated) %d -> %d\n", cur, target);
            }
            g_mx_written = target;
        } else if (g_mx_written) {
            if (cur == g_mx_written && g_mx_base > 0) { WR32(d + 0x18, g_mx_base); gp_log("difficulty: live-enemy cap back to %d\n", g_mx_base); }
            g_mx_written = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================================================ mission log
// For "random mission fails" that the log could not explain otherwise: [difficulty] mission_log = 1 (default)
// writes, on every difficulty: the end of a mission with its result (mission controller data +38h Current_Mission, +48h
// Mission_Result, +ACh EndMissionCleanupCalledOnce: Command_End_Mission 0x790A20 sets them), Ram's deaths, and every death of a buddy / escort / friend / VIP within 80 m with who killed it (sentry
// +178h KillOwner, +174h Killer). Log lines only: nothing is written to the game.
struct Friend { DWORD root, ai, ph; int faction; bool used, dead; };
static Friend g_fr[16];
static int g_ml_mission = -2, g_ml_cleanup = -1, g_ml_result = -2; static bool g_ml_ram_dead = false; static double g_ml_last = 0.0;
static const char* who_is(DWORD node, char* buf, size_t n) {   // "RAM", "faction F character X" or "nobody"
    if (!node) return "nobody";
    if (node == ram_root()) return "RAM";
    int f = -1; __try { DWORD d = sdata(node); if (d && cls_of(node) == H_ROOT_CLS) f = (int)RD32(d + 0x18); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    sprintf_s(buf, n, f >= 0 ? "character %08X (faction %d)" : "%08X (not a character)", node, f);
    return buf;
}
static void mission_log(double now) {
    if (!g_mlog || now - g_ml_last < 0.25) return;
    g_ml_last = now;
    DWORD cd = gp_cache_data(); if (!cd) return;
    int cur = -1, result = -1, cleanup = -1;
    __try { DWORD mc = RD32(cd + C_MISSION_NODE); DWORD md = sdata(mc); if (md) { cur = (int)RD32(md + 0x38); result = (int)RD32(md + 0x48); cleanup = (int)RD32(md + 0xAC); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (cur != g_ml_mission) {
        if (g_ml_mission != -2) gp_log("mission log: mission %d -> %d\n", g_ml_mission, cur);
        g_ml_mission = cur; g_ml_cleanup = cleanup; memset(g_fr, 0, sizeof g_fr);
    }
    if (result != g_ml_result) {   // Command_End_Mission stores its result here (0 failure, 1 success)
        if (g_ml_result != -2) gp_log("mission log: mission %d Mission_Result %d -> %d (end-mission cleanup flag %d)\n", cur, g_ml_result, result, cleanup);
        g_ml_result = result;
    }
    if (cleanup != g_ml_cleanup) {
        if (cleanup == 1 && g_ml_cleanup == 0)
            gp_log("mission log: mission %d ENDED with result %d (%s)\n", cur, result, result == 1 ? "success" : result == 0 ? "FAILED" : "?");
        g_ml_cleanup = cleanup;
    }
    DWORD ram = ram_root(); if (!ram) return;
    float hl = 1.0f; bool ok = false;
    __try { DWORD rd = sdata(ram); DWORD ai = rd ? RD32(rd + 0x58) : 0; DWORD a = sdata(ai); if (a) { hl = RDF(a + 0x1C) - RDF(a + 0x20); ok = hl == hl; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (ok && hl <= 0.0f && !g_ml_ram_dead) { g_ml_ram_dead = true; gp_log("mission log: RAM DIED in mission %d (health left %.0f)\n", cur, hl); }
    else if (ok && hl > 0.0f) g_ml_ram_dead = false;
    // friends near Ram: buddy flag, escort sub type, friend factions (10 DEA, 18 Mexican buddies and their copies), VIPs
    float rp[3]; if (!get_pos(ram, rp)) return;
    DWORD L = query(rp, 80.0f, 0x40, 0);
    __try {
        for (int i = 0, n = list_count(L); L && i < n && i < 128; ++i) {
            DWORD r = list_at(L, i);
            if (!r || r == ram || cls_of(r) != H_ROOT_CLS) continue;
            DWORD rd = sdata(r); DWORD ai = rd ? RD32(rd + 0x58) : 0; DWORD d = sdata(ai);
            if (!d || cls_of(ai) != H_SENTRY_CLS) continue;
            int fac = (int)RD32(rd + 0x18), sbt = (int)RD32(d + 0x28); DWORD ph = RD32(rd + 0x24);
            bool buddy = RD32(d + 0xB8) != 0, vip = false;
            if (ph) { DWORD v = 0; Cur k = cur_save(); __try { ((Prop_t)(uintptr_t)P_DF_GETPROP)(ph, nullptr, 642, &v); } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; } cur_restore(k); vip = v != 0; }
            bool fr = buddy || sbt == 4 || fac == 10 || fac == 18 || fac == 29 || fac == 37 || vip;
            if (!fr) continue;
            int st = (int)RD32(d + 0x1D4); float hp = RDF(d + 0x2C), dmg = RDF(d + 0x30);
            Friend* f = nullptr;
            for (Friend& x : g_fr) if (x.used && x.root == r) { f = &x; break; }
            if (f && f->ph != ph) { f->used = false; f = nullptr; }   // the shell went to another placement
            if (!f) {
                bool alive = st != 28 && hp > dmg;
                if (!alive) continue;   // only deaths seen happen are logged
                for (Friend& x : g_fr) if (!x.used) { f = &x; break; }
                if (!f) continue;
                f->root = r; f->ai = ai; f->ph = ph; f->faction = fac; f->used = true; f->dead = false;
                gp_log("mission log: watching %s %08X (faction %d, sub type %d, hp %.0f, placeholder %08X) in mission %d\n",
                       vip ? "a VIP" : buddy ? "a buddy" : sbt == 4 ? "an escort" : "a friend", r, fac, sbt, hp, ph, cur);
                continue;
            }
            if (!f->dead && (st == 28 || (hp > 0.0f && dmg >= hp))) {
                f->dead = true;
                char b1[64], b2[64];
                gp_log("mission log: %s %08X (faction %d, placeholder %08X) DIED in mission %d: AI state %d, damage %.0f / %.0f, kill owner %s, killer %s\n",
                       vip ? "VIP" : buddy ? "buddy" : sbt == 4 ? "escort" : "friend", r, fac, ph, cur, st, dmg, hp,
                       who_is(RD32(d + 0x178), b1, sizeof b1), who_is(RD32(d + 0x174), b2, sizeof b2));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    query_free(L);
}

// ================================================================================================ tick
static double g_last_tick = 0.0;
void difficulty_tick(void* thread, double now) {
    if (!g_installed) return;
    mission_log(now);
    if (now - g_last_tick < 0.5) return;
    g_last_tick = now;
    DWORD cd = gp_cache_data(); if (!cd) return;
    DWORD setting = 0;
    DWORD sn = 0; __try { sn = RD32(cd + C_SETTINGS); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!sn || !get_member(sn, 1, &setting)) return;
    int level = g_force >= 0 ? g_force : df::level_from_setting((int)setting, g_extra);
    if ((int)setting != g_setting || level != g_level) {
        gp_log("difficulty: Setting_difficulty %d, extra %d -> level %s%s\n", (int)setting, g_extra, df::level_name(level),
               g_force >= 0 ? " (force_level from the ini)" : "");
        g_setting = (int)setting; g_level = level;
    }
    if (g_tuning && !g_tuning_refused) {
        if (!g_captured && g_capture_tries < 20) { ++g_capture_tries; g_captured = capture(); }
        if (g_captured && (g_applied != level || ((int)(now * 2) % 8) == 0)) apply(level);   // re-check every ~4 s
    }
    if (g_max_live > 0 || bf_cap_wanted()) apply_cap(level >= df::BRUTAL);
    g_hd = heat_drive();
    bool brutal = g_on && level >= df::BRUTAL;
    g_rc_eff = g_rc; g_rc_eff.share = (brutal ? df::level_share(level, g_rc) : 0.0f) + (g_hd.active ? g_hd.share_add : 0.0f);
    if (g_reinf && (brutal || g_hd.active)) reinf_tick(now);
    else if (g_rs.deaths) forget_all(g_heat_reinf ? "not BRUTAL and no heat" : "not BRUTAL");
}

// ================================================================================================ for other features
int difficulty_level() { return g_installed ? g_level : -1; }
bool difficulty_enemy_loco_moves() { return g_installed && g_level == df::PSYCHO; }   // PSYCHO's own feature: later work
bool difficulty_owns_ammo() { return g_installed && g_tuning && !g_tuning_refused; }   // [drops] writes the bands itself otherwise

// ================================================================================================ install
bool difficulty_install(bool tick_ok) {
    if (!g_on && !(g_heat_reinf && g_reinf)) return false;
    if (!g_on) {   // [difficulty] off, [mayhem_heat] on: only the reinforcement engine (no menu, no tuning, no cap)
        g_engine_only = true; g_menu = false; g_tuning = false; g_max_live = 0; g_force = -1;
        gp_log("difficulty: [difficulty] is off: the reinforcement engine runs for [mayhem_heat] only (no menu, no tuning, no live cap)\n");
    }
    if (!gp_ref_ok("SV_CLASSOBJ") || !gp_ref_ok("DF_SETTINGS") || !gp_ref_ok("DF_GET")) { gp_log("difficulty: reference bytes differ: OFF\n"); return false; }
    sidecar_path();
    if (!g_engine_only) sidecar_load();
    if (g_menu) {
        gp_menu_register(3, &menu_get, &menu_set, "difficulty");
        if (!gp_menu_install()) gp_log("difficulty: menu hooks not installed: no BRUTAL in the menu (ini force_level still works)\n");
    }
    if (g_tuning && !(gp_ref_ok("DF_DMGHARD") && gp_ref_ok("DF_DMGMOD") && gp_ref_ok("DF_BULLETDROP") && gp_ref_ok("DF_CHARLIST") &&
                      gp_ref_ok("DF_CLS_WSUB") && gp_ref_ok("DF_CLS_MSTAT"))) { gp_log("difficulty: tuning references differ: tuning OFF\n"); g_tuning = false; }
    if (g_reinf && !(gp_ref_ok("SV_QUERY_A") && gp_ref_ok("SV_QUERY_FREE") && gp_ref_ok("F_QUERY") && gp_ref_ok("F_LISTFREE") &&
                     gp_ref_ok("SV_CLS_ROOT") && gp_ref_ok("SV_CLS_SENTRY") && gp_ref_ok("SM_ROOT_AI") && gp_ref_ok("DF_CLS_PH") &&
                     gp_ref_ok("DF_PH_STATUS") && gp_ref_ok("DF_PH_LOCKED") && gp_ref_ok("DF_PH_RES") && gp_ref_ok("DF_PH_QUERY") &&
                     gp_ref_ok("DF_SPMASK_GET") && gp_ref_ok("DF_SPMASK_SET") && gp_ref_ok("DF_PH_KILLMASK") && gp_ref_ok("SV_SHOW_FN") && gp_ref_ok("F_SETFLAGS") &&
                     gp_ref_ok("DF_GETPOS") && gp_ref_ok("DF_SENDCMD"))) { gp_log("difficulty: reinforcement references differ: reinforcements OFF\n"); g_reinf = false; }
    if ((g_reinf || g_max_live) && !(gp_ref_ok("F_FINDCHILD") && gp_ref_ok("DF_BULLETDROP"))) { gp_log("difficulty: FindChild reference differs: pool budget / cap OFF\n"); g_max_live = 0; }
    if (g_max_live && !(gp_ref_ok("DF_RC_MAX6") && gp_ref_ok("DF_RC_MAX8"))) { gp_log("difficulty: resource cap references differ: max_live_enemies OFF\n"); g_max_live = 0; }
    g_setprop_ok = gp_ref_ok("DF_SETPROP") && gp_ref_ok("F_GETPROP");
    if (g_reinf && !g_setprop_ok) gp_log("difficulty: SetProperty reference differs: a reinforcement's stored damage is cleared on arrival only\n");
    if (!tick_ok && (g_tuning || g_reinf || g_force >= 0)) gp_log("difficulty: no input tick: tuning / reinforcements OFF (the menu still stores the level)\n");
    g_installed = true;
    gp_log("difficulty: on (menu %d, tuning %d, reinforcements %d share %.2f BRUTAL / %.3f PSYCHO, %.0f..%.0f m, delay %.1f s, live cap %d%s)\n",
           g_menu ? 1 : 0, g_tuning ? 1 : 0, g_reinf ? 1 : 0, g_rc.share, g_rc.share_psycho, g_rc.min_distance, g_rc.max_distance, g_rc.delay_s, g_max_live,
           g_debug_kill_s > 0 ? ", DEBUG KILLS" : "");
    return true;
}

// ---- test host only
static int __cdecl TestFlag() { return g_extra; }
static void __cdecl TestSidecar(const char* path) { strcpy_s(g_sidecar, path); }   // the host writes into its own folder
void* difficulty_test_stub(const char* name) {
    if (!strcmp(name, "df_flag")) return (void*)&TestFlag;
    if (!strcmp(name, "df_set_sidecar")) return (void*)&TestSidecar;
    return nullptr;
}
