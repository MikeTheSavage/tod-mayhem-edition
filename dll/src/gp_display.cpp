// gp_display.cpp - [display] of TODGameplay.dll: Total Overdose at 16:9 (any wide aspect).
// OFF by default; with enabled = 0 nothing here is patched. Every site is byte-checked (sites.h, rows DS_*); the math
// is in gp_display_logic.cpp (unit tested). Background: the author's reverse-engineering notes on the HUD and menus and
// the hudwide research proxy (not included; wide_hud / hud_scale / border / res_table were ported from it).
//
//   res_table     the five sizes behind the SCREEN RESOLUTION option (0x4AB940) become 16:9 ones; "auto" =
//                 1280x720, 1600x900, 1920x1080, 2560x1440 and index 4 = the size in configpc.txt (screensize).
//   startup_size  config: the saved option (Slot11 Setting_ScreenSize) is no longer applied after the first frame
//                 (0x769ABD), so configpc.txt's screensize stays; saved: vanilla (the table entry of the saved index).
//   wide_hud      the in-game HUD canvas gets the screen's aspect (480 high, 480 W/H wide): widgets on the real edges,
//                 aim dot in the middle (the is_wide_screen "wide" branch with computed values; hudwide design).
//   hud_scale / border   HUD size (canvas 480 / hud_scale high) and the corner margin (BORDER_PERCENTAGE, vanilla 0.05).
//   menus         menus, subtitles, load screens, the front end: the 800x600 screen-relative canvas is fitted into the
//                 screen with square pixels (s = min(W/800, H/600), centred) instead of being stretched: the Sprite
//                 matrix (two render paths), the TextBox font scale + glyph offset in Font::DrawString (both
//                 versions, rotated and axis-aligned paths), the mouse hit test and the load bar quad.
//                 Full-width bands (menu frame bars, subtitle band, shutters) keep the full width (bands = stretch |
//                 fit); full-screen sprites (fades, menu backdrop, load-screen art) cover the screen with square pixels,
//                 cropping the top / bottom overhang (backgrounds = cover (default) | stretch | fit).
//   cull_fix      turns on [camera] cull_fix (culling frustum with the real aspect: no popping at the 16:9 edges).
//   mode_menu     (default 1) SETTINGS > SCREEN RESOLUTION lists the monitor's real modes (windowed / borderless /
//                 fullscreen) and applies them live; the mod keeps the choice (gp_display_modes.cpp). With it on, the
//                 resolution table, borderless and the startup size are the mode module's; dpi_aware is always on.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gp_glue.h"
#include "gp_display_logic.h"
#include "gp_display_modes.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define SAVE_XMM  __asm sub esp, 0x80 __asm movdqu [esp+0x00], xmm0 __asm movdqu [esp+0x10], xmm1 __asm movdqu [esp+0x20], xmm2 \
                  __asm movdqu [esp+0x30], xmm3 __asm movdqu [esp+0x40], xmm4 __asm movdqu [esp+0x50], xmm5 \
                  __asm movdqu [esp+0x60], xmm6 __asm movdqu [esp+0x70], xmm7
#define LOAD_XMM  __asm movdqu xmm0, [esp+0x00] __asm movdqu xmm1, [esp+0x10] __asm movdqu xmm2, [esp+0x20] \
                  __asm movdqu xmm3, [esp+0x30] __asm movdqu xmm4, [esp+0x40] __asm movdqu xmm5, [esp+0x50] \
                  __asm movdqu xmm6, [esp+0x60] __asm movdqu xmm7, [esp+0x70] __asm add esp, 0x80

static DWORD A_RENDERER = 0xA39F14;   // renderer pointer; +328h / +32Ch back buffer W / H (REF DS_SCREEN = 0x41FD70)

// ---- original targets / return addresses of the stubs (check_sites.py compares them with the sites and REF rows)
static DWORD R_DS_MATRIX = 0x4671A0;    // DS_SPR_MTX / DS_SPR2_MTX original call target (scale matrix, thiscall ret 0Ch)
static DWORD R_DS_FONTSCALE = 0x8F4950; // DS_TXT_SCALE_A / _B original call target (cdecl out[2] = W/800, H/600)
static DWORD R_DS_TA_ROT = 0x41B5D2, R_DS_TA_ROT_J = 0x41B669;   // DrawString 0x41B200 rotated path: after the je / its target
static DWORD R_DS_TA_AX = 0x41B80B, R_DS_TA_AX_J = 0x41B85C;     // ... axis-aligned path
static DWORD R_DS_TB_ROT = 0x41BD8C, R_DS_TB_ROT_J = 0x41BE23;   // DrawString 0x41B9C0 rotated path
static DWORD R_DS_TB_AX = 0x41BFC5, R_DS_TB_AX_J = 0x41C016;     // ... axis-aligned path
static DWORD R_DS_MOUSE = 0x8F4AE0;     // DS_MOUSE: back after the two replaced stores
static DWORD R_DS_QUAD = 0x4322A0;      // DS_LOADBAR original call target (textured quad, thiscall ret 28h)
static DWORD R_DS_RESTAB = 0x4AB940;    // DS_RES_START original call target (the table, cdecl)
static DWORD R_DS_ISWIDE = 0x485640;    // DS_WH_DIM / DS_WH_DOT original call target (is_wide_screen, thiscall ret 4)
static DWORD R_DS_MODEFIND = 0x450890;  // DS_FS_LOOKUP / DS_SR_LOOKUP original call target (mode list lookup, thiscall ret 0Ch)
static DWORD R_DS_FSINIT = 0x45BE30;    // DS_BL_INIT original call target (fullscreen device init, thiscall (w, h) ret 8)
static DWORD R_DS_WININIT = 0x45BEF0;   // windowed device init (same signature): borderless runs the game through it
static DWORD R_DS_WINSIZE = 0x43B9F0;   // DS_BL_WIN1 / DS_BL_WIN2 original call target (windowed window sizing, thiscall ret 4)
static const DWORD A_WH_W853 = 0x72B05C;   // mov dword ptr [esp+0Ch], 355h: imm at +4 is rewritten by the hook

// ---- config
struct DispCfg {
    bool enabled = false, log = true;
    bool res = false; gpd::Res table[5] = {};
    bool startup_config = false;
    bool wide = false; float hud_scale = 1.0f; bool border_set = false; float border = 0.05f;
    bool menus = false; int bands = gpd::MODE_STRETCH, full = gpd::MODE_COVER;
    bool cull = false;
    bool fs_modes = false, borderless = false, dpi_aware = false;
};
static DispCfg g_dc;
static int g_cfg_w = 0, g_cfg_h = 0;
static char g_cfg_text[16384];

static bool g_ds_mouse_off = false;   // [debug] menu_mouse_off (test copy only): no menu hover from the OS cursor
void display_load_config() {
    const gp::Ini& ini = gp_ini();
    g_ds_mouse_off = ini.geti("debug", "menu_mouse_off", 0) != 0;
    DispCfg c;
    c.enabled = ini.geti("display", "enabled", 0) != 0;
    if (c.enabled) {
        c.log = ini.geti("display", "log", 1) != 0;
        // configpc.txt next to the exe: the configured size (index 4 of the auto table)
        char p[MAX_PATH + 32]; GetModuleFileNameA(nullptr, p, MAX_PATH);
        char* sl = strrchr(p, '\\'); if (sl) strcpy_s(sl + 1, MAX_PATH - (sl + 1 - p), "configpc.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") == 0 && f) {
            size_t n = fread(g_cfg_text, 1, sizeof g_cfg_text - 1, f); fclose(f); g_cfg_text[n] = 0;
            gpd::parse_screensize(g_cfg_text, &g_cfg_w, &g_cfg_h);
        }
        const char* rt = ini.get("display", "res_table");
        if (!rt) rt = "auto";
        if (_stricmp(rt, "off") && _stricmp(rt, "0") && _stricmp(rt, "vanilla")) {
            c.res = gpd::res_table(rt, g_cfg_w, g_cfg_h, c.table);
            if (!c.res) gp_log("display: res_table = %s: not 'auto' / 'off' / five WxH sizes: table vanilla\n", rt);
        }
        const char* su = ini.get("display", "startup_size");
        c.startup_config = !su || !_stricmp(su, "config");
        c.wide = ini.geti("display", "wide_hud", 1) != 0;
        c.hud_scale = ini.getf("display", "hud_scale", 1.0f);
        const char* b = ini.get("display", "border");
        if (b && _stricmp(b, "vanilla") && _stricmp(b, "0")) {
            float v = (float)atof(b);
            if (v >= 0.0f && v <= 0.25f) { c.border_set = true; c.border = v; }
            else gp_log("display: border %s outside 0..0.25: vanilla\n", b);
        }
        c.menus = ini.geti("display", "menus", 1) != 0;
        c.bands = gpd::mode_from_name(ini.get("display", "bands"), gpd::MODE_STRETCH);
        c.full = gpd::mode_from_name(ini.get("display", "backgrounds"), gpd::MODE_COVER);
        c.cull = ini.geti("display", "cull_fix", 1) != 0;
        c.fs_modes = ini.geti("display", "fullscreen_any_mode", 1) != 0;
        c.borderless = ini.geti("display", "borderless", 0) != 0;
        c.dpi_aware = ini.geti("display", "dpi_aware", c.borderless ? 1 : 0) != 0;   // default: on with borderless only
        // the in-game mode menu (gp_display_modes.cpp); its first start without a side file uses configpc.txt and borderless
        DmCfg dm; dm.cfg_w = g_cfg_w; dm.cfg_h = g_cfg_h; dm.borderless_ini = c.borderless;
        bool fs = false; if (g_cfg_text[0] && gpd::parse_fullscreen(g_cfg_text, &fs)) dm.cfg_fs = fs;
        dm_load_config(ini, dm);
        if (dm_enabled()) {
            // c.res stays: the table's three call sites are the mode module's, so its sizes no longer matter (harmless)
            c.startup_config = true;  // the saved index is never applied after the first frame
            c.borderless = false;     // borderless is one of the row's modes
            c.fs_modes = true;        // exclusive fullscreen at any size the adapter offers
            c.dpi_aware = ini.geti("display", "dpi_aware", 1) != 0;   // physical pixels (the row lists real sizes)
        }
    }
    g_dc = c;
}
bool display_enabled() { return g_dc.enabled && (g_dc.res || g_dc.startup_config || g_dc.wide || g_dc.border_set || g_dc.menus || g_dc.cull || g_dc.fs_modes || g_dc.borderless || g_dc.dpi_aware || dm_enabled()); }
bool display_wants_cull() { return g_dc.enabled && g_dc.cull; }

// ---- helpers
static void screen_size(int* W, int* H) {   // what 0x41FD70 returns
    *W = *H = 0;
    __try { DWORD r = RD32(A_RENDERER); if (r) { *W = *(volatile int*)(uintptr_t)(r + 0x328); *H = *(volatile int*)(uintptr_t)(r + 0x32C); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { *W = *H = 0; }
}
static bool write_code(DWORD va, const void* src, int n) {
    DWORD op;
    if (!VirtualProtect((void*)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &op)) return false;
    memcpy((void*)(uintptr_t)va, src, n);
    VirtualProtect((void*)(uintptr_t)va, n, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)va, n);
    return true;
}

// ================================================================================================ startup size
static void __cdecl StartupResC(void* thread, const int* args) {
    int idx = -1;
    __try { idx = args ? *args : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { idx = -1; }
    int W, H; screen_size(&W, &H);
    gp_log("display: startup_size = config: the saved SCREEN RESOLUTION (index %d) is not applied; the game stays %dx%d\n", idx, W, H);
    (void)thread;
}

// ================================================================================================ wide HUD
static int g_vh = 480, g_vw_written = 853;
float g_ds_vwf = 853.0f, g_ds_vhf = 480.0f, g_ds_dotx = 437.0f, g_ds_doty = 176.0f, g_ds_border = 0.05f;
static volatile LONG g_wide_calls = 0;
static void __stdcall IsWideHook(unsigned* out) {   // replaces "call 0x485640" (thiscall, 1 stack arg, ret 4; ecx ignored)
    int W, H; screen_size(&W, &H);
    gpd::Hud h = gpd::hud(W, H, g_vh);
    g_ds_vwf = (float)h.vw; g_ds_vhf = (float)h.vh; g_ds_dotx = h.dot_x; g_ds_doty = h.dot_y;
    if (h.vw != g_vw_written) {
        static const BYTE pre[4] = {0xC7, 0x44, 0x24, 0x0C};
        if (!memcmp((const void*)(uintptr_t)A_WH_W853, pre, 4) && write_code(A_WH_W853 + 4, &h.vw, 4)) g_vw_written = h.vw;
    }
    if (out) *out = 1;
    LONG k = InterlockedIncrement(&g_wide_calls);
    if (g_dc.log && (k <= 20 || k % 100 == 0))
        gp_log("display: is_wide_screen hook #%ld: back buffer %dx%d -> HUD canvas %dx%d (%.4f px/unit), aim dot (%.1f, %.1f)\n",
               k, W, H, h.vw, h.vh, h.vh ? (double)H / h.vh : 0.0, h.dot_x, h.dot_y);
}

// ================================================================================================ menus: sprites
static volatile LONG g_spr_n[3] = {0, 0, 0};
static void __cdecl SprMtxC(const float* args, float* m, const BYTE* frame, int layout) {
    float sx = args[0], sy = args[1], sz = args[2];
    float cx, cy, hw, hh;
    if (layout == 0) { cx = *(const float*)(frame + 0x74); cy = *(const float*)(frame + 0x78); hw = *(const float*)(frame + 0x30); hh = *(const float*)(frame + 0x14); }
    else             { cx = *(const float*)(frame + 0x60); cy = *(const float*)(frame + 0x64); hw = *(const float*)(frame + 0x2C); hh = *(const float*)(frame + 0x10); }
    gpd::Cls c = gpd::classify(cx, cy, hw, hh);
    gpd::write_matrix(m, gpd::sprite_map(sx, sy, c, g_dc.bands, g_dc.full), sz);
    LONG k = InterlockedIncrement(&g_spr_n[c]);
    if (g_dc.log && k == 1) {
        static const char* nm[3] = {"box (fitted)", "full-width band", "full-screen"};
        gp_log("display: first screen-relative sprite of kind %s: centre (%.0f, %.0f) half (%.0f, %.0f), screen %.0fx%.0f\n",
               nm[c], cx, cy, hw, hh, sx * 800.0f, sy * 600.0f);
    }
}
// call sites DS_SPR_MTX 0x8F4D20 / DS_SPR2_MTX 0x8F6EED: ecx = matrix, [esp+4] sx, [esp+8] sy, [esp+0Ch] 1.0; the
// caller's frame (the offsets its code uses) starts at esp+10h (3 pushes + the return address)
__declspec(naked) void SprMtxStubA() {
    __asm mov eax, esp
    __asm push ecx
    __asm push 0
    __asm lea edx, [eax + 0x10]
    __asm push edx
    __asm push ecx
    __asm lea edx, [eax + 4]
    __asm push edx
    __asm call SprMtxC
    __asm add esp, 16
    __asm pop eax
    __asm ret 0xC
}
__declspec(naked) void SprMtxStubB() {
    __asm mov eax, esp
    __asm push ecx
    __asm push 1
    __asm lea edx, [eax + 0x10]
    __asm push edx
    __asm push ecx
    __asm lea edx, [eax + 4]
    __asm push edx
    __asm call SprMtxC
    __asm add esp, 16
    __asm pop eax
    __asm ret 0xC
}

// ================================================================================================ menus: text
float g_ds_tag = 0.0f, g_ds_tox = 0.0f, g_ds_toy = 0.0f;
static float* __cdecl FontScaleC(float* out) {   // replaces the TextBox's call of 0x8F4950 (screen-relative boxes only)
    int W, H; screen_size(&W, &H);
    gpd::Fit f = gpd::fit((float)W, (float)H);
    float t = gpd::tag_scale(f.s);
    g_ds_tox = f.ox; g_ds_toy = f.oy; g_ds_tag = t;
    out[0] = t; out[1] = t;
    return out;
}
#define DS_ADDX(o) __asm movss xmm0, dword ptr [esp + 20 + o] __asm addss xmm0, dword ptr [g_ds_tox] __asm movss dword ptr [esp + 20 + o], xmm0
#define DS_ADDY(o) __asm movss xmm0, dword ptr [esp + 20 + o] __asm addss xmm0, dword ptr [g_ds_toy] __asm movss dword ptr [esp + 20 + o], xmm0
#define DS_TAGTEST(skip) __asm push eax __asm mov eax, dword ptr [esi + 0x5C] __asm cmp eax, dword ptr [g_ds_tag] __asm jne skip \
                         __asm mov eax, dword ptr [esi + 0x60] __asm cmp eax, dword ptr [g_ds_tag] __asm jne skip \
                         __asm sub esp, 16 __asm movdqu [esp], xmm0
#define DS_TAGRESTORE __asm movdqu xmm0, [esp] __asm add esp, 16
#define DS_TAIL __asm pop eax __asm test byte ptr [esi + 0x38], 0x10
// DrawString 0x41B200, rotated glyph path, at its join 0x41B5C8 (test byte [esi+38h],10h; je 0x41B669): the four
// corners (already x font scale, or through the virtual HUD) at [esp+14h/18h] [28h/2Ch] [0Ch/10h] [1Ch/20h]
__declspec(naked) void TxtARotStub() {
    DS_TAGTEST(ta_rot_skip)
    DS_ADDX(0x14) DS_ADDY(0x18) DS_ADDX(0x28) DS_ADDY(0x2C) DS_ADDX(0x0C) DS_ADDY(0x10) DS_ADDX(0x1C) DS_ADDY(0x20)
    DS_TAGRESTORE
ta_rot_skip:
    DS_TAIL
    __asm jz ta_rot_j
    __asm jmp dword ptr [R_DS_TA_ROT]
ta_rot_j:
    __asm jmp dword ptr [R_DS_TA_ROT_J]
}
// axis-aligned path, join 0x41B805: top-left [esp+14h/18h], bottom-right [esp+0Ch/10h]
__declspec(naked) void TxtAAxStub() {
    DS_TAGTEST(ta_ax_skip)
    DS_ADDX(0x14) DS_ADDY(0x18) DS_ADDX(0x0C) DS_ADDY(0x10)
    DS_TAGRESTORE
ta_ax_skip:
    DS_TAIL
    __asm jz ta_ax_j
    __asm jmp dword ptr [R_DS_TA_AX]
ta_ax_j:
    __asm jmp dword ptr [R_DS_TA_AX_J]
}
// DrawString 0x41B9C0 (second version), rotated join 0x41BD82: corners [14h/18h] [24h/28h] [0Ch/10h] [1Ch/20h]
__declspec(naked) void TxtBRotStub() {
    DS_TAGTEST(tb_rot_skip)
    DS_ADDX(0x14) DS_ADDY(0x18) DS_ADDX(0x24) DS_ADDY(0x28) DS_ADDX(0x0C) DS_ADDY(0x10) DS_ADDX(0x1C) DS_ADDY(0x20)
    DS_TAGRESTORE
tb_rot_skip:
    DS_TAIL
    __asm jz tb_rot_j
    __asm jmp dword ptr [R_DS_TB_ROT]
tb_rot_j:
    __asm jmp dword ptr [R_DS_TB_ROT_J]
}
// axis-aligned join 0x41BFBF: [14h/18h], [0Ch/10h]
__declspec(naked) void TxtBAxStub() {
    DS_TAGTEST(tb_ax_skip)
    DS_ADDX(0x14) DS_ADDY(0x18) DS_ADDX(0x0C) DS_ADDY(0x10)
    DS_TAGRESTORE
tb_ax_skip:
    DS_TAIL
    __asm jz tb_ax_j
    __asm jmp dword ptr [R_DS_TB_AX]
tb_ax_j:
    __asm jmp dword ptr [R_DS_TB_AX_J]
}

// ================================================================================================ menus: mouse
// Sprite::TriggerMouseCallbacks 0x8F4A00 at 0x8F4AD4: [esp+0Ch/10h] = cursor pixels (float); the two replaced stores
// wrote cursor / (W/800, H/600) to [esp+14h/18h]; now the inverse of the fit. Flags (test edx,edx for the jle at
// 0x8F4AE4) and every register are kept.
static void __cdecl MouseC(BYTE* F) {
    int W, H; screen_size(&W, &H);
    gpd::Fit f = gpd::fit((float)W, (float)H);
    float u, v;
    gpd::mouse_to_canvas(f, *(float*)(F + 0x0C), *(float*)(F + 0x10), &u, &v);
    if (g_ds_mouse_off) { u = -10000.0f; v = -10000.0f; }   // test copy: key scripts keep the focus where the keys put it
    *(float*)(F + 0x14) = u; *(float*)(F + 0x18) = v;
}
__declspec(naked) void MouseStub() {
    __asm pushfd
    __asm pushad
    SAVE_XMM
    __asm lea eax, [esp + 0x80 + 0x20 + 4]
    __asm push eax
    __asm call MouseC
    __asm add esp, 4
    LOAD_XMM
    __asm popad
    __asm popfd
    __asm jmp dword ptr [R_DS_MOUSE]
}

// ================================================================================================ menus: load bar
// 0x87B830 draws the bar quad itself in pixels (unit x W/800, unit x H/600): call 0x4322A0 (ecx = batch; tex, p0..p3,
// uv0..uv3, colour) at 0x87BA5E. The four position vectors are moved onto the fitted canvas, then the original runs.
static void __cdecl LoadBarC(float** pts) {
    int W, H; screen_size(&W, &H);
    float sx = (float)W * 0.00125f, sy = (float)H * (1.0f / 600.0f);
    gpd::Fit f = gpd::fit((float)W, (float)H);
    __try { for (int i = 0; i < 4; ++i) gpd::loadbar_point(sx, sy, f, &pts[i][0], &pts[i][1]); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) void LoadBarStub() {
    __asm push ecx
    __asm lea eax, [esp + 0x0C]
    __asm push eax
    __asm call LoadBarC
    __asm add esp, 4
    __asm pop ecx
    __asm jmp dword ptr [R_DS_QUAD]
}

// ================================================================================================ fullscreen modes
// The renderer's mode list (renderer +18h entries / +1Ch count; entry {w, h, usable byte +8, refresh +0Ch, format +10h})
// is a fixed 4:3 list (640x480 .. 1600x1200, built at 0x45EA60..) that the adapter enumeration (0x4508F0) only marks.
// Fullscreen init 0x45BE30 looks the configured size up (0x450890) and falls back to 640x480 when it is not in the
// list: every wide size starts at 640x480, and setscreenresolution (0x45BF90) refuses them in fullscreen. The lookup
// calls of both get a fallback: a size the adapter really offers in X8R8G8B8 gets an entry of its own (refresh 60 Hz
// when offered, else the highest <= 85, else the lowest).
struct ModeEntry { int w, h; BYTE ok, pad[3]; UINT refresh; UINT format; };
static ModeEntry g_modes[8]; static int g_nmodes = 0;
typedef DWORD(__fastcall* ModeFind_t)(DWORD rend, DWORD edx, int w, int h, int need);
struct D3DMode { UINT w, h, refresh, format; };
static DWORD __fastcall FsLookupC(DWORD rend, DWORD edx, int w, int h, int need) {
    DWORD r = ((ModeFind_t)(uintptr_t)R_DS_MODEFIND)(rend, edx, w, h, need);
    if (r) return r;
    for (int i = 0; i < g_nmodes; ++i) if (g_modes[i].w == w && g_modes[i].h == h) return (DWORD)(uintptr_t)&g_modes[i];
    UINT best = 0;
    __try {
        DWORD d3d = *(DWORD*)(uintptr_t)(rend + 0x40);
        DWORD vt = d3d ? *(DWORD*)(uintptr_t)d3d : 0;
        if (vt) {
            typedef UINT(__stdcall* Count_t)(DWORD, UINT, UINT);
            typedef long(__stdcall* Enum_t)(DWORD, UINT, UINT, UINT, D3DMode*);
            UINT n = ((Count_t)(uintptr_t)*(DWORD*)(uintptr_t)(vt + 0x18))(d3d, 0, 0x16);
            UINT lo = 0, hi85 = 0; bool has60 = false;
            for (UINT i = 0; i < n && i < 4096; ++i) {
                D3DMode m = {};
                if (((Enum_t)(uintptr_t)*(DWORD*)(uintptr_t)(vt + 0x1C))(d3d, 0, 0x16, i, &m) < 0) continue;
                if ((int)m.w != w || (int)m.h != h) continue;
                if (m.refresh == 60) has60 = true;
                if (m.refresh <= 85 && m.refresh > hi85) hi85 = m.refresh;
                if (!lo || m.refresh < lo) lo = m.refresh;
            }
            best = has60 ? 60 : hi85 ? hi85 : lo;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { best = 0; }
    if (!best || g_nmodes >= 8) { gp_log("display: fullscreen %dx%d: not offered by the adapter (X8R8G8B8): left to the game\n", w, h); return 0; }
    ModeEntry& e = g_modes[g_nmodes++];
    e.w = w; e.h = h; e.ok = 1; e.refresh = best; e.format = 0x16;
    gp_log("display: fullscreen %dx%d @ %u Hz added (the game's own list has only 4:3 sizes)\n", w, h, best);
    return (DWORD)(uintptr_t)&e;
}

DWORD display_fs_lookup(DWORD renderer, int w, int h) {
    __try { return FsLookupC(renderer, 0, w, h, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ================================================================================================ borderless
// borderless = 1: the fullscreen device init call (0x45EBE2) goes to the windowed init (same arguments: the configured
// size), so no display mode changes; the windowed sizing (0x43B9F0, from the windowed init and from a resolution
// change) makes the window a popup without frame, centred on its monitor (exactly covering it when the size is the
// monitor's). With dpi_aware the sizes are physical pixels (no 150 % blur).
__declspec(naked) void BorderlessInitStub() { __asm jmp dword ptr [R_DS_WININIT] }
static void __fastcall BorderlessWinC(DWORD obj, DWORD, const int* sz) {
    HWND h = nullptr; int w = 0, hh = 0;
    __try { h = *(HWND*)(uintptr_t)(obj + 0x24); w = sz[0]; hh = sz[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!h || w <= 0 || hh <= 0) return;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT m = mi.rcMonitor;
    int mw = m.right - m.left, mh = m.bottom - m.top;
    int x = m.left + (w < mw ? (mw - w) / 2 : 0), y = m.top + (hh < mh ? (mh - hh) / 2 : 0);
    LONG st = GetWindowLongA(h, GWL_STYLE);
    SetWindowLongA(h, GWL_STYLE, (st & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER)) | WS_POPUP | WS_VISIBLE);
    LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
    SetWindowLongA(h, GWL_EXSTYLE, ex & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE));
    SetWindowPos(h, HWND_TOP, x, y, w, hh, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    RECT c; GetClientRect(h, &c);
    gp_log("display: borderless window %dx%d at (%d, %d) on a %dx%d monitor (client %ldx%ld)%s\n", w, hh, x, y, mw, mh,
           c.right, c.bottom, (w == mw && hh == mh) ? "" : ": not the monitor size, the desktop shows around it");
}

// ================================================================================================ install
static bool all_sites(const char* const* ids, int n) {
    for (int i = 0; i < n; ++i) if (!gp_site_ok(ids[i])) { gp_log("display: site %s differs (another mod there?)\n", ids[i]); return false; }
    return true;
}
static bool all_refs(const char* const* ids, int n) {
    for (int i = 0; i < n; ++i) if (!gp_ref_ok(ids[i])) { gp_log("display: reference %s differs\n", ids[i]); return false; }
    return true;
}
static bool oper_addr(const char* id, const void* p) { DWORD a = (DWORD)(uintptr_t)p; return gp_patch_bytes(id, &a, 4); }

int display_install() {
    if (!g_dc.enabled) return 0;
    int parts = 0;
    gp_log("display: enabled (res_table %s, startup_size %s, wide_hud %d, hud_scale %.2f, border %s, menus %d, bands %d, backgrounds %d, cull_fix %d; configpc screensize %dx%d)\n",
           g_dc.res ? "16:9" : "vanilla", g_dc.startup_config ? "config" : "saved", g_dc.wide, g_dc.hud_scale,
           g_dc.border_set ? "set" : "vanilla", g_dc.menus, g_dc.bands, g_dc.full, g_dc.cull, g_cfg_w, g_cfg_h);
    if (!gp_ref_ok("DS_SCREEN")) { gp_log("display: reference DS_SCREEN (back-buffer size) differs: [display] OFF\n"); return 0; }
    // DPI awareness first (this runs from DllMain of the loader, before the game creates its window): at 150 % scaling a
    // DPI-unaware game is drawn 1.5x by Windows (blurry, and 3024x1964 would not fit the screen)
    if (g_dc.dpi_aware) {
        typedef BOOL(WINAPI* Ctx_t)(HANDLE);
        Ctx_t f = (Ctx_t)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        BOOL ok = f ? f((HANDLE)(intptr_t)-4) : FALSE;               // per-monitor v2
        if (!ok) ok = SetProcessDPIAware();
        gp_log("display: dpi_aware: %s (window sizes are physical pixels)\n", ok ? "on" : "FAILED (already set?)");
    }
    // exclusive fullscreen at any size the adapter offers
    if (g_dc.fs_modes) {
        static const char* refs[] = {"DS_MODEFIND", "DS_FSINIT_FALLBACK"};
        if (all_refs(refs, 2) && gp_site_ok("DS_FS_LOOKUP") && gp_site_ok("DS_SR_LOOKUP")) {
            gp_patch_call("DS_FS_LOOKUP", (void*)&FsLookupC); gp_patch_call("DS_SR_LOOKUP", (void*)&FsLookupC);
            ++parts;
        } else gp_log("display: fullscreen_any_mode OFF\n");
    }
    if (g_dc.borderless) {
        static const char* ids[3] = {"DS_BL_INIT", "DS_BL_WIN1", "DS_BL_WIN2"};
        static const char* refs[] = {"DS_WININIT", "DS_WINSIZE"};
        if (all_sites(ids, 3) && all_refs(refs, 2)) {
            gp_patch_call("DS_BL_INIT", (void*)&BorderlessInitStub);
            gp_patch_call("DS_BL_WIN1", (void*)&BorderlessWinC); gp_patch_call("DS_BL_WIN2", (void*)&BorderlessWinC);
            gp_log("display: borderless on: fullscreen = true runs windowed without a frame at the configpc size (no mode change)\n");
            ++parts;
        } else gp_log("display: borderless OFF\n");
    }
    // resolution table
    if (g_dc.res) {
        static const char* ids[10] = {"DS_RES_W0", "DS_RES_H0", "DS_RES_W1", "DS_RES_H1", "DS_RES_W2", "DS_RES_H2", "DS_RES_W3", "DS_RES_H3", "DS_RES_W4", "DS_RES_H4"};
        static const char* refs[1] = {"DS_SETRES"};
        if (all_sites(ids, 10) && all_refs(refs, 1)) {
            for (int i = 0; i < 5; ++i) { gp_patch_bytes(ids[2 * i], &g_dc.table[i].w, 4); gp_patch_bytes(ids[2 * i + 1], &g_dc.table[i].h, 4); }
            gp_log("display: resolution table %dx%d, %dx%d, %dx%d, %dx%d, %dx%d (the menu still shows the old texts)\n",
                   g_dc.table[0].w, g_dc.table[0].h, g_dc.table[1].w, g_dc.table[1].h, g_dc.table[2].w, g_dc.table[2].h,
                   g_dc.table[3].w, g_dc.table[3].h, g_dc.table[4].w, g_dc.table[4].h);
            ++parts;
        } else gp_log("display: res_table OFF\n");
    }
    if (g_dc.startup_config) {
        if (gp_patch_call("DS_RES_START", (void*)&StartupResC)) ++parts;
        else gp_log("display: startup_size OFF\n");
    }
    // windowed / borderless / fullscreen in SETTINGS > SCREEN RESOLUTION (gp_display_modes.cpp)
    if (dm_enabled()) {
        if (dm_install()) ++parts;
        else gp_log("display: mode_menu OFF (the row keeps the game's five sizes)\n");
    }
    // wide HUD
    if (g_dc.wide) {
        static const char* ids[8] = {"DS_WH_DIM", "DS_WH_DOT", "DS_WH_H480", "DS_WH_W853", "DS_WH_SX", "DS_WH_SY", "DS_WH_DOTX", "DS_WH_DOTY"};
        static const char* refs[1] = {"DS_ISWIDE"};
        if (all_sites(ids, 8) && all_refs(refs, 1)) {
            g_vh = gpd::hud_height(g_dc.hud_scale);
            int W = g_cfg_w > 0 ? g_cfg_w : 1920, H = g_cfg_h > 0 ? g_cfg_h : 1080;   // until the hook sees the back buffer
            gpd::Hud h = gpd::hud(W, H, g_vh);
            g_ds_vwf = (float)h.vw; g_ds_vhf = (float)h.vh; g_ds_dotx = h.dot_x; g_ds_doty = h.dot_y;
            gp_patch_call("DS_WH_DIM", (void*)&IsWideHook);
            gp_patch_call("DS_WH_DOT", (void*)&IsWideHook);
            gp_patch_bytes("DS_WH_H480", &g_vh, 4);
            gp_patch_bytes("DS_WH_W853", &h.vw, 4); g_vw_written = h.vw;
            oper_addr("DS_WH_SX", &g_ds_vwf); oper_addr("DS_WH_SY", &g_ds_vhf);
            oper_addr("DS_WH_DOTX", &g_ds_dotx); oper_addr("DS_WH_DOTY", &g_ds_doty);
            gp_log("display: wide_hud on: HUD canvas %d high (hud_scale %.2f), width from the back buffer (%dx%d expected: %dx%d)\n", g_vh, g_dc.hud_scale, W, H, h.vw, h.vh);
            ++parts;
        } else gp_log("display: wide_hud OFF\n");
    }
    if (g_dc.border_set) {
        g_ds_border = g_dc.border;
        if (oper_addr("DS_WH_BORDER", &g_ds_border)) { gp_log("display: border %.3f (vanilla 0.05)\n", g_dc.border); ++parts; }
        else gp_log("display: border OFF\n");
    }
    // menus
    if (g_dc.menus) {
        static const char* ids[10] = {"DS_SPR_MTX", "DS_SPR2_MTX", "DS_TXT_SCALE_A", "DS_TXT_SCALE_B", "DS_TXT_A_ROT", "DS_TXT_A_AX",
                                      "DS_TXT_B_ROT", "DS_TXT_B_AX", "DS_MOUSE", "DS_LOADBAR"};
        static const char* refs[] = {"DS_MATRIX", "DS_MATRIX_RET", "DS_SPRA_CENTER", "DS_SPRA_ARGS", "DS_SPRB_CENTER", "DS_SPRB_ARGS",
                                     "DS_FONTSCALE", "DS_TXT_RESET_A", "DS_TXT_RESET_B", "DS_TA_ROT1", "DS_TA_ROT2", "DS_TA_ROT3", "DS_TA_AX",
                                     "DS_TB_ROT1", "DS_TB_ROT2", "DS_TB_ROT3", "DS_TB_AX", "DS_TA_ROT_J", "DS_TA_AX_J", "DS_TB_ROT_J", "DS_TB_AX_J",
                                     "DS_MOUSE_IN1", "DS_MOUSE_IN2", "DS_MOUSE_J", "DS_QUAD", "DS_QUAD_RET", "DS_LB_SCALE", "DS_LB_ARGS"};
        if (all_sites(ids, 10) && all_refs(refs, (int)(sizeof refs / sizeof refs[0]))) {
            gp_patch_call("DS_SPR_MTX", (void*)&SprMtxStubA);
            gp_patch_call("DS_SPR2_MTX", (void*)&SprMtxStubB);
            gp_patch_call("DS_TXT_SCALE_A", (void*)&FontScaleC);
            gp_patch_call("DS_TXT_SCALE_B", (void*)&FontScaleC);
            gp_patch_call("DS_TXT_A_ROT", (void*)&TxtARotStub);
            gp_patch_call("DS_TXT_A_AX", (void*)&TxtAAxStub);
            gp_patch_call("DS_TXT_B_ROT", (void*)&TxtBRotStub);
            gp_patch_call("DS_TXT_B_AX", (void*)&TxtBAxStub);
            gp_patch_call("DS_MOUSE", (void*)&MouseStub);
            gp_patch_call("DS_LOADBAR", (void*)&LoadBarStub);
            gp_log("display: menus on: the 800x600 canvas is fitted (pillarboxed) - sprites, text, mouse, load bar; bands %s, backgrounds %s\n",
                   g_dc.bands == gpd::MODE_FIT ? "fit" : g_dc.bands == gpd::MODE_COVER ? "cover" : "stretch",
                   g_dc.full == gpd::MODE_FIT ? "fit" : g_dc.full == gpd::MODE_COVER ? "cover" : "stretch");
            ++parts;
        } else gp_log("display: menus OFF\n");
    }
    return parts;
}

// ---- test host only
static void __cdecl TestSetRenderer(DWORD a) { A_RENDERER = a; }
static void __cdecl TestSetQuad(DWORD a) { R_DS_QUAD = a; }
static void __cdecl TestSetMouseBack(DWORD a) { R_DS_MOUSE = a; }
static void __cdecl TestSetTextBack(DWORD fall, DWORD jump) {
    R_DS_TA_ROT = R_DS_TA_AX = R_DS_TB_ROT = R_DS_TB_AX = fall;
    R_DS_TA_ROT_J = R_DS_TA_AX_J = R_DS_TB_ROT_J = R_DS_TB_AX_J = jump;
}
void* display_test_stub(const char* name) {
    if (void* p = dm_test_stub(name)) return p;
    if (!strcmp(name, "ds_spr_a")) return (void*)&SprMtxStubA;
    if (!strcmp(name, "ds_spr_b")) return (void*)&SprMtxStubB;
    if (!strcmp(name, "ds_fontscale")) return (void*)&FontScaleC;
    if (!strcmp(name, "ds_txt_a_rot")) return (void*)&TxtARotStub;
    if (!strcmp(name, "ds_txt_a_ax")) return (void*)&TxtAAxStub;
    if (!strcmp(name, "ds_txt_b_rot")) return (void*)&TxtBRotStub;
    if (!strcmp(name, "ds_txt_b_ax")) return (void*)&TxtBAxStub;
    if (!strcmp(name, "ds_mouse")) return (void*)&MouseStub;
    if (!strcmp(name, "ds_loadbar")) return (void*)&LoadBarStub;
    if (!strcmp(name, "ds_iswide")) return (void*)&IsWideHook;
    if (!strcmp(name, "ds_set_renderer")) return (void*)&TestSetRenderer;
    if (!strcmp(name, "ds_set_quad")) return (void*)&TestSetQuad;
    if (!strcmp(name, "ds_set_mouse_back")) return (void*)&TestSetMouseBack;
    if (!strcmp(name, "ds_set_text_back")) return (void*)&TestSetTextBack;
    if (!strcmp(name, "ds_vwf")) return (void*)&g_ds_vwf;
    if (!strcmp(name, "ds_dotx")) return (void*)&g_ds_dotx;
    if (!strcmp(name, "ds_tag")) return (void*)&g_ds_tag;
    return nullptr;
}
