// gp_heat.cpp - [mayhem_heat]: the mayhem meter / wanted heat. Off unless
// [mayhem_heat] enabled = 1; every site and every address it relies on is byte-checked (sites.h HT_* rows); nothing is
// written to the game while it is off.
//
// What it does
//   * Heat: chaos Ram causes adds heat points (gp_heat_logic.cpp: kills, cop / civilian kills, his explosions,
//     destruction, cars, loco moves, combo style points). Five levels. Once he has caused nothing for calm_seconds the
//     heat cools one level per decay_seconds (slower while hostiles are still close).
//   * Radar: the RADAR stage of gp_hudstage.cpp tints the game's own radar disc (vertex colour): clean = the game's
//     look, building heat = warm orange, top heat = red, and at the top level the rim pulses red <-> dark (an overlay
//     ring drawn right after the disc). 1..5 chili peppers sit in an arc along the disc's right side (art
//     made for this mod, mayhem_heat/art/chili, built into the atlas by make_heat_art.py, resource
//     HEAT_ICONS): a lit slot k shows the chili with flame level k (a tiny flicker at the top slot up to a roaring flame
//     at the bottom), an unlit slot the darkened chili; the next chili lights from the bottom while heat builds; the top
//     lit chili breathes while the heat cools.
//   * Escalation: per level a reinforcement share, waves (several dead placements re-armed together, again every
//     interval while the level holds), placements above Ram first (rooftops, balconies) and second / third comebacks of
//     one placement at the higher levels. The engine is gp_difficulty.cpp's reinforcement code with all of its rules
//     (mission death conditions, no bosses / buddies / civilians / skip factions, 18..45 m from Ram, the pool's 2 free
//     slots for the mission, the live cap, saves through the game's own setters); heat only asks (heat_drive()).
//
// Event sources (sites.h HT_*; dll/HOOKS.md "Mayhem heat")
//   HT_KILL_E / HT_KILL_N  the sentry AI's dying state 0x5D6B70 counts a death into mission data +28h (hostile) / +2Ch
//                          (neutral) and touches the member (call 0x86B4B0): the call is wrapped, EDI = the victim's
//                          sentry node. Killer = sentry data +178h KillOwner (the game compares it with the player root
//                          itself, REF HT_KILLER_RAM); faction = root data +18h, Sub_Type +14h.
//   HT_DESTROY             Destroyable_Object command_destroy_object (0x6715C4, mov ecx,[0xA3B8C0]): EDI = the object,
//                          ESI = its data (+7Ch IsExplodeable, +20h DestructorOwner).
//   HT_CAR                 vehicle_ai's explode state 0x809910: the camera shake message after Explosion_Big (call
//                          0x869E60 at 0x809DE9): EBX = the vehicle_ai node, EBP = its data (+28h take-a-hit owner).
//   HT_SCORE               PlayerStatus command_add_to_overall_score (0x25F, handler 0x64F560 -> body 0x64CEF0): the
//                          award at the end of a combo (CurrentActionPoints x ComboMultiplier) is scaled by the heat
//                          level's score multiplier before the body adds it to OverallScore (data +24h). Branch
//                          mayhem-polish; [mayhem_heat] score_multiplier / score_top / score_label.
//   polled every tick      Combo_Ctrl CurrentActionPoints (+3Ch, rises with each scored kill / structure), Ram's
//                          PowerupCtrl RunningLocoMove (+20h: 0 -> slot = a loco move started), the weapon_shell pool
//                          (cache +550h): node flag 4 set = that shell just exploded (the explode handler sets it,
//                          REF HT_SHELL_FLAG), its owner (data +24h) Ram = his explosion.
#include <windows.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <objidl.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include "gp_glue.h"
#include "gp_heat_logic.h"
#include "../../mayhem_heat/art/heat_glyphs.h"   // the atlas layout + score label figures (make_heat_art.py)

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))

// ================================================================================================ game addresses
static DWORD R_HT_TOUCH = 0x86B4B0;          // HT_KILL_E / HT_KILL_N original target (member touch, thiscall, ret 4)
static DWORD R_HT_SENDMSG = 0x869E60;        // HT_CAR original target (Node::SendMessage, thiscall, ret 8)
static DWORD R_HT_DESTROY_BACK = 0x6715CA;   // HT_DESTROY: after the 6 relocated bytes
static DWORD R_HT_SCORE = 0x64CEF0;          // HT_SCORE original target: add_to_overall_score body cdecl(thread, int* amount)
static const DWORD A_PLAYER_HOLDER = 0xA3B590, A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764;
static const DWORD A_QUADTREE = 0xA3B578, A_LIST_ENTITY = 0xA3B8A8, A_SCENE = 0xA3DCBC;
static const DWORD C_MISSION = 0x25C, C_COMBO = 0x268, C_TIMESTAMP = 0x274, C_SHELLS = 0x550;
static const DWORD H_ROOT_CLS = 0x5C3060, H_SENTRY_CLS = 0x5C90F0;
static const DWORD P_QUERY = 0x486950, P_LISTFREE = 0x8718D0, P_GETPOS = 0x88CA80;
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);

// ================================================================================================ config
struct HtCfg {
    bool enabled = false, escalate = true, icons = true, pulse = true, log = true, verbose = false;
    bool score = true, score_label = false;    // the heat level scales the score Ram earns; the xN label (off: heat-chili)
    float tint = 1.0f, scale = 1.0f, icon_scale = 1.0f, near_radius = 30.0f, high_m = 3.0f, perf_s = 10.0f;
    float score_top = 2.0f;                    // the multiplier at the top level (x1 at no heat and level 1, linear between)
    ht::Cfg h;
};
static HtCfg g_c;
static int g_dbg_pin = -1;   // test copy only: [mayhem_heat] debug_heat_level = 0..5 holds the heat at that level (proof runs)
// test copy only: debug_score_seconds = N sends the game's own add_to_overall_score (0x25F, debug_score_amount) to the
// PlayerStatus node every N s, debug_score_count times: the real handler -> HT_SCORE -> body path, for the proof runs
static float g_dbg_score_s = 0.0f; static int g_dbg_score_amount = 38, g_dbg_score_count = 3, g_dbg_score_n = 0; static double g_dbg_score_last = 0.0;
// test copy only: debug_heat_step_seconds = N walks the pinned heat 0 -> 5, one level every N s (the
// last 2 s of each step ramp the heat up, so a chili lights through its soft band), and logs a "debug keys: mark shot"
// line an in-game test harness (not included) can grab: chili_h0..h5 once a level has been held 3 s, chili_fill_2to3 halfway through that ramp
static float g_dbg_step_s = 0.0f; static double g_dbg_step_t0 = 0.0; static int g_dbg_step_marked = -1; static bool g_dbg_fill_marked = false;
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
void heat_load_config() {
    const gp::Ini& ini = gp_ini();
    HtCfg c;
    c.enabled = ini.geti("mayhem_heat", "enabled", 0) != 0;
    c.h.decay_seconds = clampf(ini.getf("mayhem_heat", "decay_seconds", 10.0f), 1.0f, 120.0f);
    c.h.calm_seconds = clampf(ini.getf("mayhem_heat", "calm_seconds", 7.0f), 1.0f, 60.0f);
    c.h.max_level = (int)clampf((float)ini.geti("mayhem_heat", "max_level", 5), 1.0f, 5.0f);
    c.h.gain = clampf(ini.getf("mayhem_heat", "heat_gain", 1.0f), 0.1f, 5.0f);
    c.h.own_weight = clampf(ini.getf("mayhem_heat", "others_weight", 0.5f), 0.0f, 1.0f);
    c.h.near_factor = clampf(ini.getf("mayhem_heat", "near_cooling", 0.3f), 0.0f, 1.0f);
    c.tint = clampf(ini.getf("mayhem_heat", "tint", 1.0f), 0.0f, 1.0f);
    c.icons = ini.geti("mayhem_heat", "icons", 1) != 0;
    c.pulse = ini.geti("mayhem_heat", "pulse", 1) != 0;
    c.icon_scale = clampf(ini.getf("mayhem_heat", "icon_scale", 1.0f), 0.5f, 1.6f);
    c.escalate = ini.geti("mayhem_heat", "escalation", 1) != 0;
    c.scale = clampf(ini.getf("mayhem_heat", "escalation_scale", 1.0f), 0.0f, 3.0f);
    c.high_m = clampf(ini.getf("mayhem_heat", "rooftop_height", 3.0f), 1.0f, 20.0f);
    c.near_radius = clampf(ini.getf("mayhem_heat", "near_radius", 30.0f), 5.0f, 80.0f);
    c.log = ini.geti("mayhem_heat", "log", 1) != 0;
    c.verbose = ini.geti("mayhem_heat", "log_verbose", 0) != 0;
    c.perf_s = ini.getf("mayhem_heat", "perf_log_seconds", 10.0f);
    c.score = ini.geti("mayhem_heat", "score_multiplier", 1) != 0;
    c.score_top = clampf(ini.getf("mayhem_heat", "score_top", 2.0f), 1.0f, 4.0f);
    c.score_label = ini.geti("mayhem_heat", "score_label", 0) != 0;   // off by default (the chilis alone show the level)
    g_dbg_pin = ini.geti("mayhem_heat", "debug_heat_level", -1);
    if (g_dbg_pin > 5) g_dbg_pin = 5;
    g_dbg_score_s = ini.getf("mayhem_heat", "debug_score_seconds", 0.0f);
    g_dbg_score_amount = ini.geti("mayhem_heat", "debug_score_amount", 38);
    g_dbg_score_count = ini.geti("mayhem_heat", "debug_score_count", 3);
    g_dbg_step_s = ini.getf("mayhem_heat", "debug_heat_step_seconds", 0.0f);
    if (g_dbg_step_s > 0.0f && g_dbg_step_s < 4.0f) g_dbg_step_s = 4.0f;
    g_c = c;
}
bool heat_enabled() { return g_c.enabled; }
bool heat_wants_tick() { return g_c.enabled; }
bool heat_ini_wants_reinforcements() {
    const gp::Ini& ini = gp_ini();
    return ini.geti("mayhem_heat", "enabled", 0) != 0 && ini.geti("mayhem_heat", "escalation", 1) != 0 &&
           ini.getf("mayhem_heat", "escalation_scale", 1.0f) > 0.0f;
}

// ================================================================================================ state
static bool g_installed = false, g_hud = false, g_hooks_ok = false;
static ht::State g_s;
static int g_last_level = 0;
static double g_last_tick = 0.0, g_last_near = 0.0, g_last_report = 0.0, g_last_wave = 0.0;
static int g_near = 0, g_ups_seen = 0, g_wave_due = 0, g_waves = 0, g_wave_spent = 0;
static int g_ev_count[ht::EV_COUNT];
static int g_mission = -2; static uint32_t g_last_gms = 0;

// events from the hooks: queued (the hooks run inside the game's own code), applied by the tick
struct QEv { int ev; float amount; bool by_ram; int faction; DWORD who; };
static QEv g_q[64]; static volatile LONG g_qn = 0; static LONG g_qlost = 0;
static void queue_ev(int ev, float amount, bool by_ram, int faction, DWORD who) {
    LONG i = g_qn;
    if (i >= 64) { ++g_qlost; return; }
    g_q[i] = QEv{ev, amount, by_ram, faction, who};
    g_qn = i + 1;
}
static DWORD sdata(DWORD n) { __try { return n ? RD32(n + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_of(DWORD node) {
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

// ================================================================================================ hooks
// HT_KILL_E / HT_KILL_N: the death is counted; EDI = the victim's character_sentry_ai node
static void __cdecl on_kill(DWORD sentry, int hostile) {
    __try {
        DWORD sd = sdata(sentry); DWORD root = RD32(sentry + 0x40); DWORD rd = sdata(root);
        if (!sd || !rd) return;
        DWORD owner = RD32(sd + 0x178);                 // KillOwner (REF HT_KILLER_RAM)
        int faction = (int)RD32(rd + 0x18), sub = (int)RD32(rd + 0x14);
        bool ram = owner && owner == ram_root();
        int ev;
        if (!ram) ev = ht::EV_KILL_NPC;
        else if (faction == 14) ev = ht::EV_KILL_COP;
        else if (faction == 7) ev = ht::EV_KILL_NPC;    // animals (chickens): no heat worth a level
        else if (!hostile && (faction == 6 || sub == 9 || faction == 11)) ev = ht::EV_KILL_CIVIL;
        else ev = ht::EV_KILL_ENEMY;
        queue_ev(ev, 1.0f, ram, faction, root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) static void KillHostileStub() {
    __asm pushad
    __asm pushfd
    __asm push 1
    __asm push edi
    __asm call on_kill
    __asm add esp, 8
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_HT_TOUCH]
}
__declspec(naked) static void KillNeutralStub() {
    __asm pushad
    __asm pushfd
    __asm push 0
    __asm push edi
    __asm call on_kill
    __asm add esp, 8
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_HT_TOUCH]
}
// HT_DESTROY: EDI = the Destroyable_Object node, ESI = its data
static void __cdecl on_destroy(DWORD node, DWORD data) {
    __try {
        if (!data) return;
        bool expl = RD32(data + 0x7C) != 0;            // IsExplodeable (REF HT_DESTR_EXPL)
        DWORD owner = RD32(data + 0x20);                // DestructorOwner
        DWORD ram = ram_root();
        queue_ev(expl ? ht::EV_BARREL : ht::EV_DESTROY, 1.0f, owner && owner == ram, -1, node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) static void DestroyStub() {
    __asm pushad
    __asm pushfd
    __asm push esi
    __asm push edi
    __asm call on_destroy
    __asm add esp, 8
    __asm popfd
    __asm popad
    __asm mov ecx, dword ptr ds:[0x00A3B8C0]
    __asm jmp dword ptr [R_HT_DESTROY_BACK]
}
// HT_CAR: EBX = the vehicle_ai node, EBP = its data
static void __cdecl on_car(DWORD node, DWORD data) {
    __try {
        DWORD owner = data ? RD32(data + 0x28) : 0;     // take-a-hit owner (inferred from the class table: logged)
        DWORD ram = ram_root();
        bool by_ram = owner && owner == ram;
        queue_ev(ht::EV_CAR, 1.0f, by_ram, owner == 0 ? -2 : -1, node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) static void CarStub() {
    __asm pushad
    __asm pushfd
    __asm push ebp
    __asm push ebx
    __asm call on_car
    __asm add esp, 8
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_HT_SENDMSG]
}

// HT_SCORE: PlayerStatus add_to_overall_score. The handler 0x64F560 copies the integer argument into its own stack slot
// and calls the body 0x64CEF0 (cdecl: the script thread, a pointer to that slot); the body adds it to OverallScore
// (PlayerStatus data +24h, REF HT_SCORE_ADD) and checks the score rewards. The slot is scaled here first, so the
// game's own combo multiplier (in the amount already) and the heat multiplier stack, and the rewards see the real sum.
static int g_score_awards = 0; static long long g_score_bonus = 0;
static float cur_score_mult() { return g_c.score ? ht::score_multiplier(g_s.level, g_c.h.max_level, g_c.score_top) : 1.0f; }
static void __cdecl ScoreWrap(DWORD thread, int* amount) {
    __try {
        if (g_installed && g_c.score && amount) {
            int a = *amount;
            float m = cur_score_mult();
            int b = ht::scale_score(a, m);
            int cm = -1, pts = -1, before = -1;
            DWORD cd = gp_cache_data(); DWORD cc = cd ? RD32(cd + C_COMBO) : 0; DWORD d = sdata(cc);
            if (d) { cm = (int)RD32(d + 0x34); pts = (int)RD32(d + 0x3C); }   // ComboMultiplier / CurrentActionPoints
            DWORD ps = sdata(RD32(A_CUR_NODE)); if (ps) before = (int)RD32(ps + 0x24);   // OverallScore (REF HT_SCORE_ADD)
            if (b != a) { *amount = b; g_score_bonus += b - a; }
            ++g_score_awards;
            if (g_c.log)
                gp_log("heat: score award +%d at heat level %d (x%.2f) -> +%d; OverallScore %d -> %d (combo now %d x %d)\n", a, g_s.level, m, b,
                       before, before >= 0 ? before + b : -1, cm, pts);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ((void(__cdecl*)(DWORD, int*))(uintptr_t)R_HT_SCORE)(thread, amount);
}

// ================================================================================================ polls
static int g_last_pts = -1; static int g_last_loco = -1;
struct Shell { DWORD node; bool hidden; };
static Shell g_sh[64]; static int g_nsh = 0;
static int list_count(DWORD L) { __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; return (L && k) ? (int)(RD32(L + 8) / k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD list_at(DWORD L, int i) {
    __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
            return el ? RD32(el + (DWORD)i * 4 * k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void poll(DWORD cd) {
    DWORD ram = ram_root();
    // combo style points
    __try {
        DWORD cc = RD32(cd + C_COMBO); DWORD d = sdata(cc);
        if (d) {
            int pts = (int)RD32(d + 0x3C);              // CurrentActionPoints (REF G_COMBO_PTS)
            if (g_last_pts >= 0 && pts > g_last_pts && pts - g_last_pts < 100000) queue_ev(ht::EV_STYLE, (float)(pts - g_last_pts), true, -1, 0);
            g_last_pts = pts;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // loco move started: Ram's PowerupCtrl (root data +8Ch) RunningLocoMove +20h 0 -> slot
    __try {
        DWORD rd = sdata(ram); DWORD pu = rd ? RD32(rd + 0x8C) : 0; DWORD pd = sdata(pu);
        if (pd) {
            int run = (int)RD32(pd + 0x20);             // REF HT_LOCO_RUN / HT_LOCO_IDLE
            if (g_last_loco == 0 && run > 0 && run < 16) queue_ev(ht::EV_LOCO, 1.0f, true, run, pu);
            g_last_loco = run;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // shells: flag 4 newly set = exploded
    __try {
        DWORD L = RD32(cd + C_SHELLS);                  // REF PH_SHELLS: the weapon_shell pool
        int n = list_count(L);
        for (int i = 0; i < n && i < 64; ++i) {
            DWORD s = list_at(L, i); if (!s) continue;
            bool hid = (RD32(s + 0x28) & 4) != 0;       // REF HT_SHELL_FLAG
            int k = 0; while (k < g_nsh && g_sh[k].node != s) ++k;
            if (k == g_nsh) { if (g_nsh >= 64) continue; g_sh[g_nsh++] = Shell{s, hid}; continue; }
            if (hid && !g_sh[k].hidden) {
                DWORD sd = sdata(s); DWORD owner = sd ? RD32(sd + 0x24) : 0;
                if (owner && owner == ram) queue_ev(ht::EV_SHELL, 1.0f, true, -1, s);
                else if (g_c.verbose) gp_log("heat: a shell %08X exploded, owner %08X (not Ram): no heat\n", s, owner);
            }
            g_sh[k].hidden = hid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// live hostile characters within near_radius of Ram (the Tornado's query: mask 40h mode 0)
static int hostiles_near(DWORD ram) {
    float rp[3] = {0, 0, 0};
    DWORD k1 = RD32(A_CUR_NODE), k2 = RD32(A_CUR_2), k3 = RD32(A_CUR_3);
    bool ok = true;
    __try { ((Vec_t)(uintptr_t)P_GETPOS)(ram, nullptr, rp); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, rp[0], rp[1], rp[2], g_c.near_radius, 0x40, 0};
    DWORD qt = 0; __try { qt = RD32(A_QUADTREE); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (ok && qt) { __try { ((Query_t)(uintptr_t)P_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { q.out = 0; } }
    int n = 0;
    if (q.out) {
        __try {
            for (int i = 0, c = list_count(q.out); i < c && i < 128; ++i) {
                DWORD r = list_at(q.out, i);
                if (!r || r == ram || cls_of(r) != H_ROOT_CLS) continue;
                DWORD rd = sdata(r); DWORD ai = rd ? RD32(rd + 0x58) : 0; DWORD d = sdata(ai);
                if (!d || cls_of(ai) != H_SENTRY_CLS) continue;
                int f = (int)RD32(rd + 0x18);
                if (f == 0 || f == 6 || f == 7 || f == 10 || f == 11 || f == 18) continue;   // player, civilians, animals, friends
                if (RDF(d + 0x2C) > 0.0f && (int)RD32(d + 0x1D4) != 28) ++n;            // alive, not dying
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        DWORD L = q.out;
        __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((Free_t)(uintptr_t)P_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (RD32(A_CUR_NODE) != k1) *(volatile DWORD*)(uintptr_t)A_CUR_NODE = k1;
    if (RD32(A_CUR_2) != k2) *(volatile DWORD*)(uintptr_t)A_CUR_2 = k2;
    if (RD32(A_CUR_3) != k3) *(volatile DWORD*)(uintptr_t)A_CUR_3 = k3;
    return n;
}
static void reset(const char* why) {
    if (g_s.heat > 0.0f || g_wave_due) gp_log("heat: %s: heat %.0f (level %d) cleared\n", why, g_s.heat, g_s.level);
    g_s.reset(); g_ups_seen = 0; g_wave_due = 0; g_last_level = 0; g_last_pts = -1; g_last_loco = -1;
    g_qn = 0;
    g_dbg_step_t0 = 0.0; g_dbg_step_marked = -1; g_dbg_fill_marked = false;   // test only: the walk starts again
}

// ================================================================================================ escalation drive
static float scale_eff() { return g_c.scale * ht::difficulty_scale(difficulty_level()); }
HeatDrive heat_drive() {
    HeatDrive d = {false, 0.0f, 0, 1, false, g_c.high_m, 0};
    if (!g_installed || !g_c.escalate || g_c.scale <= 0.0f) return d;
    ht::Esc e = ht::escalation(g_s.level, scale_eff());
    d.level = g_s.level; d.share_add = e.share_add; d.max_rearms = e.max_rearms; d.prefer_high = e.prefer_high;
    d.wave_due = g_wave_due;
    d.active = g_s.level > 0 || g_wave_due > 0;
    return d;
}
void heat_wave_spent(int n) {
    if (n <= 0) return;
    g_wave_due = g_wave_due > n ? g_wave_due - n : 0;
    g_wave_spent += n;
}
static void escalate(double now) {
    if (!g_c.escalate || g_c.scale <= 0.0f) return;
    ht::Esc e = ht::escalation(g_s.level, scale_eff());
    bool up = g_s.ups != g_ups_seen;
    g_ups_seen = g_s.ups;
    if (g_s.cooling) {                                  // he got away: waves he did not see coming are called off
        if (g_wave_due) { if (g_c.log) gp_log("heat: cooling: %d pending reinforcement(s) of the wave called off\n", g_wave_due); g_wave_due = 0; }
        return;
    }
    int add = 0; const char* why = nullptr;
    if (up && e.wave > 0) { add = e.wave; why = "level up"; }
    else if (e.wave > 0 && e.interval_s > 0.0f && now - g_last_wave >= e.interval_s) { add = e.wave; why = "the fight goes on"; }
    if (!add) return;
    g_wave_due = g_wave_due + add > 8 ? 8 : g_wave_due + add;
    g_last_wave = now; ++g_waves;
    if (g_c.log) gp_log("heat: WAVE %d (%s): level %d asks for %d reinforcement(s)%s, %d due; share +%.2f per death, comebacks %d, scale %.2f (difficulty %d)\n",
                        g_waves, why, g_s.level, add, e.prefer_high ? " from above first" : "", g_wave_due, e.share_add, e.max_rearms, scale_eff(), difficulty_level());
}

// ================================================================================================ tick
static bool game_ms(DWORD cd, uint32_t* ms) {
    __try { DWORD n = RD32(cd + C_TIMESTAMP); DWORD d = n ? RD32(n + 0x18) : 0; if (!d) return false; *ms = RD32(d + 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// test copy only: the PlayerStatus node (by name: a cache entry, else the scene graph), and the game's SendCommand
static const char* hnode_name(DWORD n) {   // Node +4Ch
    __try { const char* s = n ? (const char*)(uintptr_t)RD32(n + 0x4C) : nullptr; if (s && !IsBadStringPtrA(s, 64)) return s; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}
static bool name_has(const char* s, const char* sub) {
    for (; *s; ++s) { int i = 0; while (sub[i] && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i])) ++i; if (!sub[i]) return true; }
    return false;
}
static DWORD find_player_status(DWORD cd, int* where) {
    static bool dumped = false;
    *where = 0;
    for (DWORD off = 0; off < 0x800; off += 4) {
        DWORD n = 0; __try { n = RD32(cd + off); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        const char* nm = hnode_name(n);
        if (!dumped && nm[0]) gp_log("heat: debug: cache +%03X node %08X '%s'\n", off, n, nm);
        if (nm[0] && name_has(nm, "playerstatus")) { dumped = true; *where = 0x1000 + (int)off; return n; }
    }
    dumped = true;
    DWORD scene = 0, root = 0; __try { scene = RD32(0xA3DCBC); root = scene ? RD32(scene + 0x44) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD* stack = (DWORD*)malloc(sizeof(DWORD) * 65536); if (!stack) return 0;
    int sp = 0, visited = 0; if (root) stack[sp++] = root;
    DWORD found = 0;
    while (sp > 0 && visited < 400000 && !found) {
        DWORD n = stack[--sp]; ++visited;
        DWORD sib = 0, child = 0; __try { sib = RD32(n + 0x34); child = RD32(n + 0x44); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (name_has(hnode_name(n), "playerstatus")) found = n;
        if (sib && sp < 65536) stack[sp++] = sib;
        if (child && sp < 65536) stack[sp++] = child;
    }
    free(stack);
    *where = found ? 2 : -visited;
    return found;
}
static void debug_score(DWORD cd, double now) {
    if (!(g_dbg_score_s > 0.0f) || g_dbg_score_n >= g_dbg_score_count || now - g_dbg_score_last < g_dbg_score_s) return;
    g_dbg_score_last = now; ++g_dbg_score_n;
    int where = 0; DWORD ps = find_player_status(cd, &where);
    if (!ps) { gp_log("heat: debug score: no PlayerStatus node found (scene walk %d nodes)\n", -where); return; }
    int amount = g_dbg_score_amount; bool ok = true;
    DWORD a = 0, b = 0, c = 0; __try { a = RD32(0xA3B75C); b = RD32(0xA3B760); c = RD32(0xA3B764); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { ((void(__fastcall*)(DWORD, void*, int, void*))(uintptr_t)0x86A340)(ps, nullptr, 0x25F, &amount); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    __try { *(volatile DWORD*)0xA3B75C = a; *(volatile DWORD*)0xA3B760 = b; *(volatile DWORD*)0xA3B764 = c; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("heat: debug score %d/%d: add_to_overall_score(%d) sent to PlayerStatus %08X '%s' (%s)%s\n", g_dbg_score_n, g_dbg_score_count,
           g_dbg_score_amount, ps, hnode_name(ps), where >= 0x1000 ? "cache entry" : "scene graph", ok ? "" : " EXCEPTION");
}
void heat_tick(void* thread, double now) {
    if (!g_installed) return;
    float dt = g_last_tick > 0.0 ? (float)(now - g_last_tick) : 0.0f;
    if (dt < 0.0f) dt = 0.0f; if (dt > 0.25f) dt = 0.25f;    // menus / loads: the tick does not run, heat waits
    g_last_tick = now;
    DWORD cd = gp_cache_data(); if (!cd) return;
    int m = -1; __try { DWORD mn = RD32(cd + C_MISSION); DWORD md = sdata(mn); m = md ? (int)RD32(md + 0x38) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (m != g_mission) { if (g_mission != -2) reset("mission changed"); g_mission = m; }
    uint32_t gms = 0;
    if (game_ms(cd, &gms)) { if (gms + 2000 < g_last_gms) reset("game clock went back (load / retry)"); g_last_gms = gms; }
    poll(cd);
    // apply queued events
    LONG n = g_qn;
    for (LONG i = 0; i < n; ++i) {
        const QEv& e = g_q[i];
        float before = g_s.heat;
        g_s.event(g_c.h, e.ev, e.amount, e.by_ram);
        ++g_ev_count[e.ev];
        if (g_c.log && (e.ev != ht::EV_STYLE || g_c.verbose))
            gp_log("heat: %s%s x%.0f (faction %d, %08X): heat %.1f -> %.1f\n", ht::ev_name(e.ev), e.by_ram ? " by Ram" : "", e.amount,
                   e.faction, e.who, before, g_s.heat);
        if (e.ev == ht::EV_STYLE && g_c.log && !g_c.verbose && e.amount >= 50.0f) gp_log("heat: style points +%.0f: heat %.1f -> %.1f\n", e.amount, before, g_s.heat);
    }
    g_qn = 0;
    if (g_qlost) { gp_log("heat: %ld events lost (queue full)\n", g_qlost); g_qlost = 0; }
    DWORD ram = ram_root();
    if (ram && now - g_last_near >= 1.0) { g_last_near = now; g_near = hostiles_near(ram); }
    if (g_dbg_pin >= 0) {   // test copy only: the heat held at one level (no cooling, no rise)
        float h = g_dbg_pin > 0 ? ht::level_start(g_c.h, g_dbg_pin) + 1.0f : 0.0f;
        if (g_dbg_step_s > 0.0f) {   // test copy only: walk 0 -> max_level (debug_heat_step_seconds)
            if (g_dbg_step_t0 <= 0.0 && ram_root()) g_dbg_step_t0 = now;   // from the first tick with Ram
            double t = g_dbg_step_t0 > 0.0 ? now - g_dbg_step_t0 : 0.0;
            int top = g_c.h.max_level, L = (int)(t / g_dbg_step_s); if (L > top) L = top;
            float in = (float)(t - L * (double)g_dbg_step_s);
            auto at = [&](int l) { return l > 0 ? ht::level_start(g_c.h, l) + 1.0f : 0.0f; };
            h = at(L);
            float ramp = in - (g_dbg_step_s - 2.0f);
            if (L < top && ramp > 0.0f) {
                h = at(L) + (at(L + 1) - at(L)) * (ramp / 2.0f);
                if (L == 2 && ramp >= 1.0f && !g_dbg_fill_marked) { g_dbg_fill_marked = true; gp_log("heat: debug step: ramp 2 -> 3, heat %.1f; debug keys: mark shot chili_fill_2to3\n", h); }
            }
            if (in >= 3.0f && g_dbg_step_marked < L) { g_dbg_step_marked = L; gp_log("heat: debug step: level %d held; debug keys: mark shot chili_h%d\n", L, L); }
        }
        if (g_s.heat != h) { if (g_c.log && fabsf(g_s.heat - h) > 5.0f) gp_log("heat: debug_heat_level %d: heat %.1f -> %.1f\n", g_dbg_pin, g_s.heat, h); g_s.heat = h; }
        g_s.since_event = 0.0f;
    }
    debug_score(cd, now);
    bool was_cooling = g_s.cooling;
    int lv = g_s.tick(g_c.h, dt, g_near);
    if (lv != g_last_level) {
        if (g_c.log) gp_log("heat: LEVEL %d -> %d (heat %.1f, %s, %d hostiles within %.0f m)\n", g_last_level, lv, g_s.heat,
                            lv > g_last_level ? "rising" : "cooling", g_near, g_c.near_radius);
        g_last_level = lv;
    }
    if (g_s.cooling != was_cooling && g_c.log) gp_log("heat: %s (heat %.1f, level %d, %.1f s since Ram's last chaos, %d hostiles near)\n",
                                                     g_s.cooling ? "cooling starts" : "cooling stopped", g_s.heat, g_s.level, g_s.since_event, g_near);
    escalate(now);
    if (now - g_last_report >= (g_c.verbose ? 2.0 : 10.0) && (g_s.heat > 0.0f || g_c.verbose)) {
        g_last_report = now;
        gp_log("heat: state heat %.1f level %d peak %d shown %.2f%s, %d hostiles near, waves %d (due %d, re-armed %d)\n", g_s.heat, g_s.level,
               g_s.peak, g_s.shown, g_s.cooling ? " cooling" : "", g_near, g_waves, g_wave_due, g_wave_spent);
    }
}

// ================================================================================================ HUD: radar tint, edge pulse, icons
// [minimap] (dll/src/gp_hud.cpp) multiplies its map ink by this tint so the map follows the disc; weak-linked
// to a no-op until gp_hud.cpp is linked into the same DLL (MSVC /alternatename: the real symbol wins when present)
void hud_set_map_tint(DWORD argb);
void heat_map_tint_noop(DWORD) {}
#pragma comment(linker, "/alternatename:?hud_set_map_tint@@YAXK@Z=?heat_map_tint_noop@@YAXK@Z")
static DWORD g_map_tint = 0xFFFFFFFF;
static uint32_t* g_art = nullptr; static int g_aw = 0, g_ah = 0;
static bool load_art() {
    if (g_art) return true;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&load_art, &self);
    HRSRC r = FindResourceA(self, "HEAT_ICONS", MAKEINTRESOURCEA(10));   // RT_RCDATA
    if (!r) { gp_log("heat: icon art resource missing\n"); return false; }
    DWORD n = SizeofResource(self, r); HGLOBAL g = LoadResource(self, r); const void* p = g ? LockResource(g) : nullptr;
    if (!p || !n) return false;
    ULONG_PTR tok = 0; Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&tok, &in, nullptr) != Gdiplus::Ok) { gp_log("heat: GDI+ start failed\n"); return false; }
    bool ok = false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (mem) {
        memcpy(GlobalLock(mem), p, n); GlobalUnlock(mem);
        IStream* st = nullptr;
        if (CreateStreamOnHGlobal(mem, TRUE, &st) == S_OK) {
            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(st);
            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
                Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
                if (w > 0 && h > 0 && bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                    g_art = new uint32_t[(size_t)w * h];
                    for (int y = 0; y < h; ++y) memcpy(g_art + (size_t)y * w, (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)w * 4);
                    bmp->UnlockBits(&bd);
                    g_aw = w; g_ah = h; ok = true;
                }
            }
            delete bmp;
            st->Release();
        } else GlobalFree(mem);
    }
    Gdiplus::GdiplusShutdown(tok);
    gp_log("heat: icon art %s (%dx%d, %lu bytes)\n", ok ? "loaded" : "NOT decoded", g_aw, g_ah, n);
    return ok;
}
static IDirect3DDevice9* g_dev = nullptr; static IDirect3DTexture9* g_tex = nullptr; static bool g_tex_failed = false;
static bool make_texture(IDirect3DDevice9* dev) {
    if (dev == g_dev && g_tex) return true;
    if (g_tex_failed && dev == g_dev) return false;
    if (g_dev && dev != g_dev) { gp_log("heat: the device changed (%p -> %p): new texture\n", g_dev, dev); g_tex = nullptr; }
    g_dev = dev; g_tex_failed = true;
    if (!load_art()) return false;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(g_aw, g_ah, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) { gp_log("heat: texture not created\n"); return false; }
    // premultiplied, full mip chain (box filter in premultiplied space)
    int w = g_aw, h = g_ah;
    uint32_t* lv = new uint32_t[(size_t)w * h];
    for (int i = 0; i < w * h; ++i) {
        uint32_t c = g_art[i], a = c >> 24;
        lv[i] = a << 24 | (((c >> 16) & 255) * a / 255) << 16 | (((c >> 8) & 255) * a / 255) << 8 | ((c & 255) * a / 255);
    }
    UINT levels = t->GetLevelCount();
    bool ok = true;
    for (UINT l = 0; l < levels && ok; ++l) {
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(l, &lr, nullptr, 0))) { ok = false; break; }
        for (int y = 0; y < h; ++y) memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, lv + (size_t)y * w, (size_t)w * 4);
        t->UnlockRect(l);
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                int x0 = 2 * x, x1 = w > 1 ? 2 * x + 1 : 2 * x, y0 = 2 * y, y1 = h > 1 ? 2 * y + 1 : 2 * y;
                uint32_t s[4] = {lv[y0 * w + x0], lv[y0 * w + x1], lv[y1 * w + x0], lv[y1 * w + x1]};
                uint32_t o = 0;
                for (int sh = 0; sh < 32; sh += 8) { uint32_t acc = 0; for (int k = 0; k < 4; ++k) acc += (s[k] >> sh) & 255; o |= ((acc + 2) / 4) << sh; }
                lv[y * nw + x] = o;
            }
        w = nw; h = nh;
    }
    delete[] lv;
    if (!ok) { t->Release(); gp_log("heat: texture not filled\n"); return false; }
    g_tex = t; g_tex_failed = false;
    gp_log("heat: icon texture ready on device %p (%dx%d, %u levels)\n", dev, g_aw, g_ah, levels);
    return true;
}
struct Vtx { float x, y, z, rhw; DWORD c; float u, v; };
static DWORD pm_colour(float r, float g, float b, float a) {   // premultiplied by the caller
    auto q = [](float v) { int i = (int)(v * 255.0f + 0.5f); return (DWORD)(i < 0 ? 0 : (i > 255 ? 255 : i)); };
    return q(a) << 24 | q(r) << 16 | q(g) << 8 | q(b);
}
// a textured quad: screen corners from the affine map of a UV rectangle (radar UV space), atlas rectangle in texels
static void quad_uv(IDirect3DDevice9* d, const ht::Aff& m, float u0, float v0, float u1, float v1,
                    float ax0, float ay0, float ax1, float ay1, DWORD col) {
    float us[4] = {u0, u0, u1, u1}, vs[4] = {v0, v1, v0, v1};
    float as[4] = {ax0, ax0, ax1, ax1}, bs[4] = {ay0, ay1, ay0, ay1};
    Vtx v[4];
    for (int i = 0; i < 4; ++i) {
        float x, y; ht::aff_apply(m, us[i], vs[i], &x, &y);
        v[i] = Vtx{x - 0.5f, y - 0.5f, 0.0f, 1.0f, col, as[i] / (float)g_aw, bs[i] / (float)g_ah};
    }
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}
// the same with a colour per edge (top, bottom): the soft edge where a chili lights
static void quad_uv2(IDirect3DDevice9* d, const ht::Aff& m, float u0, float v0, float u1, float v1,
                     float ax0, float ay0, float ax1, float ay1, DWORD ctop, DWORD cbot) {
    if (v1 <= v0) return;
    float us[4] = {u0, u0, u1, u1}, vs[4] = {v0, v1, v0, v1};
    float as[4] = {ax0, ax0, ax1, ax1}, bs[4] = {ay0, ay1, ay0, ay1};
    DWORD cs[4] = {ctop, cbot, ctop, cbot};
    Vtx v[4];
    for (int i = 0; i < 4; ++i) {
        float x, y; ht::aff_apply(m, us[i], vs[i], &x, &y);
        v[i] = Vtx{x - 0.5f, y - 0.5f, 0.0f, 1.0f, cs[i], as[i] / (float)g_aw, bs[i] / (float)g_ah};
    }
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}
// one chili icon: atlas cell `cell` (0 = cold, 1..5 = flame level), the square art `sq` radar-UV wide with the pepper's
// centre on (u, v); only the rows [r0, r1] of the cell (0..1, top to bottom) are drawn, with alpha a0 at r0 and a1 at r1
static const float CHILI_SQUARE = 64.0f / 288.0f / 0.17f;   // the square art per icon_place size: 64 px at 1080p
static void chili(IDirect3DDevice9* d, const ht::Aff& m, int cell, float u, float v, float sq, float r0, float r1,
                  float a0, float a1, float bright) {
    if (r1 <= r0) return;
    float cw = sq * (float)HEAT_CELL / (float)HEAT_ICON;     // the whole cell (art + shadow margin)
    float x0 = u - HEAT_PEPPER_U * sq - sq * (float)HEAT_PAD / (float)HEAT_ICON;
    float y0 = v - HEAT_PEPPER_V * sq - sq * (float)HEAT_PAD / (float)HEAT_ICON;
    float ax = (float)(cell * HEAT_CELL);
    float c0 = a0 * bright, c1 = a1 * bright;
    quad_uv2(d, m, x0, y0 + cw * r0, x0 + cw, y0 + cw * r1, ax, HEAT_CELL * r0, ax + HEAT_CELL, HEAT_CELL * r1,
             pm_colour(c0, c0, c0, a0), pm_colour(c1, c1, c1, a1));
}
static double g_last_draw = 0.0, g_pf_last = 0.0, g_pf_sum = 0.0, g_pf_max = 0.0; static int g_pf_n = 0;
static float g_icons_a = 0.0f; static float g_full_t[ht::LEVELS];
static bool g_logged_draw = false;
// the score multiplier label ("x1.5") right of the middle coin, in the figures of the atlas; fades with the coins, pops
// when the multiplier steps up
static float g_lab_a = 0.0f; static char g_lab[16] = ""; static double g_lab_t = 0.0;
static const HeatGlyph* glyph_of(char c) { for (const HeatGlyph& g : HEAT_GLYPHS) if (g.c == c) return &g; return nullptr; }
static void draw_label(IDirect3DDevice9* d, const ht::Aff& m, float op, double now, float dt) {
    char t[16]; float mult = cur_score_mult();
    int len = g_c.score && g_c.score_label ? ht::score_label(mult, t, sizeof t) : 0;
    float target = len > 0 && g_icons_a > 0.5f ? 1.0f : 0.0f;
    g_lab_a += (target - g_lab_a) * (dt * 5.0f > 1.0f ? 1.0f : dt * 5.0f);
    if (fabsf(target - g_lab_a) < 0.01f) g_lab_a = target;
    if (len > 0 && strcmp(t, g_lab)) { if (g_lab[0] && mult > 1.0f) g_lab_t = now; strcpy(g_lab, t); }
    if (g_lab_a <= 0.003f || !g_lab[0]) return;
    int n = g_c.h.max_level;
    float u, v, s; ht::icon_place((n - 1) / 2, n, g_c.icon_scale, &u, &v, &s);
    float pop = 1.0f;
    if (g_lab_t > 0.0) { float k = (float)(now - g_lab_t) / 0.4f; if (k < 1.0f) pop = 1.0f + 0.3f * sinf(k * 3.14159f); }
    float ch = s * 0.95f * pop, px = ch / 64.0f;     // cell height in radar UV, UV per atlas pixel
    float x = u + s * CHILI_SQUARE * 0.52f, y = v - ch * 0.5f - s * 0.04f;
    float a = g_lab_a * op;
    for (const char* c = g_lab; *c; ++c) {
        const HeatGlyph* g = glyph_of(*c); if (!g) continue;
        quad_uv(d, m, x, y, x + g->w * px, y + ch, (float)g->x, (float)g->y, (float)(g->x + g->w), (float)(g->y + g->h), pm_colour(a, a, a, a));
        x += g->adv * px;
    }
}
static void draw_radar(const GpRadarFrame& f) {
    LARGE_INTEGER q0, q1, fr; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&fr);
    double now = f.now;
    float dt = g_last_draw > 0.0 ? (float)(now - g_last_draw) : 0.0f;
    if (dt < 0.0f || dt > 0.25f) dt = dt < 0.0f ? 0.0f : 0.25f;
    g_last_draw = now;
    // the tint follows the level step by step (the stage applies it from the next frame)
    g_s.shown = ht::follow(g_s.shown, g_s.level, dt);
    ht::Rgb t = ht::tint_for(g_s.shown, g_c.tint);
    gp_radar_set_tint(0, t.r, t.g, t.b, 1.0f, "mayhem_heat");
    DWORD mt = 0xFF000000u | (DWORD)(t.r * 255.0f + 0.5f) << 16 | (DWORD)(t.g * 255.0f + 0.5f) << 8 | (DWORD)(t.b * 255.0f + 0.5f);
    if (mt != g_map_tint) { g_map_tint = mt; hud_set_map_tint(mt); }
    bool want_icons = g_c.icons && (g_s.heat > 0.0f || g_s.shown > 0.01f);
    float target = want_icons ? 1.0f : 0.0f;
    g_icons_a += (target - g_icons_a) * (dt * 4.0f > 1.0f ? 1.0f : dt * 4.0f);
    if (fabsf(target - g_icons_a) < 0.01f) g_icons_a = target;
    ht::Ring ring = g_c.pulse ? ht::edge_ring(g_s.shown, g_c.h.max_level, now) : ht::Ring{0, 0, 0, 0};
    if (ring.a <= 0.003f && g_icons_a <= 0.003f && g_lab_a <= 0.003f) return;
    if (!make_texture(f.dev)) return;
    ht::Aff m;
    if (!ht::uv_affine(f.P, f.UV, &m)) return;
    IDirect3DDevice9* d = f.dev;
    d->SetTexture(0, g_tex);
    float op = f.opacity;
    // the rim: ring texture (atlas HEAT_RING_* = radar UV 0..1), red <-> dark
    if (ring.a > 0.003f) quad_uv(d, m, 0, 0, 1, 1, (float)HEAT_RING_X, (float)HEAT_RING_Y, (float)(HEAT_RING_X + HEAT_RING_N),
                                 (float)(HEAT_RING_Y + HEAT_RING_N), pm_colour(ring.r * op, ring.g * op, ring.b * op, ring.a * op));
    // chilis: slot i (0 = the top, lights first) shows flame level i + 1 once lit, the darkened chili while not. Drawn
    // top slot first, so each lower chili's flame lies over the chili above it. A slot that is filling lights from the
    // bottom of its cell up, with a soft band where the cold chili hands over to the burning one.
    if (g_icons_a > 0.003f) {
        int n = g_c.h.max_level;
        const float band = 0.10f;                        // the soft transition band, in cell heights
        for (int i = 0; i < n; ++i) {
            float u, v, s; ht::icon_place(i, n, g_c.icon_scale, &u, &v, &s);
            float fill = ht::icon_fill(g_c.h, g_s.heat, i);
            if (fill >= 1.0f) { if (g_full_t[i] <= 0.0f) g_full_t[i] = (float)now; } else g_full_t[i] = 0.0f;
            float pop = 1.0f;                            // a chili that just lit pops (0.35 s)
            if (g_full_t[i] > 0.0f) { float k = (float)(now - g_full_t[i]) / 0.35f; if (k < 1.0f) pop = 1.0f + 0.22f * sinf(k * 3.14159f); }
            float sq = s * CHILI_SQUARE * pop;
            float a = g_icons_a * op;
            int lit = i + 1 < 5 ? i + 1 : 5;
            if (fill <= 0.0f) { chili(d, m, 0, u, v, sq, 0.0f, 1.0f, a, a, 1.0f); continue; }
            float glow = ht::icon_glow(g_s, i, now);
            if (fill >= 1.0f) { chili(d, m, lit, u, v, sq, 0.0f, 1.0f, a, a, glow); continue; }
            float yt = (1.0f + band) - fill * (1.0f + 2.0f * band);   // the band's bottom edge: below the cell at 0, above it at 1
            float yb = yt - band;
            chili(d, m, 0, u, v, sq, 0.0f, yb < 0.0f ? 0.0f : yb, a, a, 1.0f);            // cold above the band
            chili(d, m, lit, u, v, sq, yt > 1.0f ? 1.0f : yt, 1.0f, a, a, glow);           // burning below it
            float b0 = yb < 0.0f ? 0.0f : yb, b1 = yt > 1.0f ? 1.0f : yt;                   // the band: cross-fade
            if (b1 > b0) {
                float t0 = (b0 - yb) / band, t1 = (b1 - yb) / band;                        // 0 at the band top, 1 at its bottom
                chili(d, m, 0, u, v, sq, b0, b1, a * (1.0f - t0), a * (1.0f - t1), 1.0f);
                chili(d, m, lit, u, v, sq, b0, b1, a * t0, a * t1, glow);
            }
        }
    }
    draw_label(d, m, op, now, dt);
    QueryPerformanceCounter(&q1);
    double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / fr.QuadPart;
    if (!g_logged_draw) {
        float x, y; ht::aff_apply(m, ht::DISC_U, ht::DISC_V, &x, &y);
        gp_log("heat: first radar draw: disc centre (%.1f, %.1f) px, %.2f px per radar texel, opacity %.2f, %.3f ms\n", x, y,
               sqrtf(m.a * m.a + m.c * m.c) / 128.0f, op, ms);
        g_logged_draw = true; return;
    }
    g_pf_sum += ms; if (ms > g_pf_max) g_pf_max = ms; ++g_pf_n;
    if (g_pf_last <= 0.0) g_pf_last = now;
    if (g_c.perf_s > 0.0f && now - g_pf_last >= g_c.perf_s && g_pf_n) {
        gp_log("heat: HUD %d frames, cost avg %.4f ms max %.4f ms (shown level %.2f, ring %.2f, icons %.2f)\n", g_pf_n, g_pf_sum / g_pf_n, g_pf_max, g_s.shown, ring.a, g_icons_a);
        g_pf_last = now; g_pf_sum = g_pf_max = 0.0; g_pf_n = 0;
    }
}

// ================================================================================================ install
int heat_install(bool tick_ok) {
    if (!g_c.enabled) return 0;
    if (!tick_ok) { gp_log("heat: no input tick: [mayhem_heat] OFF\n"); return 0; }
    static const char* refs[] = {"HT_KILLOWNER", "HT_KILLER_RAM", "HT_DESTR_EXPL", "HT_SHELL_FLAG", "HT_LOCO_RUN", "HT_LOCO_IDLE",
                                 "G_COMBO", "G_COMBO_PTS", "G_KILLS_R", "PH_SHELLS", "SV_CLASSOBJ", "SV_CLS_ROOT", "SV_CLS_SENTRY",
                                 "SM_RD_ROOT", "SM_ROOT_AI", "SV_QUERY_A", "SV_QUERY_FREE", "F_QUERY", "F_LISTFREE", "DF_GETPOS", "F_TOUCH", "F_SENDMSG"};
    for (const char* r : refs) if (!gp_ref_ok(r)) { gp_log("heat: reference %s differs: [mayhem_heat] OFF\n", r); return 0; }
    const char* sites[] = {"HT_KILL_E", "HT_KILL_N", "HT_DESTROY", "HT_CAR"};
    for (const char* s : sites) if (!gp_site_ok(s)) { gp_log("heat: site %s holds other bytes (another mod?): [mayhem_heat] OFF\n", s); return 0; }
    int n = 0;
    n += gp_patch_call("HT_KILL_E", (void*)&KillHostileStub) ? 1 : 0;
    n += gp_patch_call("HT_KILL_N", (void*)&KillNeutralStub) ? 1 : 0;
    n += gp_patch_call("HT_DESTROY", (void*)&DestroyStub) ? 1 : 0;
    n += gp_patch_call("HT_CAR", (void*)&CarStub) ? 1 : 0;
    g_hooks_ok = n == 4;
    bool score_on = false;
    if (g_c.score) {
        if (!gp_ref_ok("HT_SCORE_ADD")) gp_log("heat: reference HT_SCORE_ADD differs: no score multiplier\n");
        else if (!gp_site_ok("HT_SCORE")) gp_log("heat: site HT_SCORE holds other bytes (another mod?): no score multiplier\n");
        else score_on = gp_patch_call("HT_SCORE", (void*)&ScoreWrap);
        if (!score_on) g_c.score = false;
    }
    g_installed = true;
    if ((g_c.tint > 0.0f || g_c.icons || g_c.pulse)) {
        if (gp_radar_stage_install("mayhem_heat") && gp_radar_add_draw(&draw_radar, 10, "mayhem_heat")) g_hud = true;
        else gp_log("heat: the RADAR stage is not available: no radar tint / icons (heat and escalation still run)\n");
    }
    gp_log("heat: ON - %d event sites, HUD %s (tint %.2f, icons %d, pulse %d, icon scale %.2f), levels %d, thresholds %.0f/%.0f/%.0f/%.0f/%.0f, "
           "cooling %.0f s per level after %.0f s calm, escalation %s (scale %.2f x difficulty), rooftops >= %.1f m, score multiplier %s (x%.2f at the top, label %d)\n",
           n, g_hud ? "on" : "off", g_c.tint, g_c.icons ? 1 : 0, g_c.pulse ? 1 : 0, g_c.icon_scale, g_c.h.max_level, g_c.h.thr[0], g_c.h.thr[1],
           g_c.h.thr[2], g_c.h.thr[3], g_c.h.thr[4], g_c.h.decay_seconds, g_c.h.calm_seconds, g_c.escalate ? "on" : "off", g_c.scale, g_c.high_m,
           score_on ? "on" : "off", g_c.score_top, g_c.score_label ? 1 : 0);
    return n;
}

// ---- test host only
static void __cdecl TestRedirect(DWORD touch, DWORD sendmsg, DWORD back) { R_HT_TOUCH = touch; R_HT_SENDMSG = sendmsg; R_HT_DESTROY_BACK = back; }
static void __cdecl TestScore(DWORD body, int level, float top) { R_HT_SCORE = body; g_installed = true; g_c.score = true; g_c.log = false; g_c.score_top = top; g_c.h.max_level = 5; g_s.level = level; }
static LONG __cdecl TestQueued() { return g_qn; }
static int __cdecl TestQueuedEv(int i) { return i >= 0 && i < g_qn ? g_q[i].ev : -1; }
void* heat_test_stub(const char* name) {
    if (!strcmp(name, "ht_kill_e")) return (void*)&KillHostileStub;
    if (!strcmp(name, "ht_kill_n")) return (void*)&KillNeutralStub;
    if (!strcmp(name, "ht_destroy")) return (void*)&DestroyStub;
    if (!strcmp(name, "ht_car")) return (void*)&CarStub;
    if (!strcmp(name, "ht_redirect")) return (void*)&TestRedirect;
    if (!strcmp(name, "ht_queued")) return (void*)&TestQueued;
    if (!strcmp(name, "ht_queued_ev")) return (void*)&TestQueuedEv;
    if (!strcmp(name, "ht_score")) return (void*)&ScoreWrap;
    if (!strcmp(name, "ht_score_setup")) return (void*)&TestScore;
    return nullptr;
}
