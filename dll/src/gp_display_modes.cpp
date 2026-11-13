// gp_display_modes.cpp - [display] mode_menu: the game's own SETTINGS > SCREEN RESOLUTION row offers the
// real modes of the monitor the window is on (windowed sizes that fit, borderless at the monitor's size, exclusive
// fullscreen at the adapter's sizes) and applies the chosen one LIVE through the game's own confirm flow ("Screen
// Resolution has changed. To keep changes press [Enter] ... revert in %0 seconds"). The choice is the mod's: it is kept
// in <Documents>\Total Overdose\savegames\TODGameplay_display.txt and applied when the device is created; the saved
// option (Slot11 Setting_ScreenSize) only ever gets a vanilla index 0..4.
//
// How the game does it (static analysis): the row is Trigger_Root_Menu option type 20. A value
// change calls the settings setter 0x6BA420 case 20 (HUD_intermission data +11Ch PCScreenSize = index, +360h pending);
// Enter on the row with PCScreenSize != OldPcScreenSize (+120h) sends command_apply_screenres (1184), whose thread
// 0x6D5800 calls the table 0x4AB940 (index -> setscreenresolution 0x45BF90: window resize / mode list + device Reset
// 0x451020), shows the countdown, then keeps (Old = PCScreenSize) or reverts (setter + table with Old) and writes
// Setting_ScreenSize = Old. The engine has a dead windowed <-> fullscreen toggle 0x45C0D0 (Reset with present Windowed
// flipped): a live mode switch is a Reset the engine was written for.
//
// Sites (sites.h rows DS_MM_*, plus DS_BL_INIT / DS_BL_WIN1 / DS_BL_WIN2 with new targets):
//   DS_MM_WININIT / DS_BL_INIT  device setup 0x45E620: both init calls -> ModeInitC (the mod's mode and size)
//   DS_BL_WIN1 / DS_BL_WIN2     window sizing 0x43B9F0 -> ModeWinC (frame or popup per the mode, centred on its monitor)
//   DS_MM_APPLY / DS_MM_REVERT  the apply thread's table calls -> ModeTableC (the row's mode, live Reset)
//   DS_MM_OLDGET                page open: OldPcScreenSize = the row index of the mode in use (not the saved 0..4)
//   DS_MM_STORE                 the thread's Setting_ScreenSize write: the mode is kept (side file, configpc, registry),
//                               the saved option gets the vanilla index
//   DS_MM_SETTEXT               TextBox::SetTextIndex 0x60FA50: the row's values are codes 0x7E00+i (not text ordinals):
//                               the box gets the label text as its own text (+B8h, which the render prefers)
//   menu helper type 20         on_get: the row's value list = the modes, CurrentIntegerValue = the mode in use
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include "gp_glue.h"
#include "gp_display_logic.h"
#include "gp_display_modes.h"
#pragma comment(lib, "advapi32.lib")

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define RDF(a) (*(volatile float*)(uintptr_t)(a))

// ---- game addresses (REF rows DS_SCREEN, DS_MM_*)
static DWORD A_DM_RENDERER = 0xA39F14;              // renderer pointer
static const DWORD A_WINOBJ = 0xA35EB8;             // window object pointer; +24h HWND
static const DWORD A_VHUD = 0xA08810, A_WIDE_EMU = 0xA39F12, A_K16 = 0x9B6A60, A_K9 = 0x9B6A5C, A_REGKEY = 0xA09178;
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764;
// renderer members: +310h fullscreen flag (byte), +314h back-buffer format, +320h/+324h configured size, +328h/+32Ch
// back buffer, +178h D3DPRESENT_PARAMETERS (W, H, format +180h, Windowed +198h, refresh +1A8h), +1B0h reset pending /
// device lost (byte), +40h IDirect3D9, +5548h ratio
static const DWORD RO_FS = 0x310, RO_FMT = 0x314, RO_CFGW = 0x320, RO_CFGH = 0x324, RO_W = 0x328, RO_H = 0x32C,
                   RO_PP = 0x178, RO_PP_FMT = 0x180, RO_PP_WINDOWED = 0x198, RO_PP_REFRESH = 0x1A8, RO_LOST = 0x1B0,
                   RO_D3D = 0x40, RO_RATIO = 0x5548;
static const DWORD HD_PCSIZE = 0x11C, HD_OLDSIZE = 0x120;   // HUD_intermission data: PCScreenSize, OldPcScreenSize
static const int CMD_GET_SCREEN_DIM = 1096, CMD_REPOSITION_HUD = 1088;   // global command ids (data/lib/command_table.json)
static const int CODE_BASE = 0x7E00, MAX_MODES = 48;
static const float LABEL_CHAR_W = 13.0f;          // menu font, capitals / digits: 12.5 units each at scale 1 (in game)

// ---- original targets / continuations (check_sites.py: STUB_CONST / JUMP_REF)
static DWORD R_DM_FSINIT = 0x45BE30;   // DS_BL_INIT original target (fullscreen init, thiscall (w, h) ret 8)
static DWORD R_DM_WININIT = 0x45BEF0;  // DS_MM_WININIT original target (windowed init, same signature)
static DWORD R_DM_WINSIZE = 0x43B9F0;  // DS_BL_WIN1 / DS_BL_WIN2 original target (not called: ModeWinC sizes the window)
static DWORD R_DM_TABLE = 0x4AB940;    // DS_MM_APPLY / DS_MM_REVERT original target (the 5-entry table, not called)
static DWORD R_DM_MGET = 0x869E80;     // DS_MM_OLDGET original target (member get, thiscall (slot, out) ret 8)
static DWORD R_DM_MSET = 0x869EA0;     // DS_MM_STORE original target (member set, same signature)
static DWORD R_DM_SETIDX = 0x60FA56;   // DS_MM_SETTEXT: SetTextIndex after its first three instructions
static DWORD P_DM_RESET = 0x451020, P_DM_VHUD = 0x420D90, P_DM_ALLOC = 0x478180, P_DM_SENDCMD = 0x86A340, P_DM_FIND = 0x88EB60;

typedef char(__fastcall* Init_t)(DWORD rend, void* edx, int w, int h);
typedef void(__fastcall* Reset_t)(DWORD rend);
typedef void(__fastcall* Vhud_t)(DWORD vh, void* edx, float w, float h, float ratio, float safe);
typedef void*(__cdecl* Alloc_t)(int pool, int size);
typedef void(__fastcall* Member_t)(DWORD node, void* edx, int slot, void* io);
typedef char(__fastcall* SendCmd_t)(DWORD node, void* edx, int id, void* args);
typedef void(__fastcall* Find_t)(DWORD parent, void* edx, void* q);
struct D3DModeDesc { UINT w, h, refresh, format; };

// ---- state
static DmCfg g_cfg;
static gpd::DMode g_cur = {0, 0, -1};            // the mode in use (kind -1: not set yet)
static gpd::DMode g_saved = {0, 0, -1};          // what the side file holds
static bool g_have_file = false; static gpd::DMode g_file = {0, 0, -1}; static int g_file_mx = 0, g_file_my = 0; static bool g_file_hm = false;
static gpd::DMode g_list[MAX_MODES]; static int g_n = 0; static int g_native_w = 0, g_native_h = 0;
static LONG g_style_win = 0, g_ex_win = 0; static bool g_style_known = false;
static DWORD g_hud_node = 0;
static char g_sidecar[MAX_PATH + 64] = "";
static bool g_installed = false;
static DWORD g_scaled_box[8]; static float g_scaled_orig[8][2]; static int g_nscaled = 0;
static volatile LONG g_labels = 0;

void dm_load_config(const gp::Ini& ini, const DmCfg& base) {
    DmCfg c = base;
    c.on = ini.geti("display", "mode_menu", 1) != 0;
    c.write_configpc = ini.geti("display", "write_configpc", 1) != 0;
    c.label_chars = ini.geti("display", "label_chars", 0);   // 0 = from the value box's width
    if (c.label_chars < 0) c.label_chars = 0;
    c.relayout_hud = ini.geti("display", "relayout_hud", 1) != 0;
    g_cfg = c;
    if (!c.on) return;
    char docs[MAX_PATH] = "";
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
        sprintf_s(g_sidecar, "%s\\Total Overdose\\savegames\\TODGameplay_display.txt", docs);
    FILE* f = nullptr;
    if (g_sidecar[0] && fopen_s(&f, g_sidecar, "rb") == 0 && f) {
        char t[1024]; size_t n = fread(t, 1, sizeof t - 1, f); fclose(f); t[n] = 0;
        g_have_file = gpd::parse_mode_file(t, &g_file, &g_file_mx, &g_file_my, &g_file_hm);
        if (g_have_file) g_saved = g_file;
    }
}
bool dm_enabled() { return g_cfg.on; }

// ================================================================================================ helpers
static DWORD renderer() { __try { return RD32(A_DM_RENDERER); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static HWND game_hwnd() { __try { DWORD w = RD32(A_WINOBJ); return w ? (HWND)(uintptr_t)RD32(w + 0x24) : nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; } }
static void capture_style(HWND h) {
    if (g_style_known || !h) return;
    LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE);
    g_style_win = (st & WS_CAPTION) ? st : (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (st & WS_VISIBLE));
    g_ex_win = ex & ~WS_EX_TOPMOST;
    g_style_known = true;
    gp_log("display modes: the game's window style %08lX / ex %08lX (kept for windowed mode)\n", g_style_win, g_ex_win);
}
static UINT window_dpi(HWND h) {
    typedef UINT(WINAPI* Dpi_t)(HWND);
    static Dpi_t f = (Dpi_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    UINT d = (f && h) ? f(h) : 96;
    return d ? d : 96;
}
static RECT framed(HWND h, int w, int hh) {   // window rectangle around a w x hh client area for the windowed style
    RECT r = {0, 0, w, hh};
    typedef BOOL(WINAPI* Adj_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Adj_t f = (Adj_t)GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi");
    LONG st = g_style_known ? g_style_win : (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    BOOL menu = h && GetMenu(h) != nullptr;
    if (f) f(&r, (DWORD)st, menu, (DWORD)g_ex_win, window_dpi(h));
    else AdjustWindowRectEx(&r, (DWORD)st, menu, (DWORD)g_ex_win);
    return r;
}
struct MonCtx { gpd::Mon m; RECT rm, rw; HMONITOR h; bool primary_adapter; float dpi_scale; };
// the monitor as the desktop has it: during exclusive fullscreen the monitor runs the game's mode (e.g. 1920x1080), so
// the last measurement taken at the desktop's own mode (EnumDisplaySettings registry mode = the current size) is used
static MonCtx g_mc_desk; static bool g_mc_desk_ok = false;
static bool at_desktop_mode(HMONITOR hm, const RECT& rm) {
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(hm, (MONITORINFO*)&mi)) return true;
    DEVMODEA dm = {}; dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsExA(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm, 0)) return true;
    return (int)dm.dmPelsWidth == rm.right - rm.left && (int)dm.dmPelsHeight == rm.bottom - rm.top;
}
static bool mon_of(HWND h, MonCtx* c) {
    memset(c, 0, sizeof *c);
    c->h = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoA(c->h, &mi)) return false;
    if (!at_desktop_mode(c->h, mi.rcMonitor) && g_mc_desk_ok && g_mc_desk.h == c->h) { *c = g_mc_desk; return true; }
    c->rm = mi.rcMonitor; c->rw = mi.rcWork;
    c->m.mon_w = mi.rcMonitor.right - mi.rcMonitor.left; c->m.mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    c->m.work_w = mi.rcWork.right - mi.rcWork.left; c->m.work_h = mi.rcWork.bottom - mi.rcWork.top;
    RECT f = framed(h, 1000, 1000);
    c->m.frame_w = (f.right - f.left) - 1000; c->m.frame_h = (f.bottom - f.top) - 1000;
    c->dpi_scale = window_dpi(h) / 96.0f;
    // exclusive fullscreen runs on the adapter the device was created on (adapter 0): only when the window is there
    c->primary_adapter = false;
    DWORD r = renderer();
    __try {
        DWORD d3d = r ? RD32(r + RO_D3D) : 0; DWORD vt = d3d ? RD32(d3d) : 0;
        if (vt) {
            typedef HMONITOR(__stdcall* Mon_t)(DWORD, UINT);
            HMONITOR am = ((Mon_t)(uintptr_t)RD32(vt + 0x3C))(d3d, 0);
            c->primary_adapter = am == c->h;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { c->primary_adapter = false; }
    if (at_desktop_mode(c->h, mi.rcMonitor)) { g_mc_desk = *c; g_mc_desk_ok = true; }
    return true;
}
static int adapter_sizes(gpd::Res* out, int max) {   // X8R8G8B8 sizes of adapter 0 (deduplicated)
    int n = 0; DWORD r = renderer();
    __try {
        DWORD d3d = r ? RD32(r + RO_D3D) : 0; DWORD vt = d3d ? RD32(d3d) : 0;
        if (!vt) return 0;
        typedef UINT(__stdcall* Count_t)(DWORD, UINT, UINT);
        typedef long(__stdcall* Enum_t)(DWORD, UINT, UINT, UINT, D3DModeDesc*);
        UINT k = ((Count_t)(uintptr_t)RD32(vt + 0x18))(d3d, 0, 0x16);
        for (UINT i = 0; i < k && i < 4096 && n < max; ++i) {
            D3DModeDesc m = {};
            if (((Enum_t)(uintptr_t)RD32(vt + 0x1C))(d3d, 0, 0x16, i, &m) < 0) continue;
            bool dup = false; for (int j = 0; j < n; ++j) if (out[j].w == (int)m.w && out[j].h == (int)m.h) { dup = true; break; }
            if (!dup) { out[n].w = (int)m.w; out[n].h = (int)m.h; ++n; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static void rebuild_list(const char* why) {
    HWND h = game_hwnd(); MonCtx mc;
    if (!h || !mon_of(h, &mc)) return;
    static gpd::Res ad[256]; int nad = adapter_sizes(ad, 256);
    g_n = gpd::build_modes(mc.m, ad, nad, mc.primary_adapter, g_cur.kind >= 0 ? &g_cur : nullptr, g_list, MAX_MODES);
    g_native_w = mc.m.mon_w; g_native_h = mc.m.mon_h;
    static int last_n = -1; static gpd::DMode last_cur = {0, 0, -2};
    if (g_n != last_n || !gpd::same_mode(last_cur, g_cur)) {
        last_n = g_n; last_cur = g_cur;
        char cur[64] = "-"; if (g_cur.kind >= 0) gpd::label(g_cur, g_native_w, g_native_h, cur, sizeof cur);
        gp_log("display modes (%s): monitor %dx%d (work %dx%d, frame %dx%d, %.0f %% scaling, %s), %d adapter sizes -> %d row values, in use: %s (value %d)\n",
               why, mc.m.mon_w, mc.m.mon_h, mc.m.work_w, mc.m.work_h, mc.m.frame_w, mc.m.frame_h, mc.dpi_scale * 100.0f,
               mc.primary_adapter ? "exclusive fullscreen possible" : "not the device's adapter: no exclusive fullscreen", nad, g_n, cur,
               gpd::find_mode(g_list, g_n, g_cur));
        for (int i = 0; i < g_n; ++i) { char b[64]; gpd::label(g_list[i], g_native_w, g_native_h, b, sizeof b); gp_log("  value %2d: %s\n", i, b); }
    }
}
static int cur_index() { return gpd::find_mode(g_list, g_n, g_cur); }

// window frame / popup and position for a mode, on the window's monitor
static void place_window(HWND h, const gpd::DMode& m) {
    if (!h) return;
    capture_style(h);
    MonCtx mc; if (!mon_of(h, &mc)) return;
    int x, y, cw, ch; LONG st, ex; HWND z;
    if (m.kind == gpd::K_WINDOWED) {
        st = g_style_win | WS_VISIBLE; ex = g_ex_win;
        SetWindowLongA(h, GWL_STYLE, st); SetWindowLongA(h, GWL_EXSTYLE, ex);
        RECT r = framed(h, m.w, m.h); cw = r.right - r.left; ch = r.bottom - r.top;
        int ww = mc.rw.right - mc.rw.left, wh = mc.rw.bottom - mc.rw.top;
        x = mc.rw.left + (cw < ww ? (ww - cw) / 2 : 0); y = mc.rw.top + (ch < wh ? (wh - ch) / 2 : 0);
        z = HWND_NOTOPMOST;
    } else {
        st = (GetWindowLongA(h, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER)) | WS_POPUP | WS_VISIBLE;
        ex = GetWindowLongA(h, GWL_EXSTYLE) & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
        SetWindowLongA(h, GWL_STYLE, st); SetWindowLongA(h, GWL_EXSTYLE, ex);
        cw = m.w; ch = m.h;
        int mw = mc.m.mon_w, mh = mc.m.mon_h;
        x = mc.rm.left + (cw < mw ? (mw - cw) / 2 : 0); y = mc.rm.top + (ch < mh ? (mh - ch) / 2 : 0);
        if (m.kind == gpd::K_FULLSCREEN) { x = mc.rm.left; y = mc.rm.top; }
        z = HWND_NOTOPMOST;
    }
    // the z order stays as it is (a window behind others stays behind); only a TOPMOST left by exclusive mode is removed
    UINT zf = (GetWindowLongA(h, GWL_EXSTYLE) & WS_EX_TOPMOST) && m.kind != gpd::K_FULLSCREEN ? 0 : SWP_NOZORDER;
    SetWindowPos(h, z, x, y, cw, ch, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE | zf);
    RECT c = {}; GetClientRect(h, &c);
    gp_log("display modes: window %s %dx%d at (%d, %d), client %ldx%ld\n", gpd::kind_name(m.kind), cw, ch, x, y, c.right, c.bottom);
}

// ================================================================================================ startup
static gpd::DMode startup_mode(HWND h) {
    MonCtx mc; mon_of(h, &mc);
    static gpd::Res ad[256]; int nad = adapter_sizes(ad, 256);
    gpd::DMode m;
    if (g_have_file) {
        if (g_file_hm) {   // the monitor it was chosen on, when it still exists
            POINT p = {g_file_mx, g_file_my};
            HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
            MONITORINFO mi = {sizeof mi};
            if (hm && hm != mc.h && GetMonitorInfoA(hm, &mi)) {
                SetWindowPos(h, nullptr, mi.rcWork.left, mi.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                mon_of(h, &mc);
                gp_log("display modes: the window moved to the monitor at (%d, %d) (the side file's)\n", g_file_mx, g_file_my);
            }
        }
        m = gpd::validate(g_file, mc.m, ad, nad, mc.primary_adapter);
        gp_log("display modes: %s says %s %dx%d -> %s %dx%d on this monitor\n", g_sidecar, gpd::kind_name(g_file.kind), g_file.w, g_file.h,
               gpd::kind_name(m.kind), m.w, m.h);
    } else {
        m = gpd::startup_default(g_cfg.cfg_fs, g_cfg.cfg_w, g_cfg.cfg_h, g_cfg.borderless_ini, mc.m, ad, nad, mc.primary_adapter);
        gp_log("display modes: no %s yet: configpc fullscreen %d, screensize %dx%d, [display] borderless %d -> %s %dx%d\n", g_sidecar,
               g_cfg.cfg_fs, g_cfg.cfg_w, g_cfg.cfg_h, g_cfg.borderless_ini, gpd::kind_name(m.kind), m.w, m.h);
    }
    return m;
}
static void set_sizes(DWORD r, int w, int h) {
    WR32(r + RO_CFGW, w); WR32(r + RO_CFGH, h); WR32(r + RO_W, w); WR32(r + RO_H, h);
}
// replaces the device setup's call of the fullscreen init (DS_BL_INIT) and of the windowed init (DS_MM_WININIT):
// thiscall (w, h) ret 8 returning al
static char __fastcall ModeInitC(DWORD rend, void*, int w, int h) {
    HWND hw = game_hwnd();
    capture_style(hw);
    gpd::DMode m = startup_mode(hw);
    g_cur = m;
    char ok = 0;
    __try {
        set_sizes(rend, m.w, m.h);
        if (m.kind == gpd::K_FULLSCREEN) {
            place_window(hw, m);
            ok = ((Init_t)(uintptr_t)R_DM_FSINIT)(rend, nullptr, m.w, m.h);
            if (!ok || RD32(rend + RO_W) != (DWORD)m.w || RD32(rend + RO_H) != (DWORD)m.h) {
                gp_log("display modes: exclusive fullscreen %dx%d did not start (init %d, back buffer %dx%d): borderless instead\n",
                       m.w, m.h, ok, RD32(rend + RO_W), RD32(rend + RO_H));
                MonCtx mc; mon_of(hw, &mc);
                if (ok) {   // a device exists at another size (the game's 640x480 fallback): the row shows what is in use
                    g_cur.kind = gpd::K_FULLSCREEN; g_cur.w = (int)RD32(rend + RO_W); g_cur.h = (int)RD32(rend + RO_H);
                } else {
                    m = gpd::DMode{mc.m.mon_w, mc.m.mon_h, gpd::K_BORDERLESS}; g_cur = m;
                    set_sizes(rend, m.w, m.h);
                    ok = ((Init_t)(uintptr_t)R_DM_WININIT)(rend, nullptr, m.w, m.h);
                }
            }
        } else {
            ok = ((Init_t)(uintptr_t)R_DM_WININIT)(rend, nullptr, m.w, m.h);   // sizes the window through ModeWinC
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("display modes: exception in the device init\n"); }
    gp_log("display modes: device created %s %dx%d (the game asked for %dx%d; back buffer %dx%d, fullscreen flag %d)\n", gpd::kind_name(g_cur.kind),
           g_cur.w, g_cur.h, w, h, RD32(rend + RO_W), RD32(rend + RO_H), (int)*(volatile BYTE*)(uintptr_t)(rend + RO_FS));
    return ok;
}
// replaces the window sizing 0x43B9F0 (thiscall (int size[2]) ret 4) at the windowed init and at setscreenresolution
static void __fastcall ModeWinC(DWORD obj, void*, const int* sz) {
    HWND h = nullptr; int w = 0, hh = 0;
    __try { h = (HWND)(uintptr_t)RD32(obj + 0x24); w = sz[0]; hh = sz[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!h || w <= 0 || hh <= 0) return;
    gpd::DMode m = {w, hh, g_cur.kind >= 0 ? g_cur.kind : gpd::K_WINDOWED};
    if (m.kind == gpd::K_FULLSCREEN) m.kind = gpd::K_BORDERLESS;   // a windowed device: never an exclusive-mode window here
    if (g_cur.kind >= 0 && !(w == g_cur.w && hh == g_cur.h)) {      // setscreenresolution from a script: the row follows
        gp_log("display modes: the game resized the window to %dx%d\n", w, hh);
        g_cur.w = w; g_cur.h = hh; g_cur.kind = m.kind;
    }
    place_window(h, m);
}

// ================================================================================================ live change
// every Sprite / TextBox keeps its own render list (+94h) in pixels and rebuilds it only when its node is dirty (+14h bit
// 3; Sprite per-frame render 0x8F6CC0): after a size change the menus would keep drawing at the old size. The whole node
// tree (from the root above the HUD) is walked once and every Sprite / TextBox (vtables 0x9D2F50 / 0x9D35F8) is marked.
static const DWORD VT_SPRITE = 0x9D2F50, VT_TEXTBOX = 0x9D35F8;
static int dirty_sprites(DWORD from) {
    static DWORD st[16384];
    int sp = 0, n = 0, seen = 0;
    __try {
        DWORD root = from;
        for (int i = 0; i < 64 && root && RD32(root + 0x40); ++i) root = RD32(root + 0x40);
        if (!root) return 0;
        st[sp++] = root;
        while (sp && seen < 400000) {
            DWORD nd = st[--sp]; ++seen;
            DWORD vt = RD32(nd);
            if (vt == VT_SPRITE || vt == VT_TEXTBOX) { *(volatile BYTE*)(uintptr_t)(nd + 0x14) |= 8; ++n; }
            DWORD sib = RD32(nd + 0x34), kid = RD32(nd + 0x44);
            if (sib && sp < 16384) st[sp++] = sib;
            if (kid && sp < 16384) st[sp++] = kid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("display modes: exception in the sprite walk after %d nodes\n", seen); }
    gp_log("display modes: %d of %d nodes are sprites / text boxes: marked for a rebuild at the new size\n", n, seen);
    return n;
}
static void hud_relayout() {
    if (!g_cfg.relayout_hud || !g_hud_node) return;
    DWORD k1 = RD32(A_CUR_NODE), k2 = RD32(A_CUR_2), k3 = RD32(A_CUR_3);
    __try {
        DWORD hud = RD32(g_hud_node + 0x40);                       // HUD_intermission's parent: RealTimeFolder/HUD (HUD_root)
        const char* nm = hud ? (const char*)(uintptr_t)RD32(hud + 0x4C) : nullptr;
        if (!nm || strcmp(nm, "HUD")) { gp_log("display modes: HUD relayout skipped (parent node '%s')\n", nm ? nm : "?"); return; }
        DWORD rtf = RD32(hud + 0x40);
        struct { DWORD out; const char* name; } q = {0, "MasterControl"};
        if (rtf) ((Find_t)(uintptr_t)P_DM_FIND)(rtf, nullptr, &q);
        DWORD args[4] = {0, 0, 0, 0};
        char a = q.out ? ((SendCmd_t)(uintptr_t)P_DM_SENDCMD)(q.out, nullptr, CMD_GET_SCREEN_DIM, args) : 0;
        char b = ((SendCmd_t)(uintptr_t)P_DM_SENDCMD)(hud, nullptr, CMD_REPOSITION_HUD, args);
        gp_log("display modes: HUD relayout: MasterControl %08X command_get_screen_dimensions %d, HUD %08X command_reposition_HUD %d\n", q.out, a, hud, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("display modes: exception in the HUD relayout\n"); }
    if (RD32(A_CUR_NODE) != k1) WR32(A_CUR_NODE, k1);
    if (RD32(A_CUR_2) != k2) WR32(A_CUR_2, k2);
    if (RD32(A_CUR_3) != k3) WR32(A_CUR_3, k3);
}
static void virtual_hud(DWORD r, int w, int h) {
    float ratio = *(volatile BYTE*)(uintptr_t)A_WIDE_EMU ? (w * RDF(A_K16)) / (h * RDF(A_K9)) : RDF(r + RO_RATIO);
    ((Vhud_t)(uintptr_t)P_DM_VHUD)(A_VHUD, nullptr, (float)w, (float)h, ratio, 1.0f);
}
struct Snap { DWORD pp[14]; BYTE fs; DWORD fmt, cw, ch, w, h; gpd::DMode cur; };
static void snap(DWORD r, Snap* s) {
    for (int i = 0; i < 14; ++i) s->pp[i] = RD32(r + RO_PP + 4 * i);
    s->fs = *(volatile BYTE*)(uintptr_t)(r + RO_FS); s->fmt = RD32(r + RO_FMT);
    s->cw = RD32(r + RO_CFGW); s->ch = RD32(r + RO_CFGH); s->w = RD32(r + RO_W); s->h = RD32(r + RO_H); s->cur = g_cur;
}
static void unsnap(DWORD r, const Snap& s) {
    for (int i = 0; i < 14; ++i) WR32(r + RO_PP + 4 * i, s.pp[i]);
    *(volatile BYTE*)(uintptr_t)(r + RO_FS) = s.fs; WR32(r + RO_FMT, s.fmt);
    WR32(r + RO_CFGW, s.cw); WR32(r + RO_CFGH, s.ch); WR32(r + RO_W, s.w); WR32(r + RO_H, s.h); g_cur = s.cur;
}
static bool reset_device(DWORD r) {
    ((Reset_t)(uintptr_t)P_DM_RESET)(r);
    return *(volatile BYTE*)(uintptr_t)(r + RO_LOST) == 0;
}
static bool write_mode(DWORD r, const gpd::DMode& m) {   // renderer fields for m; false when fullscreen is not offered
    if (m.kind == gpd::K_FULLSCREEN) {
        DWORD e = display_fs_lookup(r, m.w, m.h);
        if (!e) { gp_log("display modes: fullscreen %dx%d is not offered by the adapter\n", m.w, m.h); return false; }
        *(volatile BYTE*)(uintptr_t)(r + RO_FS) = 1;
        WR32(r + RO_FMT, RD32(e + 0x10)); WR32(r + RO_PP_FMT, RD32(e + 0x10));
        WR32(r + RO_PP_WINDOWED, 0); WR32(r + RO_PP_REFRESH, RD32(e + 0x0C));
    } else {
        if (*(volatile BYTE*)(uintptr_t)(r + RO_FS)) {   // leaving exclusive mode: the desktop's format
            DWORD d3d = RD32(r + RO_D3D); DWORD vt = d3d ? RD32(d3d) : 0;
            D3DModeDesc dm = {};
            typedef long(__stdcall* Disp_t)(DWORD, UINT, D3DModeDesc*);
            if (vt && ((Disp_t)(uintptr_t)RD32(vt + 0x20))(d3d, 0, &dm) >= 0) WR32(r + RO_FMT, dm.format);
        }
        *(volatile BYTE*)(uintptr_t)(r + RO_FS) = 0;
        WR32(r + RO_PP_FMT, RD32(r + RO_FMT)); WR32(r + RO_PP_WINDOWED, 1); WR32(r + RO_PP_REFRESH, 0);
    }
    set_sizes(r, m.w, m.h);
    WR32(r + RO_PP + 0, m.w); WR32(r + RO_PP + 4, m.h);
    return true;
}
static bool apply_mode(const gpd::DMode& m, const char* why) {
    DWORD r = renderer(); HWND h = game_hwnd();
    if (!r || !h) return false;
    char lb[64]; gpd::label(m, g_native_w, g_native_h, lb, sizeof lb);
    if (gpd::same_mode(m, g_cur) && RD32(r + RO_W) == (DWORD)m.w && RD32(r + RO_H) == (DWORD)m.h) {
        gp_log("display modes: %s: %s is already in use\n", why, lb); return true;
    }
    Snap s; bool ok = false;
    __try {
        snap(r, &s);
        double t0 = gp_now();
        if (write_mode(r, m)) {
            g_cur = m;
            place_window(h, m);
            bool was_fs = s.fs != 0;
            ok = reset_device(r);
            if (ok && was_fs && m.kind != gpd::K_FULLSCREEN) place_window(h, m);   // the desktop's mode is back: place again
            if (ok) {
                virtual_hud(r, m.w, m.h);
                gp_log("display modes: %s: %s applied live (device Reset OK in %.0f ms; back buffer %dx%d, present Windowed %d, refresh %d)\n", why, lb,
                       (gp_now() - t0) * 1000.0, RD32(r + RO_W), RD32(r + RO_H), RD32(r + RO_PP_WINDOWED), RD32(r + RO_PP_REFRESH));
            } else gp_log("display modes: %s: device Reset FAILED for %s: back to the previous mode\n", why, lb);
        }
        if (!ok) {
            unsnap(r, s);
            place_window(h, g_cur.kind >= 0 ? g_cur : gpd::DMode{(int)s.w, (int)s.h, gpd::K_WINDOWED});
            bool back = reset_device(r);
            virtual_hud(r, (int)s.w, (int)s.h);
            gp_log("display modes: previous mode restored (%dx%d, Reset %s)\n", s.w, s.h, back ? "OK" : "FAILED - the game retries it when it draws");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("display modes: exception while applying %s\n", lb); ok = false; }
    if (ok) { dirty_sprites(g_hud_node ? g_hud_node : 0); hud_relayout(); }
    return ok;
}

// ================================================================================================ persistence
static void ensure_dirs(const char* file) {
    char d[MAX_PATH + 64]; strcpy_s(d, file);
    for (char* p = d + 3; *p; ++p) if (*p == '\\') { *p = 0; CreateDirectoryA(d, nullptr); *p = '\\'; }
}
static void write_registry(int w, int h) {
    const char* key = "Software\\Eidos\\Total Overdose";
    __try { const char* k = (const char*)(uintptr_t)RD32(A_REGKEY); if (k && !strncmp(k, "Software", 8)) key = k; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HKEY hk; DWORD disp = 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, &disp) != ERROR_SUCCESS) return;
    DWORD a = (DWORD)w, b = (DWORD)h;
    RegSetValueExA(hk, "XRes", 0, REG_DWORD, (const BYTE*)&a, 4); RegSetValueExA(hk, "YRes", 0, REG_DWORD, (const BYTE*)&b, 4);
    RegCloseKey(hk);
}
static void write_configpc(const gpd::Res& v) {
    char p[MAX_PATH + 32]; GetModuleFileNameA(nullptr, p, MAX_PATH);
    char* sl = strrchr(p, '\\'); if (!sl) return;
    strcpy_s(sl + 1, MAX_PATH - (sl + 1 - p), "configpc.txt");
    static char in[65536], out[65536 + 256];
    FILE* f = nullptr;
    if (fopen_s(&f, p, "rb") || !f) { gp_log("display modes: no %s to update\n", p); return; }
    size_t n = fread(in, 1, sizeof in - 1, f); fclose(f); in[n] = 0;
    if (n >= sizeof in - 1) return;
    int m = gpd::configpc_set(in, false, v.w, v.h, out, (int)sizeof out);
    if (m < 0) return;
    if ((size_t)m == n && !memcmp(in, out, n)) { gp_log("display modes: configpc.txt already says windowed %dx%d\n", v.w, v.h); return; }
    char tmp[MAX_PATH + 48]; sprintf_s(tmp, "%s.todgameplay.tmp", p);
    if (fopen_s(&f, tmp, "wb") || !f) return;
    bool wr = fwrite(out, 1, (size_t)m, f) == (size_t)m; fclose(f);
    if (wr && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        gp_log("display modes: configpc.txt: fullscreen = false, screensize = %d,%d (what the game without the DLL starts with)\n", v.w, v.h);
    else { DeleteFileA(tmp); gp_log("display modes: could not update configpc.txt\n"); }
}
static void persist(const gpd::DMode& m) {
    HWND h = game_hwnd(); MonCtx mc; if (!h || !mon_of(h, &mc)) return;
    if (g_sidecar[0] && !gpd::same_mode(m, g_saved)) {
        char t[512]; int n = gpd::format_mode_file(m, mc.rm.left, mc.rm.top, t, sizeof t);
        ensure_dirs(g_sidecar);
        FILE* f = nullptr;
        if (fopen_s(&f, g_sidecar, "wb") == 0 && f) {
            fwrite(t, 1, (size_t)n, f); fclose(f); g_saved = m;
            gp_log("display modes: kept %s %dx%d in %s\n", gpd::kind_name(m.kind), m.w, m.h, g_sidecar);
        } else gp_log("display modes: could not write %s\n", g_sidecar);
    }
    if (g_cfg.write_configpc) {
        gpd::Res v = gpd::vanilla_window(m, mc.m, mc.dpi_scale);
        write_configpc(v);
        write_registry(v.w, v.h);
        gp_log("display modes: registry XRes / YRes = %d / %d\n", v.w, v.h);
    }
}

// ================================================================================================ menu hooks
// DS_MM_APPLY / DS_MM_REVERT: the apply thread's table calls, cdecl (thread, int* index)
static void __cdecl ModeTableC(void* thread, const int* idx) {
    int i = -1;
    __try {
        i = idx ? *idx : -1;
        DWORD t = (DWORD)(uintptr_t)thread; DWORD p = t ? RD32(t + 0x40) : 0;
        g_hud_node = p ? RD32(p) : 0;                              // the HUD_intermission node running the thread
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (i < 0 || i >= g_n) { gp_log("display modes: apply thread asked for value %d (%d values): nothing changed\n", i, g_n); return; }
    apply_mode(g_list[i], "SCREEN RESOLUTION");
}
// DS_MM_OLDGET: member get (thiscall (slot, out) ret 8) at the page open: OldPcScreenSize = PCScreenSize = the row index
static void __fastcall MGetC(DWORD node, void*, int slot, int* out) {
    ((Member_t)(uintptr_t)R_DM_MGET)(node, nullptr, slot, out);
    rebuild_list("settings page");
    int i = cur_index();
    __try { if (out && i >= 0) *out = i; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// DS_MM_STORE: Setting_ScreenSize = OldPcScreenSize at the end of the apply thread. The mode in use now is the truth
// (kept: the chosen one; reverted / failed: the previous one): it is persisted, OldPcScreenSize / PCScreenSize follow
// it, and the saved option gets the vanilla index (never above 4: the game without the DLL indexes its 5 values with it)
static void __fastcall MSetC(DWORD node, void*, int slot, int* val) {
    int i = cur_index();
    __try {
        DWORD hd = g_hud_node ? RD32(g_hud_node + 0x18) : 0;
        if (hd && i >= 0) { WR32(hd + HD_OLDSIZE, i); WR32(hd + HD_PCSIZE, i); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_cur.kind >= 0) persist(g_cur);
    int safe = 1;
    HWND h = game_hwnd(); MonCtx mc;
    if (h && mon_of(h, &mc)) safe = gpd::vanilla_index(gpd::vanilla_window(g_cur, mc.m, mc.dpi_scale), mc.m, mc.dpi_scale);
    char lb[64] = "?"; if (g_cur.kind >= 0) gpd::label(g_cur, g_native_w, g_native_h, lb, sizeof lb);
    gp_log("display modes: SCREEN RESOLUTION done: %s in use (value %d; the saved option gets the vanilla index %d, was %d)\n", lb, i, safe, val ? *val : -1);
    ((Member_t)(uintptr_t)R_DM_MSET)(node, nullptr, slot, &safe);
}
// menu helper, option type 20 (the SCREEN RESOLUTION row): its values are the modes
static void menu_get(DWORD node, DWORD data) {
    rebuild_list("row");
    if (!g_n) return;
    int codes[MAX_MODES]; for (int i = 0; i < g_n; ++i) codes[i] = CODE_BASE + i;
    int n = gp_menu_set_values(data, codes, g_n);
    int i = cur_index();
    __try { if (i >= 0) WR32(data + 0x4C, i); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static int logged = 0;
    if (logged++ < 6) gp_log("display modes: SCREEN RESOLUTION row: %d values (list %d), showing value %d\n", g_n, n, i);
    (void)node;
}

// ================================================================================================ value labels
// DS_MM_SETTEXT: after the original SetTextIndex(ordinal) - which clamps an ordinal past the text table and frees the
// box's own text - a row code 0x7E00 + i becomes the box's own text (+B8h, game allocator pool 0: the game frees it).
static void restore_scale(DWORD tb) {
    for (int k = 0; k < g_nscaled; ++k) if (g_scaled_box[k] == tb) {
        *(volatile float*)(uintptr_t)(tb + 0xC4) = g_scaled_orig[k][0]; *(volatile float*)(uintptr_t)(tb + 0xC8) = g_scaled_orig[k][1];
        g_scaled_box[k] = g_scaled_box[--g_nscaled];
        return;
    }
}
static void __cdecl SetIdxPost(DWORD tb, int ord) {
    __try {
        if (ord < CODE_BASE || ord >= CODE_BASE + MAX_MODES) { if (g_nscaled) restore_scale(tb); return; }
        int i = ord - CODE_BASE;
        char a[64];
        if (i < g_n) gpd::label(g_list[i], g_native_w, g_native_h, a, sizeof a);
        else strcpy_s(a, "?");
        int len = (int)strlen(a);
        WCHAR* w = (WCHAR*)((Alloc_t)(uintptr_t)P_DM_ALLOC)(0, (len + 1) * 2);
        if (!w) return;
        for (int k = 0; k <= len; ++k) w[k] = (WCHAR)(unsigned char)a[k];
        WR32(tb + 0xB8, (DWORD)(uintptr_t)w);
        // the value box wraps at its width (Sprite +80h, 800x600 units): a label wider than the box is drawn condensed
        // (x scale), so it stays on one line; label_chars > 0 overrides the measured capacity
        float bw = RDF(tb + 0x80);
        int fit = g_cfg.label_chars > 0 ? g_cfg.label_chars : (bw >= 40.0f && bw <= 800.0f ? (int)(bw / LABEL_CHAR_W) : 12);
        float s = gpd::label_scale(len, fit);
        bool known = false; for (int k = 0; k < g_nscaled; ++k) known = known || g_scaled_box[k] == tb;
        if (s < 1.0f) {
            if (!known && g_nscaled < 8) {
                g_scaled_box[g_nscaled] = tb;
                g_scaled_orig[g_nscaled][0] = RDF(tb + 0xC4); g_scaled_orig[g_nscaled][1] = RDF(tb + 0xC8); ++g_nscaled;
                known = true;
            }
            float sy = s;   // uniform: the box measures its line width with the y scale too (in game: x-only still wrapped)
            for (int k = 0; k < g_nscaled; ++k) if (g_scaled_box[k] == tb) {
                *(volatile float*)(uintptr_t)(tb + 0xC4) = g_scaled_orig[k][0] * s; *(volatile float*)(uintptr_t)(tb + 0xC8) = g_scaled_orig[k][1] * sy;
            }
        } else if (known) restore_scale(tb);
        *(volatile BYTE*)(uintptr_t)(tb + 0x14) |= 8;              // rebuild the glyphs
        LONG k = InterlockedIncrement(&g_labels);
        if (k <= 12) gp_log("display modes: value label '%s' (box %08X %.0fx%.0f units, %d characters fit, x scale %.2f)\n", a, tb, bw, RDF(tb + 0x84), fit, s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) static void SetIdxTramp() {   // the three replaced instructions, then the original body (ret 4)
    __asm sub esp, 0xC
    __asm push esi
    __asm mov esi, ecx
    __asm jmp dword ptr [R_DM_SETIDX]
}
__declspec(naked) static void SetIdxStub() {    // thiscall (ordinal), ret 4
    __asm push ecx
    __asm push dword ptr [esp + 8]
    __asm call SetIdxTramp
    __asm pop ecx
    __asm push dword ptr [esp + 4]
    __asm push ecx
    __asm call SetIdxPost
    __asm add esp, 8
    __asm ret 4
}

// ================================================================================================ install
int dm_install() {
    if (!g_cfg.on || g_installed) return 0;
    static const char* sites[] = {"DS_BL_INIT", "DS_MM_WININIT", "DS_BL_WIN1", "DS_BL_WIN2", "DS_MM_APPLY", "DS_MM_REVERT",
                                  "DS_MM_OLDGET", "DS_MM_STORE", "DS_MM_SETTEXT"};
    static const char* refs[] = {"DS_WININIT", "DS_WINSIZE", "DS_MM_RESET", "DS_MM_TOGGLE", "DS_MM_VHUD", "DS_MM_ALLOC", "DS_MM_THREAD",
                                 "DS_MM_CONFIRM", "DS_MM_TBFREE", "DS_MM_TBDRAW", "F_CMD", "F_FINDCHILD", "DS_MODEFIND"};
    for (const char* s : sites) if (!gp_site_ok(s)) { gp_log("display modes: site %s differs (another mod there?): mode_menu OFF\n", s); return 0; }
    for (const char* r : refs) if (!gp_ref_ok(r)) { gp_log("display modes: reference %s differs: mode_menu OFF\n", r); return 0; }
    if (!gp_menu_register(20, &menu_get, nullptr, "display modes")) { gp_log("display modes: the SCREEN RESOLUTION row has another handler: mode_menu OFF\n"); return 0; }
    if (!gp_menu_install()) { gp_log("display modes: menu hooks not installed: mode_menu OFF\n"); return 0; }
    int n = 0;
    n += gp_patch_call("DS_BL_INIT", (void*)&ModeInitC);
    n += gp_patch_call("DS_MM_WININIT", (void*)&ModeInitC);
    n += gp_patch_call("DS_BL_WIN1", (void*)&ModeWinC);
    n += gp_patch_call("DS_BL_WIN2", (void*)&ModeWinC);
    n += gp_patch_call("DS_MM_APPLY", (void*)&ModeTableC);
    n += gp_patch_call("DS_MM_REVERT", (void*)&ModeTableC);
    n += gp_patch_call("DS_MM_OLDGET", (void*)&MGetC);
    n += gp_patch_call("DS_MM_STORE", (void*)&MSetC);
    n += gp_patch_call("DS_MM_SETTEXT", (void*)&SetIdxStub);
    g_installed = true;
    gp_log("display modes: on (%d sites): SETTINGS > SCREEN RESOLUTION lists windowed / borderless / fullscreen modes and applies them live; "
           "side file %s (%s)\n", n, g_sidecar, g_have_file ? "found" : "not yet");
    return n;
}

// ---- test host only
static void __cdecl TestDmSetBack(DWORD setidx) { R_DM_SETIDX = setidx; }
static void __cdecl TestDmSetRenderer(DWORD a) { A_DM_RENDERER = a; }
static void __cdecl TestDmSetAlloc(DWORD a) { P_DM_ALLOC = a; }
static int __cdecl TestDmList(int w, int h, int ww, int wh, int fw, int fh) {   // builds the row for a fake monitor (no adapter)
    gpd::Mon m = {w, h, ww, wh, fw, fh};
    gpd::DMode cur = {1920, 1080, gpd::K_WINDOWED};
    g_n = gpd::build_modes(m, nullptr, 0, false, &cur, g_list, MAX_MODES);
    g_native_w = w; g_native_h = h;
    return g_n;
}
void* dm_test_stub(const char* name) {
    if (!strcmp(name, "dm_settext")) return (void*)&SetIdxStub;
    if (!strcmp(name, "dm_set_back")) return (void*)&TestDmSetBack;
    if (!strcmp(name, "dm_set_renderer")) return (void*)&TestDmSetRenderer;
    if (!strcmp(name, "dm_set_alloc")) return (void*)&TestDmSetAlloc;
    if (!strcmp(name, "dm_list")) return (void*)&TestDmList;
    return nullptr;
}
