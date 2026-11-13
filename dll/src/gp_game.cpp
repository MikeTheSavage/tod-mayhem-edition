// gp_game.cpp - TODGameplay.dll: modern quality-of-life gameplay options for the GOG TOD.exe (Total Overdose, 2005,
// 6,860,800 bytes, md5 d9beb351b3c78ee9f14a766a368f3ff9). Game glue: identity check, ini, patches, stubs, the per-frame
// and input-tick work, shared memory. The decisions live in gp_logic.cpp (unit tested).
//
// Rules this file follows (from the bridge / proxy labs): no global objects with constructors that touch the engine, no
// engine classes, /MT CRT, every patch site's shipped bytes checked first (sites.h); a feature whose sites do not all
// match stays off and is logged; with no ini, or every feature off, NOTHING is written to the game's memory.
// Loading: dsound.dll proxy (loader/dsound_proxy.cpp) or any LoadLibraryA("TODGameplay.dll") in the game process; all
// the work happens once, in TODGameplay_Init (also run from DllMain).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <share.h>
#include "gp_logic.h"
#include "todgameplay_shared.h"
#include "gp_glue.h"

#define GP_BUILD 2

// ================================================================================================ sites
enum Kind { CALL, JMP, OPER, DATA };   // DATA: a pointer in a data section (vtable slot), replaced like OPER
struct Site { const char* id; const char* feature; DWORD va; const char* hex; int off; int len; Kind kind; const char* what; };
struct Ref { const char* id; DWORD va; const char* hex; const char* why; };
#define SITE(id, feature, va, hex, off, len, kind, what) { #id, feature, va, hex, off, len, kind, what },
#define REF(id, va, hex, why)
static const Site g_sites[] = {
#include "sites.h"
};
#undef SITE
#undef REF
#define SITE(id, feature, va, hex, off, len, kind, what)
#define REF(id, va, hex, why) { #id, va, hex, why },
static const Ref g_refs[] = {
#include "sites.h"
};
#undef SITE
#undef REF
static const int NSITES = sizeof(g_sites) / sizeof(g_sites[0]);
static const int NREFS = sizeof(g_refs) / sizeof(g_refs[0]);

// ================================================================================================ log
static char g_dir[MAX_PATH];
static int g_log = 1;
static void lg(const char* fmt, ...) {
    if (!g_log) return;
    char p[MAX_PATH + 32]; sprintf_s(p, "%s\\TODGameplay.log", g_dir);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fclose(f);
}

// ================================================================================================ memory helpers
static int hexbytes(const char* hex, BYTE* out, int cap) {
    int n = 0;
    for (; hex[0] && hex[1] && n < cap; hex += 2) {
        unsigned b; if (sscanf_s(hex, "%2x", &b) != 1) return -1;
        out[n++] = (BYTE)b;
    }
    return n;
}
static bool mem_equal(DWORD va, const char* hex) {
    BYTE b[64]; int n = hexbytes(hex, b, sizeof b);
    if (n <= 0) return n == 0;
    __try { return memcmp((const void*)(uintptr_t)va, b, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool write_mem(DWORD va, const void* src, int n) {
    DWORD op;
    if (!VirtualProtect((void*)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &op)) return false;
    memcpy((void*)(uintptr_t)va, src, n);
    VirtualProtect((void*)(uintptr_t)va, n, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)va, n);
    return true;
}
static const Site* site(const char* id) {
    for (int i = 0; i < NSITES; ++i) if (!strcmp(g_sites[i].id, id)) return &g_sites[i];
    return nullptr;
}
static bool ref_ok(const char* id) {
    for (int i = 0; i < NREFS; ++i) if (!strcmp(g_refs[i].id, id)) return mem_equal(g_refs[i].va, g_refs[i].hex);
    return false;
}
static bool site_ok(const char* id) { const Site* s = site(id); return s && mem_equal(s->va, s->hex); }
static DWORD site_addr(const char* id) { const Site* s = site(id); return s ? s->va + s->off : 0; }

static int g_patched = 0;
// CALL: retarget the call rel32; JMP: jmp rel32 + nops
static bool patch_site(const char* id, void* to) {
    const Site* s = site(id);
    if (!s || !mem_equal(s->va, s->hex)) { lg("site %s: shipped bytes differ, NOT patched\n", id); return false; }
    DWORD at = s->va + s->off;
    BYTE b[16];
    b[0] = (s->kind == CALL) ? 0xE8 : 0xE9;
    *(DWORD*)(b + 1) = (DWORD)(uintptr_t)to - (at + 5);
    for (int i = 5; i < s->len; ++i) b[i] = 0x90;
    if (!write_mem(at, b, s->len)) { lg("site %s: VirtualProtect failed\n", id); return false; }
    ++g_patched;
    lg("site %-10s %06X %s -> %p (%s)\n", id, at, s->kind == CALL ? "call" : "jmp ", to, s->what);
    return true;
}
static bool patch_oper(const char* id, const BYTE* bytes, int n) {
    const Site* s = site(id);
    if (!s || !mem_equal(s->va, s->hex)) { lg("site %s: shipped bytes differ, NOT patched\n", id); return false; }
    if (n != s->len) return false;
    if (!write_mem(s->va + s->off, bytes, n)) return false;
    ++g_patched;
    lg("site %-10s %06X operand patched (%s)\n", id, s->va + s->off, s->what);
    return true;
}

// ================================================================================================ game addresses
#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
static const DWORD A_CACHE = 0xA3B58C, A_SCENE = 0xA3DCBC, A_KBOBJ = 0xA35E80, A_MSOBJ = 0xA35EAC;
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_PLAYER_HOLDER = 0xA3B590;
static const DWORD P_SENDMSG = 0x869E60, P_TOUCH = 0x86B4B0, P_PADPOLL = 0x43A190, P_INACTIVE = 0x43BC30;
static const DWORD P_KBPROC = 0x73F7E0, P_PADPROC = 0x73DC70;
static const DWORD C_TIMECTRL = 0x280, C_MISSION = 0x25C, C_COMBO = 0x268, C_MASTER = 0x254;
static const DWORD T_WORLD = 0x850, T_DESIRED = 0x854, T_TTU = 0x85C, T_EXCL = 0x864;
static const int   M_WORLD = 0x1F4;   // member index of WorldTime for the notify

typedef char(__fastcall* SendMsg_t)(void* node, void* edx, int id, void* args);
typedef void(__fastcall* Touch_t)(void* node, void* edx, int member);
static DWORD cache_data() { DWORD n = RD32(A_CACHE); return n ? RD32(n + 0x18) : 0; }

// ================================================================================================ config
struct Cfg {
    gp::BtCfg bt; gp::MeterCfg meter; bool start_full = true; bool dive_in_bt = false;
    int bt_key = gp::KEY_NONE, bt_key2 = gp::KEY_NONE, bt_pad = gp::KEY_NONE;
    bool dive_slowmo_off = false, wire_slowmo_off = false;
    bool clock_fix = false, pickup_fix = false; int fps_limit = 0;
    bool mouse_turn = false; float mouse_spf = 0.95f / 60.0f; float sens_x = 1.0f, sens_y = 1.0f;
    bool invert_y = false, y_fps_norm = false, diagonal = false;
    float fov = 0.0f; bool fov_all = false, cull_fix = false;
    bool restart = false; int restart_key = gp::KEY_NONE; float restart_hold = 1.0f;
    float steer_return = 0.0f; bool brake_parity = false;
    gp::KeyRemap remap;
};
static Cfg g_cfg;
static gp::Ini g_ini;

static int key_cfg(const char* sec, const char* k, const char* def) {
    const char* v = g_ini.get(sec, k);
    int c = gp::key_from_name(v ? v : def);
    if (c == -2) { lg("ini [%s] %s = %s: unknown key name, ignored\n", sec, k, v); c = gp::KEY_NONE; }
    return c;
}
static void load_config() {
    Cfg& c = g_cfg;
    g_log = g_ini.geti("general", "log", 1);
    c.bt.enabled = g_ini.geti("bullettime", "enabled", 0) != 0;
    const char* mode = g_ini.get("bullettime", "mode");
    c.bt.hold_mode = mode && !_stricmp(mode, "hold");
    c.bt.scale = g_ini.getf("bullettime", "scale", 0.30f);
    if (c.bt.scale < 0.05f) c.bt.scale = 0.05f; if (c.bt.scale > 0.9f) c.bt.scale = 0.9f;
    c.bt.ramp_in = g_ini.getf("bullettime", "ramp_in", 0.25f);
    c.bt.ramp_out = g_ini.getf("bullettime", "ramp_out", 0.25f);
    c.bt.blur = g_ini.geti("bullettime", "motion_blur", 1) != 0;
    c.dive_in_bt = g_ini.geti("bullettime", "dive_slowmo_in_bullettime", 0) != 0;
    c.bt_key = key_cfg("bullettime", "key", "MOUSE3");
    c.bt_key2 = key_cfg("bullettime", "key_alt", "NONE");
    c.bt_pad = key_cfg("bullettime", "pad_button", "NONE");
    c.meter.capacity_s = g_ini.getf("bullettime", "meter_seconds", 8.0f);
    c.meter.min_start = g_ini.getf("bullettime", "meter_min_start", 0.10f);
    c.meter.regen_per_s = g_ini.getf("bullettime", "regen_per_second", 0.02f);
    c.meter.regen_delay_s = g_ini.getf("bullettime", "regen_delay", 2.0f);
    c.meter.per_kill = g_ini.getf("bullettime", "refill_per_kill", 0.12f);
    c.meter.per_point = g_ini.getf("bullettime", "refill_per_combo_point", 0.0005f);
    c.start_full = g_ini.geti("bullettime", "start_full", 1) != 0;
    c.dive_slowmo_off = g_ini.geti("slowmo", "dive_auto_slowmo", 1) == 0;
    c.wire_slowmo_off = g_ini.geti("slowmo", "wire_auto_slowmo", 1) == 0;
    c.clock_fix = g_ini.geti("timing", "clock_fix", 0) != 0;
    c.pickup_fix = g_ini.geti("timing", "pickup_step_fix", 0) != 0;
    c.fps_limit = g_ini.geti("timing", "fps_limit", 0);
    if (c.fps_limit < 0) c.fps_limit = 0; if (c.fps_limit > 1000) c.fps_limit = 1000;
    if (c.fps_limit > 0 && c.fps_limit < 20) c.fps_limit = 20;
    c.mouse_turn = g_ini.geti("controls", "mouse_turn_fix", 0) != 0;
    c.mouse_spf = g_ini.getf("controls", "mouse_turn_seconds_per_frame", 0.95f / 60.0f);
    c.sens_x = g_ini.getf("controls", "mouse_sensitivity_x", 1.0f);
    c.sens_y = g_ini.getf("controls", "mouse_sensitivity_y", 1.0f);
    c.invert_y = g_ini.geti("controls", "invert_y", 0) != 0;
    c.y_fps_norm = g_ini.geti("controls", "mouse_y_fps_normalize", 0) != 0;
    c.diagonal = g_ini.geti("controls", "diagonal_normalize", 0) != 0;
    c.fov = g_ini.getf("camera", "fov", 0.0f);
    if (c.fov != 0.0f && (c.fov < 30.0f || c.fov > 120.0f)) { lg("camera fov %.1f outside 30..120: off\n", c.fov); c.fov = 0.0f; }
    if (c.fov == 64.0f) c.fov = 0.0f;
    c.fov_all = g_ini.geti("camera", "fov_all_cameras", 0) != 0;
    c.cull_fix = g_ini.geti("camera", "cull_fix", 0) != 0;
    c.restart = g_ini.geti("restart", "enabled", 0) != 0;
    c.restart_key = key_cfg("restart", "key", "F10");
    c.restart_hold = g_ini.getf("restart", "hold_seconds", 1.0f);
    c.steer_return = g_ini.getf("vehicles", "steering_return_rate", 0.0f);
    if (c.steer_return < 0.0f) c.steer_return = 0.0f; if (c.steer_return > 1.0f) c.steer_return = 1.0f;
    c.brake_parity = g_ini.geti("vehicles", "s_brake_parity", 0) != 0;
    suave_load_config();
    smart_load_config();
    gw_load_config();                            // [gang_wars] (gang_wars/code/gw_game.cpp)
    seam_load_config();
    phys_load_config();                          // [physics] (physics/code/phys_game.cpp)
    mprops_load_config();                        // [mayhem] chain reactions / breakable cover (mayhem/code/mh_game.cpp)
    gp_fix_load_config();
    display_load_config();                       // [display] (dll/src/gp_display.cpp)
    pad_load_config();                           // [pad] controller support (dll/src/gp_pad.cpp)
    difficulty_load_config();                    // [difficulty] (dll/src/gp_difficulty.cpp)
    mayhem_load_config();                        // [mayhem_title] (dll/src/gp_mayhem.cpp)
    psycho_load_config();                        // [psycho] PSYCHO enemy loco moves (psycho/code/psycho_game.cpp)
    loot_load_config();                          // [drops] more pickups from dead enemies (drops/code/drops_game.cpp)
    hud_load_config();                           // [minimap] (dll/src/gp_hud.cpp)
    heat_load_config();                          // [mayhem_heat] (dll/src/gp_heat.cpp)
    bf_load_config();                            // [bigger_fights] (dll/src/gp_bigfights.cpp)
    if (display_wants_cull() && !c.cull_fix) { c.cull_fix = true; lg("display: cull_fix on (culling frustum with the real aspect)\n"); }
    // [keys]: game key (DIK name or action name) = physical key
    c.remap.clear();
    for (int i = 0; i < g_ini.n; ++i) {
        const gp::IniEntry& e = g_ini.e[i];
        if (_stricmp(e.section, "keys")) continue;
        int g = gp::action_default_key(e.key);
        if (g < 0) g = gp::key_from_name(e.key);
        int s = gp::key_from_name(e.value);
        if (g < 0 || g > 255 || s == -2 || !c.remap.bind(g, s)) { lg("ini [keys] %s = %s: not a keyboard game key / known source, ignored\n", e.key, e.value); continue; }
        lg("key remap: game %s <- %s\n", gp::key_name(g), gp::key_name(s));
    }
}

// ================================================================================================ state
static volatile LONG g_inited = 0;
static DWORD g_status = 0;                 // TODGP_F_* flags
static bool g_patch_allowed = false;       // exe identity confirmed (or the test host's simulated image)
static HANDLE g_map = nullptr; static TodgpShared* g_sh = nullptr;
static LARGE_INTEGER g_qpf;
static double now_s() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)g_qpf.QuadPart; }

static gp::Meter g_meter;
static gp::BtLogic g_bt;
static bool g_bt_want = false;             // toggle state
static volatile LONG g_bt_press = 0;       // press latched by PostInput, consumed by the input tick
static bool g_bt_held = false;
static double g_last_tick = 0.0, g_last_post = 0.0;
static int g_last_kills = -1, g_last_points = -1;
static DWORD g_kills_total = 0, g_bt_uses = 0, g_frame = 0;
static bool g_restart_held = false; static double g_restart_since = 0.0; static bool g_restart_fired = false;
static bool g_need_post = false, g_need_tick = false;
static bool g_mouse_prev[8];
volatile BYTE g_suppress_dive = 0, g_suppress_wire = 0;   // read by the arm stubs
float g_mx_factor = 1.0f, g_my_factor = 1.0f;              // read by the mouse stubs
static bool g_ok_cutscene_ref = false, g_ok_kills_ref = false, g_ok_combo_ref = false;

// ================================================================================================ XInput (lazy, only when a PAD_ key is configured)
typedef struct { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGP;
typedef struct { DWORD dwPacketNumber; XGP Gamepad; } XST;
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XST*);
static XInputGetState_t g_xget = nullptr; static bool g_xtried = false; static double g_xretry = 0.0; static bool g_xconnected = false;
static WORD pad_buttons(double now) {
    if (!g_xtried) {
        g_xtried = true;
        const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char* d : dlls) { HMODULE m = LoadLibraryA(d); if (m) { g_xget = (XInputGetState_t)GetProcAddress(m, "XInputGetState"); if (g_xget) { lg("gamepad: %s\n", d); break; } } }
    }
    if (!g_xget) return 0;
    if (!g_xconnected && now < g_xretry) return 0;   // a missing pad is slow to query: retry every 2 s
    XST s; memset(&s, 0, sizeof s);
    if (g_xget(0, &s) != 0) { g_xconnected = false; g_xretry = now + 2.0; return 0; }
    g_xconnected = true;
    return s.Gamepad.wButtons;
}

// ================================================================================================ input reading
struct Phys { bool ok; BYTE edges[256]; BYTE state[256]; bool mdown[8]; WORD pad; };
static bool code_down(const Phys& p, int code) {
    if (code < 0) return false;
    if (code < 256) return (p.state[code] & 0x80) != 0;
    if (code < gp::KEY_MOUSE + 8 && code >= gp::KEY_MOUSE) return p.mdown[code - gp::KEY_MOUSE];
    if (code >= gp::KEY_PAD && code < gp::KEY_PAD + 16) return (p.pad >> (code - gp::KEY_PAD)) & 1;
    return false;
}

// ================================================================================================ the game's own calls
// Messages are sent with the game's Node::SendMessage (0x869E60). The script "current node" globals that the dispatch
// sets are put back afterwards, so the caller (the input procedure's state) sees what it saw before.
static bool send_msg(DWORD node, int id, void* args) {
    if (!node) return false;
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    bool ok = true;
    __try { ((SendMsg_t)(uintptr_t)P_SENDMSG)((void*)(uintptr_t)node, nullptr, id, args); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    *(volatile DWORD*)(uintptr_t)A_CUR_NODE = s1; *(volatile DWORD*)(uintptr_t)A_CUR_2 = s2; *(volatile DWORD*)(uintptr_t)A_CUR_3 = s3;
    return ok;
}
static bool set_world_time(float w) {
    __try {
        DWORD cn = RD32(A_CACHE); if (!cn) return false;
        DWORD cd = RD32(cn + 0x18); if (!cd) return false;
        ((Touch_t)(uintptr_t)P_TOUCH)((void*)(uintptr_t)cn, nullptr, M_WORLD);   // what change_world_time does first
        *(volatile float*)(uintptr_t)(cd + T_WORLD) = w;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct SlowArgs { float target; float seconds; int no_blur; };
static void perform(const gp::BtActions& a, const char* why) {
    DWORD cd = 0; DWORD tc = 0;
    __try { cd = cache_data(); tc = cd ? RD32(cd + C_TIMECTRL) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { tc = 0; }
    if (a.set_world_time) { bool ok = set_world_time(a.world_time); lg("BT %s: WorldTime -> %.3f%s\n", why, a.world_time, ok ? "" : " (FAILED)"); }
    if (a.send_slowmo && tc) {
        SlowArgs s = {a.slowmo_target, a.slowmo_seconds, a.slowmo_no_blur};
        bool ok = send_msg(tc, -8, &s);   // command_trigger_slowmotion(number,number,truth)
        lg("BT %s: trigger_slowmotion(%.3f, %.2f s, no_blur %d)%s\n", why, s.target, s.seconds, s.no_blur, ok ? "" : " (FAILED)");
    }
    if (a.send_force_normal && tc) {
        bool ok1 = send_msg(tc, -6, nullptr);   // command_force_normal_time (normal state)
        bool ok2 = send_msg(tc, -5, nullptr);   // command_force_normal_time (automatic slow-motion state)
        lg("BT %s: force_normal_time%s\n", why, ok1 && ok2 ? "" : " (FAILED)");
    }
}
static gp::BtGame read_game(bool in_tick) {
    gp::BtGame g;
    __try {
        DWORD cd = cache_data(); if (!cd) return g;
        DWORD scene = RD32(A_SCENE); if (!scene) return g;
        if (!RD32(cd + C_TIMECTRL)) return g;
        g.world_time = RDF(cd + T_WORLD); g.desired = RDF(cd + T_DESIRED); g.time_to_use = RDF(cd + T_TTU);
        g.exclusive = RD32(cd + T_EXCL) != 0;
        g.time_mult = RDF(scene + 0x114);
        if (g_ok_cutscene_ref) {   // MASTER_CTRL ->+30h ->+D4h: the test the explosion slow motion uses (0x7856A0)
            DWORD m = RD32(cd + C_MASTER); DWORD md = m ? RD32(m + 0x18) : 0; DWORD x = md ? RD32(md + 0x30) : 0;
            DWORD xd = x ? RD32(x + 0x18) : 0; g.cutscene = xd ? RD32(xd + 0xD4) != 0 : false;
        }
        g.ok = in_tick;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g.ok = false; }
    return g;
}
static int read_playmode() { __try { DWORD s = RD32(A_SCENE); return s ? (int)RD32(s + 0x5C) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }

// ================================================================================================ input tick (script context)
static void update_suppress() {
    bool bt_on = g_bt.state != gp::BT_OFF;
    g_suppress_dive = (BYTE)(g_cfg.dive_slowmo_off || (bt_on && !g_cfg.dive_in_bt));
    g_suppress_wire = (BYTE)(g_cfg.wire_slowmo_off || (bt_on && !g_cfg.dive_in_bt));
}
static void input_tick(void* thread) {
    __try { if (thread && RD32((DWORD)(uintptr_t)thread + 0x3C)) return; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }   // state change pending
    double now = now_s();
    float dt = g_last_tick > 0.0 ? (float)(now - g_last_tick) : 0.0f;
    if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    g_last_tick = now;
    if (g_cfg.bt.enabled) {
        if (InterlockedExchange(&g_bt_press, 0)) g_bt_want = !g_bt_want;
        bool want = g_cfg.bt.hold_mode ? g_bt_held : g_bt_want;
        gp::BtGame g = read_game(true);
        gp::BtState before = g_bt.state;
        gp::BtActions a = g_bt.tick(g_cfg.bt, g_cfg.meter, g_meter, g, want, dt);
        if (before == gp::BT_OFF && g_bt.state == gp::BT_RAMP_IN) ++g_bt_uses;
        perform(a, before == gp::BT_OFF ? "on" : (g_bt.state == gp::BT_OFF ? "off" : "latch"));
        if (g_bt.state == gp::BT_OFF && before != gp::BT_OFF) { g_bt_want = false; if (!a.send_slowmo) lg("BT ended by the game (WorldTime %.3f)\n", g.world_time); }
        update_suppress();
    }
    if (g_cfg.restart && g_restart_held && !g_restart_fired && now - g_restart_since >= g_cfg.restart_hold) {
        g_restart_fired = true;
        __try {
            DWORD cd = cache_data(); DWORD mc = cd ? RD32(cd + C_MISSION) : 0; DWORD md = mc ? RD32(mc + 0x18) : 0;
            int cur = md ? (int)RD32(md + 0x38) : -1;
            if (!gp::restart_allowed(cur)) lg("quick restart: mission %d has no retry screen (intermission, prologue or tutorial): ignored\n", cur);
            else {
                if (g_bt.state != gp::BT_OFF) { gp::BtActions off; off.set_world_time = g_bt.state == gp::BT_ACTIVE; off.world_time = 1.0f; off.send_force_normal = true; perform(off, "restart"); g_bt.state = gp::BT_OFF; g_bt_want = false; update_suppress(); }
                int result = 0;
                bool ok = send_msg(mc, -10, &result);   // Command_End_Mission(0): the game's failed-mission path -> result screen (retry)
                lg("quick restart: Command_End_Mission(0) sent for mission %d%s\n", cur, ok ? "" : " (FAILED)");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { lg("quick restart: read failed\n"); }
    }
    // hang_where: the step a [debug] hang_watchdog report names when the game thread stops inside it
    hang_where("input tick: suave_input_tick"); suave_input_tick(thread, now);
    hang_where("input tick: smart_tick"); smart_tick(thread, now);
    hang_where("input tick: gw_tick"); gw_tick(thread, now);     // [gang_wars]: rival gangs, the faction matrix
    hang_where("input tick: seam_tick"); seam_tick(thread, now);
    hang_where("input tick: phys_tick"); phys_tick(thread, now);   // [physics]: scan, step, write the props back
    hang_where("input tick: mprops_tick"); mprops_tick(thread, now);   // [mayhem]: chain reactions, breakable cover, debris (after the physics step)
    hang_where("input tick: heat_tick"); heat_tick(thread, now);         // [mayhem_heat]: events, cooling, wave requests (before the engine below reads them)
    hang_where("input tick: bf_tick"); bf_tick(thread, now);           // [bigger_fights]: extra enemies, heat vehicles
    hang_where("input tick: difficulty_tick"); difficulty_tick(thread, now);   // [difficulty] tuning / BRUTAL reinforcements / heat waves
    hang_where("input tick: psycho_tick"); psycho_tick(thread, now);       // [psycho] enemy loco moves (PSYCHO)
    hang_where("input tick: loot_tick"); loot_tick(thread, now);         // [drops] dead enemies' pickups, dropped ammo
    hang_where("input tick: gp_debug_tick"); gp_debug_tick(now);   // [debug] stress swaps (test copy only)
    hang_where("game (after the input tick)");
}
typedef void(__cdecl* Proc_t)(void* thread, int zero);
static void __cdecl KbProcWrap(void* thread, int zero) { ((Proc_t)(uintptr_t)P_KBPROC)(thread, zero); input_tick(thread); }
static void __cdecl PadProcWrap(void* thread, int zero) { ((Proc_t)(uintptr_t)P_PADPROC)(thread, zero); input_tick(thread); }

// ================================================================================================ post-input (main loop, every pass)
static void publish(const gp::BtGame& g, int playmode) {
    if (!g_sh) return;
    g_sh->seq++;
    MemoryBarrier();
    DWORD f = g_status;
    if (g_bt.state != gp::BT_OFF) f |= TODGP_F_BT_RUNNING;
    if (g.cutscene) f |= TODGP_F_CUTSCENE;
    if (playmode == 2) f |= TODGP_F_PAUSED;
    if (g_last_tick > 0.0 && now_s() - g_last_tick < 0.25) f |= TODGP_F_IN_CONTROL;
    g_sh->flags = f; g_sh->frame = g_frame;
    g_sh->meter = g_meter.value; g_sh->meter_seconds = g_meter.value * g_cfg.meter.capacity_s;
    g_sh->bt_state = (uint32_t)g_bt.state; g_sh->bt_scale = g_cfg.bt.scale;
    g_sh->time_mult = g.time_mult; g_sh->world_time = g.world_time;
    g_sh->kills = g_kills_total; g_sh->bt_uses = g_bt_uses; g_sh->bt_game_ends = (uint32_t)g_bt.ended_by_game;
    suave_publish(g_sh);
    MemoryBarrier();
    g_sh->seq++;
}
static void post_input() {
    hang_beat();   // [debug] hang_watchdog: the heartbeat of the game thread (one per main-loop pass)
    hang_where("POST_INPUT: hotkeys / remap / key script / pad / minimap");
    ++g_frame;
    double now = now_s();
    float dt = g_last_post > 0.0 ? (float)(now - g_last_post) : 0.0f;
    if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    g_last_post = now;
    // physical input (before any remap)
    Phys p; memset(&p, 0, sizeof p);
    BYTE* kb = nullptr; DWORD* ms = nullptr;
    __try {
        kb = (BYTE*)(uintptr_t)RD32(A_KBOBJ); ms = (DWORD*)(uintptr_t)RD32(A_MSOBJ);
        if (kb) { memcpy(p.edges, kb, 256); memcpy(p.state, kb + 0x100, 256); p.ok = true; }
        if (ms) for (int b = 0; b < 8; ++b) p.mdown[b] = (ms[b] & 1) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { kb = nullptr; ms = nullptr; p.ok = false; }
    bool want_pad = (g_cfg.bt_pad >= gp::KEY_PAD);
    if (want_pad) p.pad = pad_enabled() ? pad_xinput_buttons() : pad_buttons(now);
    // hotkeys
    if (g_cfg.bt.enabled) {
        bool held = code_down(p, g_cfg.bt_key) || code_down(p, g_cfg.bt_key2) || code_down(p, g_cfg.bt_pad);
        if (held && !g_bt_held) InterlockedExchange(&g_bt_press, 1);
        else if (g_last_tick < g_last_post - dt - 0.001) InterlockedExchange(&g_bt_press, 0);   // not in control: drop stale presses
        g_bt_held = held;
    }
    if (g_cfg.restart) {
        bool held = code_down(p, g_cfg.restart_key);
        if (held && !g_restart_held) { g_restart_since = now; g_restart_fired = false; }
        g_restart_held = held;
    }
    // key rebinding on the game's own keyboard arrays
    if (g_cfg.remap.any && kb && p.ok) {
        __try { g_cfg.remap.apply(kb, kb + 0x100, p.mdown, g_mouse_prev); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (kb && p.ok) gp_debug_post_input(kb, kb + 0x100, now);   // [debug] key script (test copy only)
    if (pad_enabled()) pad_post_input(p.ok ? kb : nullptr, ms, now, dt);   // [pad]: devices, device used last, prompts, rumble
    memcpy(g_mouse_prev, p.mdown, sizeof g_mouse_prev);
    if (hud_enabled()) hud_post_input(now);                  // [minimap]: radar blips and their targets
    if (g_cfg.y_fps_norm || g_cfg.invert_y || g_cfg.sens_y != 1.0f)
        g_my_factor = gp::mouse_y_factor(g_cfg.invert_y, g_cfg.sens_y, g_cfg.y_fps_norm, dt > 0.0f ? dt : 1.0f / 60.0f);
    // bullet time: meter refills, aborts, shared memory
    gp::BtGame g = read_game(false);
    int pm = read_playmode();
    if (g_cfg.bt.enabled) {
        if (g_ok_kills_ref) __try {
            DWORD cd = cache_data(); DWORD mc = cd ? RD32(cd + C_MISSION) : 0; DWORD md = mc ? RD32(mc + 0x18) : 0;
            if (md) {
                int k = (int)RD32(md + 0x28);   // EnemiesKilledOnCurrentMission (+1 per enemy death, 0x5D7186)
                if (g_last_kills >= 0 && k > g_last_kills && k - g_last_kills < 50) { g_meter.kills(g_cfg.meter, k - g_last_kills); g_kills_total += (DWORD)(k - g_last_kills); }
                g_last_kills = k;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_ok_combo_ref) __try {
            DWORD cd = cache_data(); DWORD cc = cd ? RD32(cd + C_COMBO) : 0; DWORD cdd = cc ? RD32(cc + 0x18) : 0;
            if (cdd) {
                int pts = (int)RD32(cdd + 0x3C);   // CurrentActionPoints
                if (g_last_points >= 0 && pts > g_last_points && pts - g_last_points < 100000) g_meter.points(g_cfg.meter, pts - g_last_points);
                g_last_points = pts;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        bool paused = pm == 2;
        if (g_bt.state == gp::BT_OFF && !paused && !g.cutscene) g_meter.idle(g_cfg.meter, dt);
        bool stale = g_bt.state != gp::BT_OFF && g_last_tick > 0.0 && now - g_last_tick > 0.4;
        gp::BtActions a = g_bt.abort_check(g, g.cutscene, stale, paused);
        if (a.send_force_normal || a.set_world_time) { perform(a, g.cutscene ? "abort (cut-scene)" : "abort (player not in control)"); g_bt_want = false; update_suppress(); }
    }
    // Suave: cheat key, input lock / automatic dive on the game's arrays (after the remap), stale-tick abort
    hang_where("POST_INPUT: phys_post_input (ragdoll keys, the vehicle physics step)");
    phys_post_input(p.ok ? kb : nullptr, p.ok ? kb + 0x100 : nullptr, ms, now);   // [physics] frame count, test-copy keys
    hang_where("POST_INPUT: psycho / loot / mprops / suave post-input");
    if (p.ok) psycho_post_input(kb, kb + 0x100, now);   // [psycho] debug start key / test-copy dodge
    if (p.ok) loot_post_input(kb, kb + 0x100, now);     // [drops] test-copy keys
    mprops_post_input(p.ok ? kb : nullptr, p.ok ? kb + 0x100 : nullptr, now);    // [mayhem] frame count, test-copy keys
    suave_post_input(p.ok ? p.state : nullptr, p.ok ? kb : nullptr, p.ok ? kb + 0x100 : nullptr, ms, now,
                     g_last_tick > 0.0 && now - g_last_tick < 0.4);
    publish(g, pm);
    if (p.ok) hang_post_input(kb);   // [debug] debug_stall_key (test copy only)
    hang_where("game (after POST_INPUT)");
}

// ================================================================================================ frame limiter
static gp::Limiter g_lim; static HANDLE g_timer = nullptr; static bool g_timer_tried = false;
static void limiter_wait() {
    if (g_cfg.fps_limit <= 0) return;
    if (!g_timer_tried) {
        g_timer_tried = true;
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!g_timer) { timeBeginPeriod(1); lg("limiter: no high-resolution timer, Sleep(1) + spin\n"); }
        else lg("limiter: high-resolution waitable timer, %d fps\n", g_cfg.fps_limit);
    }
    double now = now_s();
    double w = g_lim.wait_for(now, 1.0 / g_cfg.fps_limit);
    if (w <= 0.0) return;
    double until = now + w;
    if (w > 0.0015) {
        double coarse = w - 0.001;
        if (g_timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(coarse * 1e7);
            if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 100);
        } else {
            while (now_s() < until - 0.002) Sleep(1);
        }
    }
    while (now_s() < until) YieldProcessor();
}

// ================================================================================================ stubs
#define SAVE_XMM  __asm sub esp, 0x80 __asm movdqu [esp+0x00], xmm0 __asm movdqu [esp+0x10], xmm1 __asm movdqu [esp+0x20], xmm2 \
                  __asm movdqu [esp+0x30], xmm3 __asm movdqu [esp+0x40], xmm4 __asm movdqu [esp+0x50], xmm5 \
                  __asm movdqu [esp+0x60], xmm6 __asm movdqu [esp+0x70], xmm7
#define LOAD_XMM  __asm movdqu xmm0, [esp+0x00] __asm movdqu xmm1, [esp+0x10] __asm movdqu xmm2, [esp+0x20] \
                  __asm movdqu xmm3, [esp+0x30] __asm movdqu xmm4, [esp+0x40] __asm movdqu xmm5, [esp+0x50] \
                  __asm movdqu xmm6, [esp+0x60] __asm movdqu xmm7, [esp+0x70] __asm add esp, 0x80

static DWORD R_INACTIVE = P_INACTIVE, R_PADPOLL = P_PADPOLL, R_SENDMSG = P_SENDMSG;
static void __cdecl LimiterC() { limiter_wait(); }
static void __cdecl PostInputC() { post_input(); }
// call site 0x43C4E8 (ecx = platform object, return address 0x43C4ED on the stack): wait, then tail-jump to the original
__declspec(naked) void LoopTopStub() {
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm call LimiterC
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_INACTIVE]
}
// call site 0x43C516: the original pad poll first, then our per-pass work; returns to 0x43C51B
__declspec(naked) void PostInputStub() {
    __asm call dword ptr [R_PADPOLL]
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm call PostInputC
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm ret
}
// dive / wire arm sites: thiscall SendMessage(TIME_CTRL, -12, 0), callee pops 8; skipped while suppressed
__declspec(naked) void ArmDiveStub() {
    __asm cmp byte ptr [g_suppress_dive], 0
    __asm je pass
    __asm xor eax, eax
    __asm ret 8
pass:
    __asm jmp dword ptr [R_SENDMSG]
}
__declspec(naked) void ArmWireStub() {
    __asm cmp byte ptr [g_suppress_wire], 0
    __asm je pass
    __asm xor eax, eax
    __asm ret 8
pass:
    __asm jmp dword ptr [R_SENDMSG]
}
// clocks: replaces "call _ftol2" after fld/fmul; st(0) = ms value, result in eax (edx = sign), st(0) popped.
// ecx, ebx, esi, edi, ebp and xmm0-7 are preserved (more than _ftol2 promises).
static gp::MsCarry g_carry_game, g_carry_real;
static int __cdecl CarryC(gp::MsCarry* c, double v) { return c->credit(v); }
__declspec(naked) void FtolGameStub() {
    __asm push ecx
    __asm push edx
    SAVE_XMM
    __asm sub esp, 8
    __asm fstp qword ptr [esp]
    __asm push offset g_carry_game
    __asm call CarryC
    __asm add esp, 12
    LOAD_XMM
    __asm pop edx
    __asm pop ecx
    __asm cdq
    __asm ret
}
__declspec(naked) void FtolRealStub() {
    __asm push ecx
    __asm push edx
    SAVE_XMM
    __asm sub esp, 8
    __asm fstp qword ptr [esp]
    __asm push offset g_carry_real
    __asm call CarryC
    __asm add esp, 12
    LOAD_XMM
    __asm pop edx
    __asm pop ecx
    __asm cdq
    __asm ret
}
// pickups: at 0x79D0A3 (xmm0 = 0, esi = prop_move_ctrl data: t +44h, TimeStep +A8h); t = clamp(t - step, 0, step)
static const float g_zero = 0.0f;
static DWORD R_PICKUP = 0x79D0A8;
__declspec(naked) void PickupStub() {
    __asm movss xmm0, dword ptr [esi + 0x44]
    __asm subss xmm0, dword ptr [esi + 0xA8]
    __asm maxss xmm0, dword ptr [g_zero]
    __asm minss xmm0, dword ptr [esi + 0xA8]
    __asm movss dword ptr [esi + 0x44], xmm0
    __asm jmp dword ptr [R_PICKUP]
}
// mouse deltas: xmm0 = value about to be stored
static DWORD R_MDX = 0x73ED9E, R_MDY = 0x73EDF5;
__declspec(naked) void MouseDXStub() {
    __asm mulss xmm0, dword ptr [g_mx_factor]
    __asm movss dword ptr [esi + 0x44], xmm0
    __asm jmp dword ptr [R_MDX]
}
__declspec(naked) void MouseDYStub() {
    __asm mulss xmm0, dword ptr [g_my_factor]
    __asm movss dword ptr [esi + 0x48], xmm0
    __asm jmp dword ptr [R_MDY]
}
// frame-rate independent mouse yaw (design: tools/movement/proxy/movefeel.cpp; drop window follows the time scale)
static gp::YawAcc g_yaw;
static volatile DWORD g_yaw_phys = 0;
static bool g_yaw_patched = false;
bool gp_yaw_add(float deg) {   // Suave's turn before the automatic dive (main thread, like the integrate)
    if (!g_yaw_patched || !g_yaw.mouse_mode || !g_yaw_phys) return false;
    g_yaw.acc = deg;   // set, not added: a repeated request must not stack (fifth test)
    return true;
}
static float g_tmp = 0.0f;
static DWORD player_phys_data() {
    __try {
        DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = RD32(h + 0x18); DWORD r = RD32(d + 0x84); if (!r) return 0;
        DWORD e = RD32(RD32(r + 0x18) + 0x68); return RD32(e + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static float cur_time_mult() { __try { DWORD s = RD32(A_SCENE); return s ? RDF(s + 0x114) : 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 1.0f; } }
static void __cdecl YawWriteC(DWORD phys, float turn) { g_yaw_phys = phys; g_yaw.on_write(turn, GetTickCount(), gp::yaw_drop_ms(cur_time_mult())); }
static void __cdecl YawPadC() { g_yaw.on_pad(); }
static float __cdecl YawReadC(DWORD phys, float orig) {
    if (!g_yaw.mouse_mode || phys != g_yaw_phys) return orig;
    float ts = 0.04f;
    __try { ts = RDF(phys + 0x84); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (ts <= 0.0f) return orig;
    return g_yaw.on_consume(ts, GetTickCount());
}
static DWORD R_YAWW = 0x73FFDA, R_YAWP = 0x73E3FE, R_YAWR = 0x57D509;
__declspec(naked) void YawWriteStub() {   // at 0x73FFD5: movss [eax+24h],xmm0 (eax = PhysicCtrl data)
    __asm movss dword ptr [eax + 0x24], xmm0
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm sub esp, 4
    __asm movss dword ptr [esp], xmm0
    __asm push eax
    __asm call YawWriteC
    __asm add esp, 8
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_YAWW]
}
__declspec(naked) void YawPadStub() {     // at 0x73E3F9: movss [edx+24h],xmm0
    __asm movss dword ptr [edx + 0x24], xmm0
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm call YawPadC
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_YAWP]
}
__declspec(naked) void YawReadStub() {    // at 0x57D504: movss xmm0,[esi+24h] (esi = PhysicCtrl data)
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm sub esp, 4
    __asm movss xmm0, dword ptr [esi + 0x24]
    __asm movss dword ptr [esp], xmm0
    __asm push esi
    __asm call YawReadC
    __asm add esp, 8
    __asm fstp dword ptr [g_tmp]
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm movss xmm0, dword ptr [g_tmp]
    __asm jmp dword ptr [R_YAWR]
}
// diagonal clamp: at 0x55FCEF (jmp 0x560711) after the player stick copy into [esi]
static void __cdecl DiagC(float* v) { __try { gp::diag_clamp(v); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static DWORD R_DIAG = 0x560711;
__declspec(naked) void DiagStub() {
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm push esi
    __asm call DiagC
    __asm add esp, 4
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_DIAG]
}
// vehicles: gradual steering return. At 0x4F60C5 (no steering key; esi = car_move_ctrl data, wheel_angle +378h):
// wheel_angle = max(v - r, 0) + min(v + r, 0), i.e. toward 0 by r per 0.04 s step. The code after it reads xmm0 = 0.
float g_steer_rate = 0.25f;
static DWORD R_STEER = 0x4F60D0;
__declspec(naked) void SteerReturnStub() {
    __asm movss xmm0, dword ptr [esi + 0x378]
    __asm subss xmm0, dword ptr [g_steer_rate]
    __asm maxss xmm0, dword ptr [g_zero]
    __asm movss xmm1, dword ptr [esi + 0x378]
    __asm addss xmm1, dword ptr [g_steer_rate]
    __asm minss xmm1, dword ptr [g_zero]
    __asm addss xmm0, xmm1
    __asm movss dword ptr [esi + 0x378], xmm0
    __asm xorps xmm0, xmm0
    __asm jmp dword ptr [R_STEER]
}
// camera fov (design: tools/camera/proxy/camfov.cpp, with the culling-operand fix described in HOOKS.md)
float g_fov_eff = 64.0f, g_aspect = 4.0f / 3.0f;
static float g_fov_in = -1.0f, g_fov_out = 64.0f;
static void __cdecl CameraC(BYTE* cam) {
    __try {
        float fov = *(float*)(cam + 0xB4);
        float eff = fov;
        if (g_cfg.fov > 0.0f) {
            bool apply = true;
            if (!g_cfg.fov_all) { DWORD cd = cache_data(); DWORD gc = cd ? RD32(cd + 0x260) : 0; apply = ((DWORD)(uintptr_t)cam == gc); }
            if (apply) { if (fov != g_fov_in) { g_fov_in = fov; g_fov_out = gp::fov_remap(fov, g_cfg.fov); } eff = g_fov_out; }
        }
        g_fov_eff = eff;
        if (g_cfg.cull_fix) {
            float a = 4.0f / 3.0f;
            DWORD r = RD32(0xA39F14); float rx = RDF(0xA119F4);
            if (r) { int w = *(int*)(uintptr_t)(r + 0x320), h = *(int*)(uintptr_t)(r + 0x324); if (w > 0 && h > 0 && rx > 0.01f) a = (float)w / (float)h / rx; }
            if (a != g_aspect) { static int n = 0; if (++n <= 8) lg("cull_fix: culling aspect %.4f (vanilla 1.3333)\n", a); }
            g_aspect = a;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_fov_eff = 64.0f; }
}
static DWORD R_CAM = 0x87DE46;
__declspec(naked) void CameraStub() {
    __asm pushad
    __asm pushfd
    SAVE_XMM
    __asm push ecx
    __asm call CameraC
    __asm add esp, 4
    LOAD_XMM
    __asm popfd
    __asm popad
    __asm sub esp, 0xF0
    __asm jmp dword ptr [R_CAM]
}

// ================================================================================================ install
static void install() {
    const Cfg& c = g_cfg;
    bool refs_core = ref_ok("F_SENDMSG") && ref_ok("F_TOUCH") && ref_ok("G_CACHE_WT") && ref_ok("G_CACHE_DES") &&
                     ref_ok("G_CACHE_TTU") && ref_ok("G_CACHE_EXC") && ref_ok("G_WT_MEMBER") && ref_ok("G_TIMECTRL") &&
                     ref_ok("G_SCENE_TM") && ref_ok("H_SLOWMO");
    bool refs_input = ref_ok("G_KBOBJ") && ref_ok("G_MSOBJ") && ref_ok("G_KBEDGE") && ref_ok("G_KBSTATE") && ref_ok("F_PADPOLL");
    g_ok_cutscene_ref = ref_ok("G_CUTSCENE");
    g_ok_kills_ref = ref_ok("G_KILLS_R") && ref_ok("G_KILLS_W") && ref_ok("G_MISSION");
    g_ok_combo_ref = ref_ok("G_COMBO") && ref_ok("G_COMBO_PTS");
    bool bt = c.bt.enabled, restart = c.restart;
    if ((bt || restart) && !(refs_core && refs_input)) { lg("bullet time / restart: reference bytes differ, OFF\n"); bt = restart = false; }
    if (restart && !(ref_ok("G_MISSION") && ref_ok("G_MISSION_CM") && ref_ok("H_ENDMISSION"))) { lg("restart: mission references differ, OFF\n"); restart = false; }
    g_cfg.bt.enabled = bt; g_cfg.restart = restart;
    bool remap = c.remap.any && refs_input;
    bool ynorm = c.y_fps_norm;
    bool sv = suave_enabled() && refs_core && refs_input;
    if (suave_enabled() && !sv) lg("suave: core / input reference bytes differ, OFF\n");
    bool sm = smart_enabled() && refs_core && refs_input;   // Smart AI: its tick runs in the input tick
    bool smt = sm && smart_wants_tick();
    if (smart_enabled() && !sm) lg("smart: core / input reference bytes differ, OFF\n");
    bool slt = seam_enabled() && seam_wants_tick() && refs_input;   // seamless: the tick only feeds the prefetch
    bool dbi = gp_debug_wants_input() && refs_input;                // [debug] key script / stress run
    bool phy = phys_enabled() && refs_input;                         // [physics]: the input tick steps the props
    bool dft = difficulty_wants_tick() && refs_input;               // [difficulty] tuning / reinforcements
    bool psy = psycho_enabled() && refs_core && refs_input;         // [psycho] enemy loco moves: input tick + post-input
    bool lot = loot_enabled() && refs_core && refs_input;           // [drops]: input tick + post-input
    bool gwt = gw_wants_tick() && refs_input;                       // [gang_wars]: everything runs in the input tick
    bool hdw = hud_enabled() && refs_input;                         // [minimap]: radar blips every pass
    bool htt = heat_wants_tick() && refs_input;                     // [mayhem_heat]: events, cooling, escalation
    if (heat_enabled() && !htt) lg("heat: input reference bytes differ: [mayhem_heat] OFF\n");
    bool bft = bf_wants_tick() && refs_input;                       // [bigger_fights]: extra enemies, heat vehicles
    // per-pass hook and input tick
    bool post_ok = false, tick_ok = false;
    bool pdw = pad_wants_post() && refs_input;                     // [pad] per-frame work
    if (bt || restart || remap || ynorm || sv || smt || slt || dbi || pdw || phy || dft || psy || gwt || hdw || htt || bft || lot) post_ok = patch_site("POST_INPUT", (void*)&PostInputStub);
    if ((bt || restart || sv || smt || slt || dbi || phy || dft || psy || gwt || htt || bft || lot) && post_ok) {
        bool k = site_ok("KB_PROC") && site_ok("PAD_PROC");
        if (k) { patch_site("KB_PROC", (void*)&KbProcWrap); patch_site("PAD_PROC", (void*)&PadProcWrap); tick_ok = true; }
        else lg("input tick sites differ: bullet time / restart OFF\n");
    }
    if (!tick_ok) { g_cfg.bt.enabled = false; g_cfg.restart = false; }
    if (!post_ok) g_cfg.remap.any = false;
    // [debug] hang_watchdog (dll/src/gp_hang.cpp): its heartbeat is POST_INPUT; the real GOG exe only (not the test host)
    if (hang_enabled() && g_patch_allowed) {
        if (post_ok) hang_start();
        else lg("hang watchdog: no per-frame hook (no feature needs POST_INPUT): off\n");
    }
    if (sv && suave_install(tick_ok)) g_status |= TODGP_F_SUAVE_ENABLED;
    if (sm) smart_install(tick_ok);
    if (gw_enabled()) gw_install(tick_ok && gwt);   // [gang_wars] (gang_wars/code/gw_game.cpp)
    if (seam_enabled()) seam_install(tick_ok);   // seamless border swaps (seamless/code/seam_game.cpp)
    if (phy) phys_install(tick_ok);              // [physics] Box3D props (physics/code/phys_game.cpp)
    if (phy) mprops_install(tick_ok);            // [mayhem] chain reactions / breakable cover on top of it (mayhem/code/mh_game.cpp)
    gp_fix_install();                            // engine fixes ([fixes], dll/src/gp_fix.cpp): the border-swap double free
    display_install();                           // [display]: 16:9 table, wide HUD, pillarboxed menus (dll/src/gp_display.cpp)
    if (pad_enabled()) pad_install(post_ok && refs_input);   // [pad]: controller support (dll/src/gp_pad.cpp)
    if (hdw || !hud_enabled()) hud_install();     // [minimap]: overlay clients (dll/src/gp_hud.cpp); before heat: the map ink is drawn under its rim pulse
    else lg("minimap: input references differ: [minimap] OFF\n");
    if (htt) heat_install(tick_ok);              // [mayhem_heat]: heat events, radar tint / icons (dll/src/gp_heat.cpp)
    difficulty_install(tick_ok);                 // [difficulty]: menu levels, runtime tuning, BRUTAL reinforcements (+ heat waves)
    bf_install(tick_ok && bft);                  // [bigger_fights]: separate saves, extra enemies, heat vehicles (dll/src/gp_bigfights.cpp)
    if (psy) psycho_install(tick_ok);            // [psycho]: PSYCHO enemy loco moves (psycho/code/psycho_game.cpp)
    else if (psycho_enabled()) lg("psycho: core / input reference bytes differ, OFF\n");
    if (lot) loot_install(tick_ok);              // [drops]: pickups from dead enemies (drops/code/drops_game.cpp)
    else if (loot_enabled()) lg("drops: core / input reference bytes differ, OFF\n");
    if (phy) ragdoll_install();                  // [ragdoll]: after every REF check of the others (ragdoll/code/rd_game.cpp)
    mayhem_install();                            // [mayhem_title]: MAYHEM in flames on the title screen (dll/src/gp_mayhem.cpp)
    gp_overlay_install();                        // the shared overlay hooks, once every client registered (dll/src/gp_overlay.cpp)
    // automatic slow motion: the arm sites are needed for "off" and for bullet time (dives keep the bullet-time speed)
    bool arm_dive = c.dive_slowmo_off || (g_cfg.bt.enabled && !c.dive_in_bt);
    bool arm_wire = c.wire_slowmo_off || (g_cfg.bt.enabled && !c.dive_in_bt);
    if (arm_dive) {
        if (site_ok("ARM_DIVE1") && site_ok("ARM_DIVE2") && site_ok("ARM_DIVE3")) {
            patch_site("ARM_DIVE1", (void*)&ArmDiveStub); patch_site("ARM_DIVE2", (void*)&ArmDiveStub); patch_site("ARM_DIVE3", (void*)&ArmDiveStub);
        } else lg("dive arm sites differ: dive slow-motion option OFF\n");
    }
    if (arm_wire) patch_site("ARM_WIRE", (void*)&ArmWireStub);
    update_suppress();
    // clocks, pickups, limiter
    if (c.clock_fix) {
        if (site_ok("CLK_GAME") && site_ok("CLK_REWIND") && site_ok("CLK_REAL")) {
            patch_site("CLK_GAME", (void*)&FtolGameStub); patch_site("CLK_REWIND", (void*)&FtolGameStub); patch_site("CLK_REAL", (void*)&FtolRealStub);
        } else lg("clock sites differ: clock_fix OFF\n");
    }
    if (c.pickup_fix) patch_site("PICKUP", (void*)&PickupStub);
    if (c.fps_limit > 0) patch_site("LOOP_TOP", (void*)&LoopTopStub);
    // mouse
    if (c.sens_x != 1.0f) { g_mx_factor = c.sens_x; patch_site("MOUSE_DX", (void*)&MouseDXStub); }
    if (c.sens_y != 1.0f || c.invert_y || c.y_fps_norm) {
        g_my_factor = gp::mouse_y_factor(c.invert_y, c.sens_y, false, 1.0f / 60.0f);
        patch_site("MOUSE_DY", (void*)&MouseDYStub);
    }
    if (c.mouse_turn) {
        g_yaw.seconds_per_frame = c.mouse_spf > 0.0f ? c.mouse_spf : 0.95f / 60.0f;
        if (site_ok("YAW_WRITE") && site_ok("YAW_PAD") && site_ok("YAW_READ")) {
            patch_site("YAW_WRITE", (void*)&YawWriteStub); patch_site("YAW_PAD", (void*)&YawPadStub); patch_site("YAW_READ", (void*)&YawReadStub);
            g_yaw_patched = true;
        } else lg("mouse turn sites differ: mouse_turn_fix OFF\n");
    }
    if (c.diagonal) patch_site("DIAG", (void*)&DiagStub);
    // vehicles (experimental: they apply to every car that takes these branches, AI cars included)
    if (c.steer_return > 0.0f) { g_steer_rate = c.steer_return; patch_site("STEER_RET", (void*)&SteerReturnStub); }
    if (c.brake_parity) {
        if (site_ok("SBRAKE_A") && site_ok("SBRAKE_B")) {
            static const BYTE a[6] = {0xE9, 0x4B, 0x01, 0x00, 0x00, 0x90}, b[2] = {0xEB, 0x1E};
            patch_oper("SBRAKE_A", a, 6); patch_oper("SBRAKE_B", b, 2);
        } else lg("S-brake sites differ: s_brake_parity OFF\n");
    }
    // camera
    if (c.cull_fix) {   // the renderer bridge (and the camfov research proxy) patch the same culling operand
        HMODULE di = GetModuleHandleA("dinput8.dll"); char p[MAX_PATH] = "";
        if (di && GetModuleFileNameA(di, p, MAX_PATH)) {
            char* s = strrchr(p, '\\'); if (s) *s = 0;
            if (!_stricmp(p, g_dir)) { lg("cull_fix: a dinput8.dll proxy from the game folder is loaded (renderer bridge?): left to it\n"); g_cfg.cull_fix = false; }
        }
    }
    if (c.fov > 0.0f || c.cull_fix) {
        bool ok = site_ok("CAM_ENTRY") && (c.fov <= 0.0f || (site_ok("CAM_VIEW") && site_ok("CAM_CULL"))) && (!c.cull_fix || site_ok("CAM_ASPECT"));
        if (!ok) lg("camera sites differ (the renderer bridge's cull fix may own CAM_ASPECT): camera options OFF\n");
        else {
            if (c.fov > 0.0f) {
                BYTE v[6] = {0xA1, 0, 0, 0, 0, 0x90}; *(DWORD*)(v + 1) = (DWORD)(uintptr_t)&g_fov_eff;
                patch_oper("CAM_VIEW", v, 6);
                BYTE m[5] = {0x05, 0, 0, 0, 0}; *(DWORD*)(m + 1) = (DWORD)(uintptr_t)&g_fov_eff;   // modrm 05 = [disp32]
                patch_oper("CAM_CULL", m, 5);
            }
            if (c.cull_fix) { DWORD a = (DWORD)(uintptr_t)&g_aspect; patch_oper("CAM_ASPECT", (BYTE*)&a, 4); }
            patch_site("CAM_ENTRY", (void*)&CameraStub);
        }
    }
}

// ================================================================================================ shared helpers (gp_glue.h)
void gp_log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    lg("%s", buf);
}
bool gp_send(DWORD node, int id, void* args) { return send_msg(node, id, args); }
bool gp_ref_ok(const char* id) { return ref_ok(id); }
bool gp_site_ok(const char* id) { return site_ok(id); }
bool gp_patch_call(const char* id, void* to) { return patch_site(id, to); }
bool gp_patch_bytes(const char* id, const void* bytes, int n) { return patch_oper(id, (const BYTE*)bytes, n); }
DWORD gp_cache_data() { __try { return cache_data(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
const gp::Ini& gp_ini() { return g_ini; }
int gp_key_cfg(const char* s, const char* k, const char* def) { return key_cfg(s, k, def); }
double gp_now() { return now_s(); }
bool gp_bt_running() { return g_cfg.bt.enabled && g_bt.state != gp::BT_OFF; }
void gp_bt_force_off(const char* why) {
    if (!gp_bt_running()) return;
    gp::BtActions off; off.set_world_time = g_bt.state == gp::BT_ACTIVE; off.world_time = 1.0f; off.send_force_normal = true;
    perform(off, why); g_bt.state = gp::BT_OFF; g_bt_want = false; update_suppress();
}
bool gp_cutscene() { gp::BtGame g = read_game(false); return g.cutscene; }
int gp_playmode() { return read_playmode(); }

// ================================================================================================ identity
static bool exe_identity(char md5out[33], DWORD* size) {
    char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    md5out[0] = 0; *size = 0;
    if (h == INVALID_HANDLE_VALUE) return false;
    *size = GetFileSize(h, nullptr);
    gp::Md5 m; m.init();
    static BYTE buf[65536]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) m.update(buf, got);
    CloseHandle(h);
    BYTE d[16]; m.final(d); gp::md5_hex(d, md5out);
    return true;
}

// ================================================================================================ init / exports
static const char* GOG_MD5 = "d9beb351b3c78ee9f14a766a368f3ff9";
static const DWORD GOG_SIZE = 6860800;

static void read_ini_file() {
    char p[MAX_PATH + 32]; sprintf_s(p, "%s\\TODGameplay.ini", g_dir);
    FILE* f = _fsopen(p, "rb", _SH_DENYNO);
    static char text[262144]; size_t n = 0;   // the full sample ini is ~45 KB
    bool more = false;
    if (f) { n = fread(text, 1, sizeof text - 1, f); more = fgetc(f) != EOF; fclose(f); }
    text[n] = 0;
    g_ini.parse(text);
    if (!f) lg("no TODGameplay.ini next to the exe: every feature off, nothing patched\n");
    else lg("ini: %u bytes, %d entries read (room for %d)%s\n", (unsigned)n, g_ini.n, gp::Ini::CAP,
            more ? " - FILE TRUNCATED: the rest of the ini was not read" : (g_ini.n >= gp::Ini::CAP ? " - FULL: later entries were dropped" : ""));
}
static void open_shared() {
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, TODGP_SHARED_NAME);
    if (!g_map) return;
    g_sh = (TodgpShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 4096);
    if (!g_sh) return;
    memset(g_sh, 0, sizeof *g_sh);
    g_sh->magic = TODGP_SHARED_MAGIC; g_sh->version = TODGP_SHARED_VERSION; g_sh->size = (uint16_t)sizeof(TodgpShared);
}
static bool any_feature() {
    const Cfg& c = g_cfg;
    return c.bt.enabled || c.dive_slowmo_off || c.wire_slowmo_off || c.clock_fix || c.pickup_fix || c.fps_limit > 0 ||
           c.mouse_turn || c.sens_x != 1.0f || c.sens_y != 1.0f || c.invert_y || c.y_fps_norm || c.diagonal ||
           c.fov > 0.0f || c.cull_fix || c.restart || c.remap.any || c.steer_return > 0.0f || c.brake_parity ||
           suave_enabled() || smart_enabled() || seam_enabled() || gp_debug_on() || display_enabled() || pad_enabled() ||
           phys_enabled() || difficulty_enabled() || mayhem_enabled() || psycho_enabled() || loot_enabled() || gw_enabled() || hud_enabled() || heat_enabled() || bf_enabled();
}
static void init_common(bool allow_patch, const char* how) {
    load_config();
    if (g_cfg.start_full) g_meter.value = 1.0f; else g_meter.value = 0.0f;
    if (g_cfg.bt.enabled) g_status |= TODGP_F_BT_ENABLED;
    if (g_cfg.dive_slowmo_off) g_status |= TODGP_F_DIVE_SLOWMO_OFF;
    if (allow_patch) gp_debug_load_config();   // [debug] (off unless the ini asks; test copy only)
    hang_load_config();                         // [debug] hang_watchdog (started by install() on the GOG exe only)
    if (!allow_patch) { g_status |= TODGP_F_REFUSED; lg("exe identity does not match (%s): NOTHING patched\n", how); }
    else if (!any_feature()) lg("every feature is off: nothing patched (vanilla)\n");
    else { install(); g_status |= TODGP_F_HOOKS_OK; lg("%d site(s) patched\n", g_patched); }
    if (g_sh) { g_sh->flags = g_status; g_sh->meter = g_meter.value; }
}
extern "C" __declspec(dllexport) DWORD __cdecl TODGameplay_Init() {
    if (InterlockedExchange(&g_inited, 1)) return g_status;
    QueryPerformanceFrequency(&g_qpf);
    GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
    char* sl = strrchr(g_dir, '\\'); if (sl) *sl = 0;
    read_ini_file();
    g_log = g_ini.geti("general", "log", 1);
    char md5[33]; DWORD size = 0;
    exe_identity(md5, &size);
    lg("TODGameplay build %d loading in %s (exe %u bytes, md5 %s)\n", GP_BUILD, g_dir, size, md5);
    open_shared();
    g_patch_allowed = size == GOG_SIZE && !strcmp(md5, GOG_MD5) && mem_equal(0x400000, "4d5a");
    init_common(g_patch_allowed, "not the GOG TOD.exe");
    return g_status;
}
extern "C" __declspec(dllexport) DWORD __cdecl TODGameplay_Status() { return g_status; }

// ---- test host only: install onto a simulated read-only copy of TOD.exe's sections mapped by tests/dll/host.exe.
// Never called by the game, the loader or the bridge. Refuses inside a process whose exe is TOD.exe.
extern "C" __declspec(dllexport) DWORD __cdecl TODGameplay_TestInstall(const char* token, const char* ini_text) {
    if (!token || strcmp(token, "simulated-image-not-the-game")) return 0xFFFFFFFF;
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    if (!_stricmp(base, "TOD.exe") || g_patch_allowed) return 0xFFFFFFFE;
    g_ini.parse(ini_text ? ini_text : "");
    g_status = 0; g_patched = 0;
    init_common(true, "test host");
    return (DWORD)g_patched;
}
extern "C" __declspec(dllexport) void* __cdecl TODGameplay_TestStub(const char* name) {   // stubs the host may execute
    if (!strcmp(name, "ftol_game")) return (void*)&FtolGameStub;
    if (!strcmp(name, "ftol_real")) return (void*)&FtolRealStub;
    if (!strcmp(name, "arm_dive")) return (void*)&ArmDiveStub;
    if (!strcmp(name, "suppress_dive")) return (void*)&g_suppress_dive;
    if (void* p = suave_test_stub(name)) return p;
    if (void* p = seam_test_stub(name)) return p;
    if (void* p = gp_fix_test_stub(name)) return p;
    if (void* p = display_test_stub(name)) return p;
    if (void* p = pad_test_stub(name)) return p;
    if (void* p = phys_test_stub(name)) return p;
    if (void* p = mprops_test_stub(name)) return p;
    if (void* p = ragdoll_test_stub(name)) return p;
    if (void* p = gp_menu_test_stub(name)) return p;
    if (void* p = difficulty_test_stub(name)) return p;
    if (void* p = mayhem_test_stub(name)) return p;
    if (void* p = psycho_test_stub(name)) return p;
    if (void* p = loot_test_stub(name)) return p;
    if (void* p = gw_test_stub(name)) return p;
    if (void* p = gp_overlay_test_stub(name)) return p;
    if (void* p = hud_test_stub(name)) return p;
    if (void* p = heat_test_stub(name)) return p;
    if (void* p = bf_test_stub(name)) return p;
    return smart_test_stub(name);
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) TODGameplay_Init();
    return TRUE;
}
