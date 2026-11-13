// smart_game.cpp - game glue of "Smart AI" ([smart_ai] in TODGameplay.ini). Compiled into TODGameplay.dll by
// dll/build.bat. Rules as in dll/src/gp_game.cpp: off unless [smart_ai] enabled = 1, every patched site and every
// referenced address byte-checked first (sites.h SM_* rows), every game read SEH-guarded, no engine classes, nothing
// patched when off.
//
// What it does (hooks: dll/HOOKS.md "Smart AI"):
//   * one call retarget, SM_INIT 0x5E7C28: the character_sentry_ai reset handler's call of the AI init 0x5D4190. After
//     the game has copied the placeholder + behaviour row into the enemy, SmartInitWrap gives it a personality by
//     writing the game's own members (state 20 weights and times, StopDistance, Retreat_Range, ShootDodge_Min_dist, a
//     one-shot Wait_until_Aggressive) and registers it;
//   * the input tick (the player's input procedure, script context, player in control) runs every 100 ms: it clears
//     the one-shot reaction delay, hands out approach tokens (the game's own DontHunt member) and raises / restores
//     CS_HealthLeft / CS_Change_for from the fear of nearby ally deaths. The game's state machine does the rest.
// Every member write is preceded by the game's member-write notify (touch slot), as the game's own code does. A member
// that something else changed after we wrote it is never written again (the feature backs off for that enemy).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../dll/src/gp_glue.h"
#include "smart_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v)  (*(volatile float*)(uintptr_t)(a) = (float)(v))

// ================================================================================================ game addresses
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764;
static DWORD R_SM_INIT = 0x5D4190;                 // the AI init the call site called (check_sites.py: SM_INIT target)
static DWORD P_SM_TOUCH = 0x86B4B0, P_SM_GETPOS = 0x88CA80;   // member-write notify, Node::GetWorldPosition
static const DWORD H_SENTRY_CLS = 0x5C90F0;        // character_sentry_ai class handler (REF SV_CLS_SENTRY)
static const DWORD C_MISSION_NODE = 0x25C, C_TIMESTAMP = 0x274;   // cache data: mission control, game-ms clock node
// character_sentry_ai data (REF rows SM_*; slot = member index, proven by the init's own touch + store pairs)
static const DWORD S_SUBTYPE = 0x28, S_HEALTH = 0x2C, S_DAMAGE = 0x30, S_ISMELEE = 0xAC, S_ISBUDDY = 0xB8, S_MORALE = 0xD8,
                   S_DONTHUNT = 0xF0, S_STATE = 0x1D4, S_COMBAT = 0x234;
static const int   SLOT_DONTHUNT = 0x33;
// node: +18h data, +40h parent (the character_root); character_root data: +14h Sub_Type, +18h FactionID
// character_combat_ctrl data: +8 CurrentTarget, +10h CurrentTargetSqrDist
struct FieldDef { int field; DWORD off; int slot; };
static const FieldDef SM_FIELDS[] = {   // parsed by smart_ai/code/tools/check_smart.py (proves each pair in the exe)
    {sm::F_WAIT_AGGR, 0x0CC, 0x2A},
    {sm::F_RETREAT, 0x048, 0x09},
    {sm::F_STRAFE_FROM, 0x04C, 0x0A},
    {sm::F_STRAFE_TO, 0x050, 0x0B},
    {sm::F_CROUCH_FROM, 0x054, 0x0C},
    {sm::F_CROUCH_TO, 0x058, 0x0D},
    {sm::F_WAIT_FROM, 0x060, 0x0F},
    {sm::F_WAIT_TO, 0x064, 0x10},
    {sm::F_PRO_CROUCH, 0x088, 0x19},
    {sm::F_PRO_STRAFE, 0x08C, 0x1A},
    {sm::F_PRO_STRAFE_CROUCH, 0x090, 0x1B},
    {sm::F_PRO_SD, 0x094, 0x1C},
    {sm::F_PRO_SD_STRAFE_CROUCH, 0x098, 0x1D},
    {sm::F_PRO_SD_CROUCH, 0x09C, 0x1E},
    {sm::F_PRO_CROUCH_SD_STRAFE, 0x0A0, 0x1F},
    {sm::F_PRO_STRAFE_SD_STRAFE, 0x0A4, 0x20},
    {sm::F_STOPDIST, 0x0FC, 0x36},
    {sm::F_SD_MIN, 0x0BC, 0x26},
    {sm::F_CS_HEALTHLEFT, 0x068, 0x11},
    {sm::F_CS_CHANGE, 0x06C, 0x12},
};
static const FieldDef* fdef(int f) { for (const FieldDef& d : SM_FIELDS) if (d.field == f) return &d; return nullptr; }

typedef void(__cdecl* Init_t)(void* thread, int* out);
typedef void(__fastcall* SmTouch_t)(DWORD node, void* edx, int slot);    // thiscall, ret 4
typedef void(__fastcall* SmVec_t)(DWORD node, void* edx, float* v);      // thiscall, ret 4

// ================================================================================================ config
static sm::PersonaCfg g_pc; static sm::SquadCfg g_qc; static sm::MoraleCfg g_mc;
static bool g_master = false, g_verbose = true, g_log_states = false, g_installed = false, g_tick_ok = false;
static float g_period = 0.1f, g_discover_s = 1.0f, g_discover_m = 60.0f;
static float fcfg(const char* k, float d) { return gp_ini().getf("smart_ai", k, d); }
static bool  bcfg(const char* k, bool d) { return gp_ini().geti("smart_ai", k, d ? 1 : 0) != 0; }
static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
void smart_load_config() {
    g_master = bcfg("enabled", false);
    g_verbose = bcfg("log_spawns", true);
    g_log_states = bcfg("log_states", false);
    sm::PersonaCfg p; sm::SquadCfg q; sm::MoraleCfg m;
    p.enabled = g_master && bcfg("personality", true);
    p.reaction = bcfg("reaction_stagger", true);
    p.react_min = clampf(fcfg("reaction_min", p.react_min), 0.0f, 5.0f);
    p.react_max = clampf(fcfg("reaction_max", p.react_max), p.react_min, 5.0f);
    p.assault_share = clampf(fcfg("assault_share", p.assault_share), 0.0f, 1.0f);
    p.cautious_share = clampf(fcfg("cautious_share", p.cautious_share), 0.0f, 1.0f - p.assault_share);
    p.jitter = clampf(fcfg("jitter", p.jitter), 0.0f, 0.6f);
    p.assault_range = clampf(fcfg("assault_range", p.assault_range), 0.3f, 2.0f);
    p.cautious_range = clampf(fcfg("cautious_range", p.cautious_range), 0.3f, 2.5f);
    p.dodge_bonus = clampf(fcfg("shootdodge_bonus", p.dodge_bonus), 0.0f, 30.0f);
    float boss = clampf(fcfg("boss_health", 1000.0f), 1.0f, 1e6f);
    p.boss_health = boss;
    p.skip_factions = sm::parse_factions(gp_ini().get("smart_ai", "skip_factions"), sm::DEFAULT_SKIP_FACTIONS);
    q.enabled = g_master && bcfg("squad", true);
    q.max_advancing = (int)clampf((float)gp_ini().geti("smart_ai", "max_advancing", q.max_advancing), 1.0f, 8.0f);
    q.min_squad = (int)clampf((float)gp_ini().geti("smart_ai", "min_squad", q.min_squad), 2.0f, 16.0f);
    q.token_s = clampf(fcfg("token_seconds", q.token_s), 1.0f, 60.0f);
    q.cooldown_s = clampf(fcfg("token_cooldown", q.cooldown_s), 0.0f, 60.0f);
    q.hold_max_m = clampf(fcfg("hold_max_distance", q.hold_max_m), 5.0f, 70.0f);
    q.stuck_s = clampf(fcfg("hold_stuck_seconds", q.stuck_s), 1.0f, 60.0f);
    m.enabled = g_master && bcfg("morale", true);
    m.radius_m = clampf(fcfg("morale_radius", m.radius_m), 1.0f, 70.0f);
    m.per_death = clampf(fcfg("fear_per_death", m.per_death), 0.0f, 1.0f);
    m.last_man = clampf(fcfg("fear_last_man", m.last_man), 0.0f, 1.0f);
    m.decay_per_s = clampf(fcfg("fear_decay_per_second", m.decay_per_s), 0.0f, 1.0f);
    m.health_bonus = clampf(fcfg("fear_health_bonus", m.health_bonus), 0.0f, 200.0f);
    m.max_chance = clampf(fcfg("fear_flee_chance", m.max_chance), 0.0f, 100.0f);
    m.boss_health = boss;
    m.skip_factions = p.skip_factions;
    g_pc = p; g_qc = q; g_mc = m;
    g_period = clampf(fcfg("tick_seconds", 0.1f), 0.02f, 1.0f);
    g_discover_s = clampf(fcfg("discover_seconds", 1.0f), 0.2f, 10.0f);
    g_discover_m = clampf(fcfg("discover_radius", 60.0f), 10.0f, 150.0f);
}
bool smart_enabled() { return g_master && (g_pc.enabled || g_qc.enabled || g_mc.enabled); }
// the tick also adopts enemies restored from a save (no init for those), so every feature wants it
bool smart_wants_tick() { return g_qc.enabled || g_mc.enabled || g_pc.enabled; }

// ================================================================================================ memory helpers
static volatile LONG g_test = 0;                  // test host: clock / mission / play mode come from TestEnv
static uint32_t g_test_ms = 0; static int g_test_mission = 0, g_test_pm = 1;
static DWORD sdata(DWORD node) { __try { return node ? RD32(node + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_of(DWORD node) {   // script class object (node -> +4 type -> +28h) -> +60h handler (REF SV_CLASSOBJ)
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void touch(DWORD node, int slot) { __try { ((SmTouch_t)(uintptr_t)P_SM_TOUCH)(node, nullptr, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static bool write_f(DWORD node, DWORD d, int slot, DWORD off, float v) {
    __try { touch(node, slot); WRF(d + off, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool write_i(DWORD node, DWORD d, int slot, DWORD off, DWORD v) {
    __try { touch(node, slot); WR32(d + off, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool get_pos(DWORD node, float* p) {
    if (!node) return false;
    float v[3] = {0, 0, 0};
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    bool ok = true;
    __try { ((SmVec_t)(uintptr_t)P_SM_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
    if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
    if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
    if (!ok || !(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    return true;
}
static bool game_ms(uint32_t* ms) {   // cache data +274h TIMESTAMP node -> data +4 (game ms; state 13's last_init_time clock)
    if (g_test) { *ms = g_test_ms; return true; }
    DWORD cd = gp_cache_data(); if (!cd) return false;
    __try { DWORD n = RD32(cd + C_TIMESTAMP); DWORD d = n ? RD32(n + 0x18) : 0; if (!d) return false; *ms = RD32(d + 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int mission_now() {
    if (g_test) return g_test_mission;
    DWORD cd = gp_cache_data(); if (!cd) return -1;
    __try { DWORD m = RD32(cd + C_MISSION_NODE); DWORD md = m ? RD32(m + 0x18) : 0; return md ? (int)RD32(md + 0x38) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ================================================================================================ the registry
enum { MAXE = 32 };
struct Ent {
    bool  used, dead, eligible, sq_ok, mo_ok, react_set, has_pos;
    DWORD node, data, root;
    int   role, faction, morale, state;
    float react_val;
    DWORD dh_orig, dh_expect;                    // DontHunt at spawn / the value we last wrote (or saw at spawn)
    float hl0, cf0, hl_expect, cf_expect;        // CS_HealthLeft / CS_Change_for at spawn and as last written
    float pos[3];
    sm::Fear fear; sm::SquadMem sq;
    uint32_t serial;
};
static Ent g_e[MAXE];
static uint32_t g_serial = 0, g_spawns = 0, g_last_gms = 0;
static int g_mission = -2;
static double g_last_tick = 0.0;
static DWORD g_writes = 0, g_backoffs = 0, g_deaths = 0;

static Ent* reg_slot(DWORD node) {
    for (Ent& e : g_e) if (e.used && e.node == node) return &e;
    for (Ent& e : g_e) if (!e.used) return &e;
    Ent* old = nullptr;   // full: reuse a dead one, else the oldest
    for (Ent& e : g_e) if (e.dead && (!old || e.serial < old->serial)) old = &e;
    if (!old) for (Ent& e : g_e) if (!old || e.serial < old->serial) old = &e;
    return old;
}
static bool read_sentry(DWORD node, DWORD d, DWORD root, sm::Sentry* s) {
    __try {
        for (const FieldDef& f : SM_FIELDS) s->v[f.field] = RDF(d + f.off);
        s->health = RDF(d + S_HEALTH);
        s->morale = (int)RD32(d + S_MORALE);
        s->is_buddy = (int)RD32(d + S_ISBUDDY);
        s->subtype = (int)RD32(d + S_SUBTYPE);
        s->dont_hunt = (int)RD32(d + S_DONTHUNT);
        DWORD rd = root ? RD32(root + 0x18) : 0;
        s->root_subtype = rd ? (int)RD32(rd + 0x14) : 0;
        s->faction = rd ? (int)RD32(rd + 0x18) : -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ personalities written
// Kept across forget_all (mission change, reload, clock reset): an enemy the discovery finds again (same node, same
// data, no init since) is re-registered WITHOUT new writes. Without this the persona was computed a
// second time on top of the first one ("found ... dodge 8.0->14.6 pro 8->15"): values grew with every re-adoption. A
// member the game changed meanwhile (the test-copy run r1 saw StopDistance 8.9 -> 8.0) stays the game's.
enum { MAXA = 64 };
struct Applied { DWORD node, data; uint32_t changed; int role; float wrote[sm::F_COUNT]; };
static Applied g_ap[MAXA]; static int g_ap_next = 0;
static Applied* ap_find(DWORD node, DWORD d) {
    for (Applied& a : g_ap) if (a.node == node && a.data == d && a.node) return &a;
    return nullptr;
}
static Applied* ap_slot(DWORD node, DWORD d) {
    if (Applied* a = ap_find(node, d)) return a;
    Applied* a = &g_ap[g_ap_next]; g_ap_next = (g_ap_next + 1) % MAXA;
    return a;
}

// ================================================================================================ spawn / adoption
// late = found by the tick's discovery (an enemy restored from a save: the game does not run the init for those, as
// seen in test logs); it gets the same personality, the reaction delay only while it is still passive.
static bool on_spawn(DWORD node, bool late) {
    if (cls_of(node) != H_SENTRY_CLS) return false;
    DWORD d = sdata(node); if (!d) return false;
    DWORD root = 0; __try { root = RD32(node + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    int st = 1; __try { st = (int)RD32(d + S_STATE); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (late && (st == -1 || st == 28)) return false;                   // in the pool / dead: the init will come
    bool passive = !late || (st >= 4 && st <= 8);
    sm::Sentry s;
    if (!read_sentry(node, d, root, &s)) return false;
    ++g_spawns;
    sm::Persona p;
    const Applied* had = late ? ap_find(node, d) : nullptr;
    bool kept = had && had->changed && sm::eligible(s, g_pc.boss_health, g_pc.skip_factions);
    if (kept) {                                                         // found again: personalized already, no second one
        p.eligible = true; p.role = had->role; p.changed = 0;
        for (int f = 0; f < sm::F_COUNT; ++f) p.v[f] = s.v[f];
    } else {
        sm::Rng rng(sm::mix((uint32_t)node, g_spawns * 2654435761u ^ GetTickCount()));
        p = sm::make_persona(g_pc, s, rng, g_tick_ok && g_pc.reaction && passive);
    }
    int nw = 0;
    if (p.eligible && p.changed) {
        Applied* a = ap_slot(node, d);
        a->node = node; a->data = d; a->changed = 0; a->role = p.role;
        for (int f = 0; f < sm::F_COUNT; ++f) {
            a->wrote[f] = p.v[f];
            if (!(p.changed & (1u << f))) continue;
            const FieldDef* fd = fdef(f);
            if (fd && write_f(node, d, fd->slot, fd->off, p.v[f])) { ++nw; ++g_writes; a->changed |= 1u << f; }
        }
    } else if (!kept) {
        if (Applied* a = ap_find(node, d)) a->node = 0;                 // nothing of ours in it any more
    }
    bool elig = sm::eligible(s, g_pc.boss_health, g_pc.skip_factions);
    if (!elig && !p.eligible) { if (g_verbose) gp_log("smart: %s %08X not managed (buddy / civilian / special / boss; faction %d, health %.0f)\n", late ? "found" : "spawn", node, s.faction, s.health); }
    Ent* e = reg_slot(node);
    if (!e) return false;
    memset(e, 0, sizeof *e);
    e->used = true; e->node = node; e->data = d; e->root = root; e->serial = ++g_serial;
    e->eligible = elig; e->role = p.role; e->faction = s.faction; e->morale = s.morale; e->state = -1;
    e->react_set = (p.changed & (1u << sm::F_WAIT_AGGR)) != 0; e->react_val = p.v[sm::F_WAIT_AGGR];
    if (kept && (had->changed & (1u << sm::F_WAIT_AGGR)) && had->wrote[sm::F_WAIT_AGGR] > 0.0f &&
        fabsf(s.v[sm::F_WAIT_AGGR] - had->wrote[sm::F_WAIT_AGGR]) < 1e-4f) {   // our unused delay: the tick still clears it
        e->react_set = true; e->react_val = had->wrote[sm::F_WAIT_AGGR];
    }
    e->dh_orig = e->dh_expect = (DWORD)s.dont_hunt;
    e->sq_ok = elig && s.dont_hunt == 0 && s.morale != 2;          // authored holders / stand-fast keep their design
    e->hl0 = e->hl_expect = s.v[sm::F_CS_HEALTHLEFT]; e->cf0 = e->cf_expect = s.v[sm::F_CS_CHANGE];
    e->mo_ok = elig && s.morale == 1;                              // 0 flees anyway, 2 never
    if (g_verbose && kept)
        gp_log("smart: found %08X %-8s faction %d hp %.0f: personalized before (re-registered, 0 writes)\n",
               node, sm::role_name(p.role), s.faction, s.health);
    else if (g_verbose && p.eligible)
        gp_log("smart: %s %08X %-8s faction %d hp %.0f: stop %.1f->%.1f retreat %.1f->%.1f react %.2f dodge %.1f->%.1f "
               "pro %.0f->%.0f wait x%.2f (%d writes)\n", late ? "found" : "spawn", node, sm::role_name(p.role), s.faction, s.health,
               s.v[sm::F_STOPDIST], p.v[sm::F_STOPDIST], s.v[sm::F_RETREAT], p.v[sm::F_RETREAT], p.v[sm::F_WAIT_AGGR],
               s.v[sm::F_PRO_SD], p.v[sm::F_PRO_SD], sm::pro_sum(s.v), sm::pro_sum(p.v),
               s.v[sm::F_WAIT_TO] > 0 ? p.v[sm::F_WAIT_TO] / s.v[sm::F_WAIT_TO] : 1.0f, nw);
    return true;
}

// call site 0x5E7C28 in the reset handler 0x5E7BE0: push &out; push thread; mov [0xA3B75C], esi; call 0x5D4190; then
// add esp, 8 and a test of out. cdecl with the same two arguments: the original first, then the personality.
static void __cdecl SmartInitWrap(void* thread, int* out) {
    DWORD node = RD32(A_CUR_NODE);    // the sentry node (the handler just stored it there)
    ((Init_t)(uintptr_t)R_SM_INIT)(thread, out);
    int ok = 0; __try { ok = out ? *out : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) return;
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    __try { on_spawn(node, false); } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("smart: spawn %08X: exception, left as the game made it\n", node); }
    if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
    if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
    if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
}

// ================================================================================================ discovery
// Enemies restored from a save never pass the init. Every discover_seconds the tick asks the game's spatial query (the
// Tornado's: quadtree [0xA3B578], mask 40h = characters, REF SV_QUERY_A / SV_QUERY_FREE) for characters within
// discover_radius of Ram, and adopts every live character_sentry_ai it does not know yet.
static const DWORD A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_PLAYER_HOLDER = 0xA3B590;
static DWORD P_SM_QUERY = 0x486950, P_SM_LISTFREE = 0x8718D0;
static const DWORD H_ROOT_CLS = 0x5C3060;                     // character_root class handler (REF SV_CLS_ROOT)
typedef void(__fastcall* SmQuery_t)(DWORD qt, void* edx, void* q);       // thiscall, ret 4
typedef void(__fastcall* SmFree_t)(DWORD type, void* edx, DWORD* plist); // thiscall, ret 4
static double g_last_discover = -100.0;
static DWORD g_found = 0;
static DWORD ram_root() {   // [0xA3B590] ->+18h ->+84h (REF SV_CAM_RELEASE)
    __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool known(DWORD node, DWORD d) {
    for (Ent& e : g_e) if (e.used && e.node == node && e.data == d && !e.dead) return true;
    return false;
}
static void discover() {
    DWORD ram = ram_root(); float rp[3];
    if (!ram || !get_pos(ram, rp)) return;
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp[0], rp[1], rp[2], g_discover_m, 0x40, 0};
    DWORD qt = 0; __try { qt = RD32(A_QUADTREE); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!qt) return;
    __try { ((SmQuery_t)(uintptr_t)P_SM_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    DWORD L = q.out;
    if (!L) return;
    __try {
        DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0;   // list stride (the game divides by it)
        DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
        int n = k ? (int)(RD32(L + 8) / k) : 0;
        for (int i = 0; i < n && i < 256 && el; ++i) {
            DWORD r = RD32(el + (DWORD)i * 4 * k);
            if (!r || r == ram || cls_of(r) != H_ROOT_CLS) continue;
            DWORD rd = sdata(r); DWORD ai = rd ? RD32(rd + 0x58) : 0;   // root data +58h AICtrl (REF SM_ROOT_AI)
            if (!ai || cls_of(ai) != H_SENTRY_CLS) continue;
            DWORD d = sdata(ai);
            if (!d || known(ai, d)) continue;
            if (on_spawn(ai, true)) ++g_found;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((SmFree_t)(uintptr_t)P_SM_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================================================ tick (input tick, script context)
// Paused (the menu, where the game saves): every DontHunt / CS value the tick wrote goes back to the row value, so a
// save holds what the game made; the first tick after the pause applies them again.
static bool g_suspended = false;
static void release_all() {
    for (Ent& e : g_e) {
        if (!e.used || e.dead) continue;
        __try {
            if (e.sq_ok && e.dh_expect != e.dh_orig && RD32(e.data + S_DONTHUNT) == e.dh_expect &&
                write_i(e.node, e.data, SLOT_DONTHUNT, S_DONTHUNT, e.dh_orig)) { e.dh_expect = e.dh_orig; e.sq.holding = false; e.sq.token = false; ++g_writes; }
            if (e.mo_ok && (e.hl_expect != e.hl0 || e.cf_expect != e.cf0) &&
                fabsf(RDF(e.data + 0x68) - e.hl_expect) < 1e-3f && fabsf(RDF(e.data + 0x6C) - e.cf_expect) < 1e-3f) {
                if (write_f(e.node, e.data, 0x11, 0x68, e.hl0)) e.hl_expect = e.hl0;
                if (write_f(e.node, e.data, 0x12, 0x6C, e.cf0)) e.cf_expect = e.cf0;
                g_writes += 2;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static void forget_all(const char* why) {
    int n = 0; for (Ent& e : g_e) if (e.used) ++n;
    if (n) gp_log("smart: %s: %d registered enemies forgotten (nothing written)\n", why, n);
    memset(g_e, 0, sizeof g_e);
}
static void tick_body(uint32_t gms) {
    float gdt = (g_last_gms && gms >= g_last_gms) ? (float)(gms - g_last_gms) / 1000.0f : 0.0f;
    if (gdt > 1.0f) gdt = 1.0f;
    g_last_gms = gms;
    sm::SquadIn in[MAXE]; bool hold[MAXE]; sm::SquadMem mem[MAXE]; int map[MAXE]; int n = 0;
    float deaths[MAXE][3]; int dfac[MAXE]; int nd = 0;
    bool alive[MAXE]; memset(alive, 0, sizeof alive);
    // ---- read
    for (int i = 0; i < MAXE; ++i) {
        Ent& e = g_e[i];
        if (!e.used) continue;
        if (cls_of(e.node) != H_SENTRY_CLS || sdata(e.node) != e.data) { e.used = false; continue; }   // gone / reused
        if (e.dead) continue;
        DWORD d = e.data; int st = -1; float hp = 0, dmg = 0; DWORD tgt = 0; float sqr = 0; int melee = 0; DWORD dh = 0;
        float hl = 0, cf = 0, wa = 0;
        __try {
            st = (int)RD32(d + S_STATE); hp = RDF(d + S_HEALTH); dmg = RDF(d + S_DAMAGE); melee = (int)RD32(d + S_ISMELEE);
            dh = RD32(d + S_DONTHUNT); hl = RDF(d + 0x68); cf = RDF(d + 0x6C); wa = RDF(d + 0xCC);
            DWORD cc = RD32(d + S_COMBAT); DWORD cd = cc ? RD32(cc + 0x18) : 0;
            if (cd) { tgt = RD32(cd + 8); sqr = RDF(cd + 0x10); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { e.used = false; continue; }
        if (g_log_states && st != e.state)
            gp_log("smart: %08X %-8s state %d -> %d (target %s %.1f m, DontHunt %u, fear %.2f)\n", e.node, sm::role_name(e.role), e.state, st,
                   tgt ? "yes" : "no", sqr > 0.0f ? sqrtf(sqr) : 0.0f, dh, e.fear.f);
        e.state = st;
        if (st == -1) { e.used = false; continue; }   // deactivated (back in the pool): forget, no event
        bool live = st != 28 && hp - dmg > 0.0f;
        if (!live) {
            e.dead = true; ++g_deaths;
            if (e.has_pos && nd < MAXE) { memcpy(deaths[nd], e.pos, sizeof e.pos); dfac[nd] = e.faction; ++nd; }
            continue;
        }
        alive[i] = true;
        if (g_mc.enabled && e.mo_ok) e.has_pos = get_pos(e.root, e.pos) || e.has_pos;
        // one-shot reaction delay: cleared once the enemy is past state 14 (and nobody else changed it)
        if (e.react_set && sm::reaction_used(st)) {
            const FieldDef* fd = fdef(sm::F_WAIT_AGGR);
            if (fabsf(wa - e.react_val) < 1e-4f && fd && write_f(e.node, d, fd->slot, fd->off, 0.0f)) ++g_writes;
            e.react_set = false;
        }
        // back off when someone else (mounted gun, mission script, rewind) changed what we manage
        if (e.sq_ok && dh != e.dh_expect) { e.sq_ok = false; ++g_backoffs; gp_log("smart: %08X DontHunt changed by the game (%u): squad leaves it alone\n", e.node, dh); }
        if (e.mo_ok && (fabsf(hl - e.hl_expect) > 1e-3f || fabsf(cf - e.cf_expect) > 1e-3f)) { e.mo_ok = false; ++g_backoffs; gp_log("smart: %08X CS values changed by the game: morale leaves it alone\n", e.node); }
        if (n < MAXE) {
            sm::SquadIn& s = in[n];
            s.managed = e.sq_ok; s.engaged = sm::squad_engaged_state(st) && tgt != 0; s.ranged = melee == 0;
            s.state = st; s.target = (uint32_t)tgt; s.target_sqr = sqr;
            mem[n] = e.sq; map[n] = i; ++n;
        }
    }
    // ---- morale: ally deaths nearby, the last man, fading fear
    if (g_mc.enabled) {
        float r2 = g_mc.radius_m * g_mc.radius_m;
        for (int k = 0; k < nd; ++k) {
            int hit = 0;
            for (int i = 0; i < MAXE; ++i) {
                Ent& e = g_e[i];
                if (!alive[i] || !e.mo_ok || !e.has_pos || e.faction != dfac[k]) continue;
                if (sm::dist2(e.pos, deaths[k]) > r2) continue;
                sm::fear_add(e.fear, g_mc.per_death); ++hit;
                int allies = 0;
                for (int j = 0; j < MAXE; ++j)
                    if (j != i && alive[j] && g_e[j].faction == e.faction && g_e[j].has_pos && sm::squad_engaged_state(g_e[j].state) &&
                        sm::dist2(g_e[j].pos, e.pos) <= 4.0f * r2) ++allies;
                if (!allies) sm::fear_add(e.fear, g_mc.last_man);
            }
            if (hit) gp_log("smart: an ally died at (%.1f %.1f %.1f): %d enem%s within %.0f m afraid\n", deaths[k][0], deaths[k][1], deaths[k][2], hit, hit == 1 ? "y" : "ies", g_mc.radius_m);
        }
        for (int i = 0; i < MAXE; ++i) {
            Ent& e = g_e[i];
            if (!alive[i] || !e.mo_ok) continue;
            sm::fear_decay(e.fear, g_mc, gdt);
            float hl, cf; sm::morale_values(g_mc, e.hl0, e.cf0, e.fear.f, &hl, &cf);
            bool restore = e.fear.f <= 0.0f && (e.hl_expect != e.hl0 || e.cf_expect != e.cf0);
            if (restore || fabsf(hl - e.hl_expect) >= 0.5f || fabsf(cf - e.cf_expect) >= 0.5f) {
                if (write_f(e.node, e.data, 0x11, 0x68, hl)) { e.hl_expect = hl; ++g_writes; }
                if (write_f(e.node, e.data, 0x12, 0x6C, cf)) { e.cf_expect = cf; ++g_writes; }
            }
        }
    }
    // ---- squad: approach tokens through DontHunt
    if (g_qc.enabled && n) {
        int holders = sm::squad_assign(g_qc, in, mem, hold, n, gms);
        static int last_holders = -1;
        for (int k = 0; k < n; ++k) {
            Ent& e = g_e[map[k]];
            e.sq = mem[k];
            if (!e.sq_ok) continue;
            DWORD want = hold[k] ? 1u : e.dh_orig;
            if (want != e.dh_expect && write_i(e.node, e.data, SLOT_DONTHUNT, S_DONTHUNT, want)) { e.dh_expect = want; ++g_writes; }
        }
        if (holders != last_holders) { gp_log("smart: squad: %d holding (DontHunt), max %d advancing per target\n", holders, g_qc.max_advancing); last_holders = holders; }
    }
}
void smart_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed || !g_tick_ok) return;
    if (now - g_last_tick < g_period && now >= g_last_tick) return;
    g_last_tick = now;
    int pm = g_test ? g_test_pm : gp_playmode();
    if (pm == 2) {                                  // paused: put our values back once (a save sees the game's own)
        if (!g_suspended) {
            DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
            __try { release_all(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
            if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
            if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
            g_suspended = true;
        }
        return;
    }
    g_suspended = false;
    if (pm == 3) return;                            // rewinding: the game restores its own records
    uint32_t gms;
    if (!game_ms(&gms)) return;
    int mission = mission_now();
    if (mission != g_mission || (g_last_gms && gms + 2000 < g_last_gms)) {   // new mission, reload or clock reset
        if (g_mission != -2) forget_all(mission != g_mission ? "mission changed" : "game clock went back");
        g_mission = mission; g_last_gms = 0;
    }
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    if (now - g_last_discover >= g_discover_s || now < g_last_discover) {
        g_last_discover = now;
        __try { discover(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try { tick_body(gms); } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("smart: tick exception: registry cleared\n"); memset(g_e, 0, sizeof g_e); }
    if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
    if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
    if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
}

// ================================================================================================ install
static const char* const SM_REFS[] = {   // check_smart.py: every SM_ REF row of sites.h is listed here
    "F_TOUCH", "F_GETPOS", "SV_CLASSOBJ", "SV_CLS_SENTRY", "G_MISSION", "G_MISSION_CM",
    "SM_INIT_PROLOGUE", "SM_INIT_OUT",
    "SM_SLOT_01", "SM_SLOT_02", "SM_SLOT_03", "SM_SLOT_09", "SM_SLOT_0A", "SM_SLOT_0B", "SM_SLOT_0C", "SM_SLOT_0D",
    "SM_SLOT_0F", "SM_SLOT_10", "SM_SLOT_11", "SM_SLOT_12", "SM_SLOT_19", "SM_SLOT_1A", "SM_SLOT_1B", "SM_SLOT_1C",
    "SM_SLOT_1D", "SM_SLOT_1E", "SM_SLOT_1F", "SM_SLOT_20", "SM_SLOT_22", "SM_SLOT_25", "SM_SLOT_26", "SM_SLOT_2A",
    "SM_SLOT_2D", "SM_SLOT_33", "SM_SLOT_36",
    "SM_RD_WAITAGGR", "SM_RD_WAIT14", "SM_RD_DONTHUNT13", "SM_RD_DONTHUNT_LOST", "SM_RD_MORALE", "SM_RD_CS", "SM_RD_CF",
    "SM_RD_COMBAT", "SM_RD_STATE", "SM_RD_ROOT", "SM_RD_FACTION", "SM_RD_RETREAT22", "SM_CLOCK",
    "SM_ROOT_AI", "SV_CLS_ROOT", "SV_QUERY_A", "SV_QUERY_FREE", "F_QUERY", "F_LISTFREE", "SV_CAM_RELEASE",
};
bool smart_install(bool tick_ok) {
    g_installed = false;
    if (!smart_enabled()) return false;
    for (const char* r : SM_REFS) if (!gp_ref_ok(r)) { gp_log("smart: reference %s differs: OFF\n", r); return false; }
    g_tick_ok = tick_ok;
    if (!tick_ok) {
        if (g_qc.enabled || g_mc.enabled || g_pc.reaction) gp_log("smart: the input tick is not installed: squad, morale and the reaction stagger OFF\n");
        g_qc.enabled = g_mc.enabled = false;
        if (!g_pc.enabled) return false;
    }
    if (!gp_site_ok("SM_INIT")) { gp_log("smart: the AI init call site differs (another mod?): OFF\n"); return false; }
    if (!gp_patch_call("SM_INIT", (void*)&SmartInitWrap)) { gp_log("smart: patching SM_INIT failed: OFF\n"); return false; }
    g_installed = true;
    gp_log("smart: installed - personality %d (reaction %d), squad %d (max %d advancing, from %d), morale %d (radius %.0f m, fear %.2f/death)\n",
           g_pc.enabled, g_pc.enabled && g_pc.reaction && g_tick_ok, g_qc.enabled, g_qc.max_advancing, g_qc.min_squad,
           g_mc.enabled, g_mc.radius_m, g_mc.per_death);
    return true;
}

// ---- test host only (tests/dll/host.cpp): redirect the original init / touch / position calls to host functions,
// drive the clock, run the tick, read the registry back
static void __cdecl TestRedirect(DWORD init_fn, DWORD touch_fn, DWORD getpos_fn) { R_SM_INIT = init_fn; P_SM_TOUCH = touch_fn; P_SM_GETPOS = getpos_fn; }
static void __cdecl TestRedirectQuery(DWORD query_fn, DWORD free_fn) { P_SM_QUERY = query_fn; P_SM_LISTFREE = free_fn; }
static DWORD __cdecl TestCounters(int which) { return which == 0 ? g_found : which == 1 ? g_writes : which == 2 ? g_backoffs : g_deaths; }
static void __cdecl TestEnv(int on, uint32_t ms, int mission, int playmode) { g_test = on; g_test_ms = ms; g_test_mission = mission; g_test_pm = playmode; }
static void __cdecl TestTick(double now) { smart_tick(nullptr, now); }
static int  __cdecl TestInstalled() { return g_installed ? 1 : 0; }
struct TestEnt { DWORD node; int used, dead, role, token, holding, sq_ok, mo_ok, react_set; float fear; };
static int __cdecl TestEntry(DWORD node, TestEnt* out) {
    for (Ent& e : g_e) if (e.used && e.node == node) {
        out->node = node; out->used = 1; out->dead = e.dead; out->role = e.role; out->token = e.sq.token; out->holding = e.sq.holding;
        out->sq_ok = e.sq_ok; out->mo_ok = e.mo_ok; out->react_set = e.react_set; out->fear = e.fear.f;
        return 1;
    }
    return 0;
}
void* smart_test_stub(const char* name) {
    if (!strcmp(name, "smart_init_wrap")) return (void*)&SmartInitWrap;
    if (!strcmp(name, "smart_redirect")) return (void*)&TestRedirect;
    if (!strcmp(name, "smart_redirect_query")) return (void*)&TestRedirectQuery;
    if (!strcmp(name, "smart_counters")) return (void*)&TestCounters;
    if (!strcmp(name, "smart_env")) return (void*)&TestEnv;
    if (!strcmp(name, "smart_tick")) return (void*)&TestTick;
    if (!strcmp(name, "smart_installed")) return (void*)&TestInstalled;
    if (!strcmp(name, "smart_entry")) return (void*)&TestEntry;
    return nullptr;
}
