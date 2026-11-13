// gp_pad.cpp - [pad]: native controller support for TOD.exe (GOG) - Xbox-style pads through XInput, DualShock 4 /
// DualSense through HID, button prompts that follow the device used last, rumble. Glue only; the decisions are in
// gp_pad_logic.cpp (tests/dll/test_pad.cpp), the button pictures in gp_glyphs.cpp. Design and sites: dll/HOOKS.md
// section "Pad".
//
// How it plugs into the game (everything below is the game's own console code path, fed from a pad it understands):
//  * Gamepad 0 (the object Gamepad::Gamepad 0x43A2F0 builds at start-up, [0xA35E7C][0]) gets a fake DirectInput
//    device whose GetDeviceState returns the pad as a DIJOYSTATE; the game's own Gamepad::ProcessController 0x439E00
//    turns it into button edges and stick values every frame. Type 2 (no remap), force feedback off (+0xCC = 0).
//  * ControlSetup "PC" (ControlDict) gets the bindings of the console setup "XBox" appended to its table when the game
//    builds it (event "start", handler 0x926280 -> our wrapper): every action keeps its keys and also has its pad
//    button / stick (the game's binding types 4 and 5), menus included. The cheats folder is not merged.
//  * The game picks its gamepad procedure 0x73DC70 (analog movement, right-stick turning and look) instead of the
//    keyboard / mouse one when the stub "is a pad present" (0x4853D0, always 0 on PC) says yes: four call sites are
//    pointed at a stub that answers 1 while the pad is the device used last.
//  * Rumble: the game's set_vibration (ControlSetup::SetVibration_Impl 0x924F40 -> Gamepad::SetControllerVibration
//    0x439B10) stores motor 0 / 1 at Gamepad +0xD8 / +0xDC; the game already asks only when VIBRATION is on. The DLL
//    sends those values to the pad, adds a few cheap events (player shots, landing a shoot-dodge, camera shakes =
//    explosions near the player) and scales everything by [pad] rumble_strength.
//  * Prompts: TextBox key-name parameters ([%0] filled from the Keyboard text slot) and the literal [Enter] / [Esc] /
//    [A] / [Space] of the PC texts become pad pictures at draw time (expander 0x8FD410); the pictures are 18 glyphs
//    of the game's own fonts (console code points no PC text uses) repointed at cells this DLL draws into the free
//    half of the font atlas. Keyboard style restores the original glyph records.
// With [pad] enabled = 0 (default) nothing here runs and nothing is written.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <share.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <d3d9.h>
#include "gp_glue.h"
#include "gp_pad_logic.h"
#include "gp_glyphs.h"
#include "gp_pad_prompts.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))

// ================================================================================================ game addresses
static const DWORD A_PADS = 0xA35E7C, A_PADCOUNT = 0xA08FD0, A_CACHE = 0xA3B58C, A_CUR_NODE = 0xA3B75C,
                   A_PLAYER_HOLDER = 0xA3B590;
static const DWORD C_CONTROLLER = 0x304, C_KBSLOT = 0x2D8, C_SETTINGS = 0x398, C_GAMECAM = 0x260;
static const DWORD P_START_IMPL = 0x925A60, P_GLYPH = 0x41AE90, P_MEMBER_GET = 0x869E80;
static DWORD R_PADPRESENT = 0x4853D0, R_CTLSTART = 0x926280, R_TXTPARAM = 0x8FD870, R_EXPAND = 0x8FD410,
             R_VIBRATE = 0x439B10, R_SHOT_BACK = 0x834769,
             R_USEVEH_BACK = 0x60D823, R_USEON_BACK = 0x60D64D;
// Gamepad object (TOD1 InputGameController.h): +0 device, +4.. 32 button dwords, +94h lX .. +ACh lRz, +B4h type,
// +B8h SmartJoy, +CCh force feedback, +D8h / +DCh motor 0 / 1
static const DWORD GP_DEV = 0x00, GP_TYPE = 0xB4, GP_SMARTJOY = 0xB8, GP_FF = 0xCC, GP_MOTOR0 = 0xD8, GP_MOTOR1 = 0xDC;
// Node: +34h next sibling, +40h parent, +44h first child, +4Ch name. ControlSetup: +50h entries, +54h count (0x20 each:
// +4 name, +10h Control*[], +14h count). Control: +50h code, +58h type (1 key, 3 mouse, 4 pad button, 5 pad axis)
static const DWORD N_NEXT = 0x34, N_PARENT = 0x40, N_CHILD = 0x44, N_NAME = 0x4C;
static const DWORD CS_LIST = 0x50, CS_COUNT = 0x54, CE_NAME = 0x04, CE_NODES = 0x10, CE_NCOUNT = 0x14;
static const DWORD CT_CODE = 0x50, CT_TYPE = 0x58;
// TextBox: +A4h FontAsset (+24h & ~3 = Font), +B0h param strings (10 x ushort*), +D8h flags (bit 13 glyph colours)
static const DWORD TB_FONT = 0xA4, TB_PARAMS = 0xB0, TB_FLAGS = 0xD8, FA_FONT = 0x24, FONT_TEX = 0x3C;

// ================================================================================================ config
struct PadCfg {
    bool enabled = false;
    bool xinput = true, hid = true;
    int prompts = pad::MODE_AUTO;          // [pad] prompts
    int pad_style = 0;                     // [pad] glyphs = auto (0) | xbox | playstation: forces the pad style
    bool scripts = false;                  // also answer command_use_controller_input (scripts: console menus / help)
    bool pc_menus = true;                  // START opens the menu (menu_show), the console stats pause unbound
    pad::Shape shape;
    pad::Map map;
    float rumble_strength = 1.0f;
    bool rumble = true, rumble_events = true, rumble_log = true;
    float ev_shot = 0.25f, ev_shot_s = 0.07f, ev_land = 0.55f, ev_land_s = 0.18f, ev_shake = 1.0f;
    float kb_mouse_threshold = 3.0f;
    struct Bind { char control[40]; int btn; } binds[24]; int nbinds = 0;
    char script[64] = "";
    bool log_state = false;
};
static PadCfg g_pc;

// ================================================================================================ state
static volatile LONG g_pad_mode = 0;        // read by the stub: 1 = the pad is the device used last
static pad::State g_state;                  // the merged pad of this frame
static pad::DiState g_di;
static pad::Activity g_act;
static pad::Rumble g_rum;
static pad::PadScript g_ps; static pad::PadPlayer g_pp; static bool g_script = false;
static int g_style = pad::STYLE_KEYBOARD, g_style_applied = -1;
static DWORD g_frame = 0, g_polled_frame = 0xFFFFFFFF;
static DWORD g_pad_obj = 0, g_dev_orig = 0;
static bool g_installed = false, g_attach_logged = false;
static int g_merges = 0;
static double g_now = 0;
static int g_src = 0;                        // 0 none, 1 xinput, 2 hid, 3 script
static int g_xslot = -1;

// ================================================================================================ XInput
typedef struct { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGP;
typedef struct { DWORD dwPacketNumber; XGP Gamepad; } XST;
typedef struct { WORD l, r; } XVIB;
typedef DWORD(WINAPI* XGet_t)(DWORD, XST*);
typedef DWORD(WINAPI* XSet_t)(DWORD, XVIB*);
static XGet_t g_xget = nullptr; static XSet_t g_xset = nullptr; static bool g_xtried = false;
static double g_xretry[4] = {0, 0, 0, 0}; static bool g_xconn[4] = {false, false, false, false};
static pad::State g_xs[4]; static uint32_t g_xprev[4] = {0, 0, 0, 0};
static void xinput_load() {
    if (g_xtried) return;
    g_xtried = true;
    const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* d : dlls) {
        HMODULE m = LoadLibraryA(d);
        if (!m) continue;
        g_xget = (XGet_t)GetProcAddress(m, "XInputGetState"); g_xset = (XSet_t)GetProcAddress(m, "XInputSetState");
        if (g_xget) { gp_log("pad: XInput from %s\n", d); return; }
    }
    gp_log("pad: no XInput DLL\n");
}
static bool xinput_poll(double now, pad::State& out) {   // the slot used last (or any that moved), false = none
    xinput_load();
    if (!g_xget) return false;
    int best = -1;
    for (int i = 0; i < 4; ++i) {
        if (!g_xconn[i] && now < g_xretry[i]) continue;   // a missing pad is slow to ask: every 2 s
        XST s; memset(&s, 0, sizeof s);
        if (g_xget(i, &s) != 0) {
            if (g_xconn[i]) gp_log("pad: XInput slot %d disconnected\n", i);
            g_xconn[i] = false; g_xretry[i] = now + 2.0; g_xs[i] = pad::State(); continue;
        }
        if (!g_xconn[i]) gp_log("pad: XInput pad in slot %d\n", i);
        g_xconn[i] = true;
        g_xs[i] = pad::from_xinput(s.Gamepad.wButtons, s.Gamepad.lt, s.Gamepad.rt, s.Gamepad.lx, s.Gamepad.ly, s.Gamepad.rx, s.Gamepad.ry);
        if (pad::pad_activity(g_xs[i], g_xprev[i])) best = i;
        g_xprev[i] = g_xs[i].buttons;
    }
    if (best >= 0) g_xslot = best;
    if (g_xslot < 0 || !g_xconn[g_xslot]) { g_xslot = -1; for (int i = 0; i < 4; ++i) if (g_xconn[i]) { g_xslot = i; break; } }
    if (g_xslot < 0) return false;
    out = g_xs[g_xslot];
    return true;
}

// ================================================================================================ HID (PlayStation)
struct HidDev { HANDLE h = INVALID_HANDLE_VALUE; pad::HidModel model = pad::HID_NONE; bool bt = false; USHORT in_len = 0, out_len = 0; WORD vid = 0, pid = 0; };
static HidDev g_hid; static CRITICAL_SECTION g_hcs; static pad::State g_hstate; static double g_hstamp = 0;
static volatile LONG g_hrun = 0; static HANDLE g_hthread = nullptr;
static uint8_t g_hout[2] = {0, 0}; static volatile LONG g_hout_dirty = 0;
static bool hid_open_first() {
    GUID hg; HidD_GetHidGuid(&hg);
    HDEVINFO di = SetupDiGetClassDevsA(&hg, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(di, nullptr, &hg, i, &ifd); ++i) {
        DWORD need = 0; SetupDiGetDeviceInterfaceDetailA(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need || need > 1024) continue;
        static BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_A* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buf; d->cbSize = sizeof *d;
        if (!SetupDiGetDeviceInterfaceDetailA(di, &ifd, d, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileA(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at; at.Size = sizeof at;
        pad::HidModel m = pad::HID_NONE;
        if (HidD_GetAttributes(h, &at)) m = pad::hid_model(at.VendorID, at.ProductID);
        if (m == pad::HID_NONE) { CloseHandle(h); continue; }
        PHIDP_PREPARSED_DATA pp = nullptr; HIDP_CAPS caps; memset(&caps, 0, sizeof caps);
        if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        if (caps.UsagePage != 1 || (caps.Usage != 5 && caps.Usage != 4)) { CloseHandle(h); continue; }
        char low[512]; strncpy_s(low, d->DevicePath, _TRUNCATE); _strlwr_s(low);
        g_hid.h = h; g_hid.model = m; g_hid.vid = at.VendorID; g_hid.pid = at.ProductID;
        g_hid.bt = strstr(low, "00001124-0000-1000-8000-00805f9b34fb") != nullptr || caps.InputReportByteLength > 64;
        g_hid.in_len = caps.InputReportByteLength; g_hid.out_len = caps.OutputReportByteLength;
        gp_log("pad: PlayStation pad %04X:%04X (%s, %s), input report %u bytes, output %u bytes\n", at.VendorID, at.ProductID,
               m == pad::HID_DS4 ? "DualShock 4" : "DualSense", g_hid.bt ? "Bluetooth" : "USB", caps.InputReportByteLength, caps.OutputReportByteLength);
        found = true;
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}
static void hid_write_rumble() {
    if (g_hid.h == INVALID_HANDLE_VALUE || !InterlockedExchange(&g_hout_dirty, 0)) return;
    uint8_t rep[80]; size_t n = pad::hid_rumble_report(g_hid.model, g_hid.bt, g_hout[0], g_hout[1], rep, sizeof rep);
    if (!n) { static bool told = false; if (!told) { told = true; gp_log("pad: rumble on a Bluetooth PlayStation pad is not sent (USB only)\n"); } return; }
    if (g_hid.out_len && n > g_hid.out_len) n = g_hid.out_len;
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    DWORD w = 0;
    if (!WriteFile(g_hid.h, rep, (DWORD)n, &w, &ov) && GetLastError() == ERROR_IO_PENDING) {
        // a cancelled write still owns ov and rep until the driver lets go: wait for that before they leave the stack
        if (WaitForSingleObject(ov.hEvent, 50) != WAIT_OBJECT_0) { CancelIo(g_hid.h); GetOverlappedResult(g_hid.h, &ov, &w, TRUE); }
    }
    CloseHandle(ov.hEvent);
}
static DWORD WINAPI hid_thread(void*) {
    double next_scan = 0;
    uint8_t buf[600];
    while (g_hrun) {
        if (g_hid.h == INVALID_HANDLE_VALUE) {
            double t = gp_now();
            if (t < next_scan) { Sleep(200); continue; }
            next_scan = t + 3.0;
            if (!hid_open_first()) continue;
        }
        OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0; DWORD len = g_hid.in_len && g_hid.in_len <= sizeof buf ? g_hid.in_len : 64;
        BOOL ok = ReadFile(g_hid.h, buf, len, &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 100);
            if (w == WAIT_OBJECT_0) ok = GetOverlappedResult(g_hid.h, &ov, &got, FALSE);
            else { CancelIo(g_hid.h); GetOverlappedResult(g_hid.h, &ov, &got, TRUE); ok = FALSE; got = 0; if (w == WAIT_TIMEOUT) { CloseHandle(ov.hEvent); hid_write_rumble(); continue; } }
        }
        CloseHandle(ov.hEvent);
        if (!ok) { gp_log("pad: PlayStation pad read failed (%lu): closed\n", GetLastError()); CloseHandle(g_hid.h); g_hid.h = INVALID_HANDLE_VALUE;
                   EnterCriticalSection(&g_hcs); g_hstate = pad::State(); LeaveCriticalSection(&g_hcs); continue; }
        pad::State s;
        if (pad::hid_decode(g_hid.model, g_hid.bt, buf, got, s)) {
            EnterCriticalSection(&g_hcs); g_hstate = s; g_hstamp = gp_now(); LeaveCriticalSection(&g_hcs);
        }
        hid_write_rumble();
    }
    return 0;
}
static bool hid_poll(pad::State& out) {
    if (!g_hthread) return false;
    EnterCriticalSection(&g_hcs);
    bool ok = g_hstate.connected && gp_now() - g_hstamp < 1.0;
    if (ok) out = g_hstate;
    LeaveCriticalSection(&g_hcs);
    return ok;
}

// [debug] pad script "mark hurt N" (test copy only: pad scripts run only with [debug] pad_script): Ram takes N damage
// through his own character_root take_a_hit (message -2, the way psycho hits him), owner Ram, for the death-screen tests.
static DWORD ram_root();
static void script_hurt_ram(float dmg) {
    DWORD ram = ram_root(); if (!ram || !(dmg > 0)) { gp_log("pad: hurt %.0f: no Ram\n", dmg); return; }
    if (!gp_ref_ok("F_GETPROP")) { gp_log("pad: hurt: F_GETPROP differs\n"); return; }
    typedef void(__fastcall* Get_t)(DWORD, void*, int, void*);
    DWORD vol = 0, phys = 0;
    __try { DWORD d = RD32(ram + 0x18); phys = d ? RD32(d + 0x68) : 0; if (phys) ((Get_t)(uintptr_t)0x86A6A0)(phys, nullptr, 0x69D, &vol); } __except (EXCEPTION_EXECUTE_HANDLER) { vol = 0; }
    struct { float dmg; DWORD owner, inflictor, volume; float imp[3]; float pos[3]; int type; } h = {dmg, ram, ram, vol, {0, 0, 0}, {0, 0, 0}, 1};
    bool ok = gp_send(ram, -2, &h);
    gp_log("pad: hurt: Ram %08X takes %.0f (torso volume %08X)%s\n", ram, dmg, vol, ok ? "" : " FAILED");
}

// ================================================================================================ the frame's pad
static uint32_t g_hprev = 0;
static void poll_devices() {
    if (g_polled_frame == g_frame) return;
    g_polled_frame = g_frame;
    double now = gp_now();
    pad::State xs, hs, ss;
    bool hx = g_pc.xinput && xinput_poll(now, xs);
    bool hh = g_pc.hid && hid_poll(hs);
    bool hsc = false;
    if (g_script) {
        g_pp.step(g_ps, now, [](const char* t, void*) { return gp_debug_log_has(t); }, nullptr);
        if (g_pp.mark) { gp_log("pad: mark %s\n", g_pp.mark); if (!_strnicmp(g_pp.mark, "hurt ", 5)) script_hurt_ram((float)atof(g_pp.mark + 5)); }
        if (g_pp.rumble[2] > 0) { g_rum.pulse(g_pp.rumble[0], g_pp.rumble[1], g_pp.rumble[2]); gp_log("rumble: event test pulse big %.2f little %.2f %.2f s\n", g_pp.rumble[0], g_pp.rumble[1], g_pp.rumble[2]); g_pp.rumble[2] = 0; }
        ss = g_pp.st; hsc = ss.connected;
    }
    // one device at a time: the one that moved this frame, else the one that was used last
    int src = g_src;
    bool mx = hx && pad::pad_activity(xs, g_src == 1 ? g_act.prev_buttons : 0);
    bool mh = hh && pad::pad_activity(hs, g_hprev);
    if (hsc && pad::pad_activity(ss, g_src == 3 ? g_act.prev_buttons : 0)) src = 3;
    else if (mh) src = 2;
    else if (mx) src = 1;
    if ((src == 1 && !hx) || (src == 2 && !hh) || (src == 3 && !hsc)) src = 0;
    if (!src) src = hsc ? 3 : hh ? 2 : hx ? 1 : 0;
    if (src != g_src) { gp_log("pad: input from %s\n", src == 1 ? "XInput" : src == 2 ? "PlayStation HID" : src == 3 ? "the [debug] pad script" : "no pad"); g_src = src; }
    g_hprev = hh ? hs.buttons : 0;
    g_state = src == 1 ? xs : src == 2 ? hs : src == 3 ? ss : pad::State();
    pad::to_dijoystate(g_state, g_pc.map, g_pc.shape, g_di);
}

// ================================================================================================ the fake DirectInput device
// IDirectInputDevice8A, 32 methods (stdcall, this first). Only what the game calls does something.
static HRESULT __stdcall F_QI(void*, REFIID, void** p) { if (p) *p = nullptr; return E_NOINTERFACE; }
static ULONG __stdcall F_AddRef(void*) { return 2; }
static ULONG __stdcall F_Release(void*) { return 1; }
static HRESULT __stdcall F_A1(void*, void*) { return E_NOTIMPL; }
static HRESULT __stdcall F_A3(void*, void*, void*, DWORD) { return E_NOTIMPL; }
static HRESULT __stdcall F_A2(void*, void*, void*) { return S_OK; }   // Get/SetProperty
static HRESULT __stdcall F_Acq(void*) { return S_OK; }
static HRESULT __stdcall F_GetState(void*, DWORD cb, void* data) {
    if (!data) return E_POINTER;
    __try {
        poll_devices();
        size_t n = cb < sizeof(g_di) ? cb : sizeof(g_di);
        memset(data, 0, cb); memcpy(data, &g_di, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
    return S_OK;
}
static HRESULT __stdcall F_GetData(void*, DWORD, void*, DWORD* io, DWORD) { if (io) *io = 0; return S_OK; }
static HRESULT __stdcall F_A1ok(void*, void*) { return S_OK; }
static HRESULT __stdcall F_Coop(void*, HWND, DWORD) { return S_OK; }
static HRESULT __stdcall F_ObjInfo(void*, void*, DWORD, DWORD) { return E_NOTIMPL; }
static HRESULT __stdcall F_Ctl(void*, HWND, DWORD) { return S_OK; }
static HRESULT __stdcall F_Init(void*, HINSTANCE, DWORD, REFGUID) { return S_OK; }
static HRESULT __stdcall F_Effect(void*, REFGUID, void*, void** p, void*) { if (p) *p = nullptr; return E_NOTIMPL; }
static HRESULT __stdcall F_EnumFx(void*, void*, void*, DWORD) { return S_OK; }
static HRESULT __stdcall F_FxInfo(void*, void*, REFGUID) { return E_NOTIMPL; }
static HRESULT __stdcall F_FFState(void*, DWORD* o) { if (o) *o = 0; return S_OK; }
static HRESULT __stdcall F_FFCmd(void*, DWORD) { return S_OK; }
static HRESULT __stdcall F_EnumCreated(void*, void*, void*, DWORD) { return S_OK; }
static HRESULT __stdcall F_Esc(void*, void*) { return E_NOTIMPL; }
static HRESULT __stdcall F_SendData(void*, DWORD, void*, DWORD* io, DWORD) { if (io) *io = 0; return S_OK; }
static HRESULT __stdcall F_FxFile(void*, const char*, void*, void*, DWORD) { return E_NOTIMPL; }
static HRESULT __stdcall F_WrFx(void*, const char*, DWORD, void*, DWORD) { return E_NOTIMPL; }
static HRESULT __stdcall F_Map(void*, void*, const char*, DWORD) { return E_NOTIMPL; }
static void* g_fake_vtbl[32] = {
    (void*)F_QI, (void*)F_AddRef, (void*)F_Release, (void*)F_A1 /*GetCapabilities*/, (void*)F_A3 /*EnumObjects*/,
    (void*)F_A2 /*GetProperty*/, (void*)F_A2 /*SetProperty*/, (void*)F_Acq /*Acquire*/, (void*)F_Acq /*Unacquire*/,
    (void*)F_GetState, (void*)F_GetData, (void*)F_A1ok /*SetDataFormat*/, (void*)F_A1ok /*SetEventNotification*/,
    (void*)F_Coop, (void*)F_ObjInfo, (void*)F_A1 /*GetDeviceInfo*/, (void*)F_Ctl /*RunControlPanel*/, (void*)F_Init,
    (void*)F_Effect, (void*)F_EnumFx, (void*)F_FxInfo, (void*)F_FFState, (void*)F_FFCmd, (void*)F_EnumCreated,
    (void*)F_Esc, (void*)F_Acq /*Poll*/, (void*)F_SendData, (void*)F_FxFile, (void*)F_WrFx, (void*)F_Map /*BuildActionMap*/,
    (void*)F_Map /*SetActionMap*/, (void*)F_A1 /*GetImageInfo*/};
static struct { void** vtbl; } g_fake = {g_fake_vtbl};

static void attach_pad() {   // every frame (cheap): Gamepad 0 uses the fake device
    __try {
        DWORD arr = RD32(A_PADS); DWORD gp = arr ? RD32(arr) : 0;
        if (!gp) return;
        if (RD32(gp + GP_DEV) == (DWORD)(uintptr_t)&g_fake && g_pad_obj == gp) return;
        g_pad_obj = gp; g_dev_orig = RD32(gp + GP_DEV);
        *(volatile DWORD*)(uintptr_t)(gp + GP_DEV) = (DWORD)(uintptr_t)&g_fake;
        *(volatile DWORD*)(uintptr_t)(gp + GP_TYPE) = 2;
        *(volatile DWORD*)(uintptr_t)(gp + GP_SMARTJOY) = 0;
        *(volatile BYTE*)(uintptr_t)(gp + GP_FF) = 0;
        if ((int)RD32(A_PADCOUNT) < 1) *(volatile DWORD*)(uintptr_t)A_PADCOUNT = 1;
        gp_log("pad: Gamepad 0 at %08X now reads the DLL's pad (its DirectInput device was %08X%s), type 2, force feedback off, pads found %d\n",
               gp, g_dev_orig, g_dev_orig ? ": a real DirectInput pad, no longer polled" : " (none)", (int)RD32(A_PADCOUNT));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================================================ control setups
typedef void(__fastcall* Start_t)(void* self, void* edx, const int* args);
typedef void(__fastcall* StartImpl_t)(void* self, void* edx, DWORD node);
static const char* node_name(DWORD n) { __try { const char* s = (const char*)(uintptr_t)RD32(n + N_NAME); return s ? s : ""; } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; } }
static DWORD child_named(DWORD parent, const char* name) {
    __try {
        for (DWORD c = RD32(parent + N_CHILD), k = 0; c && k < 4096; c = RD32(c + N_NEXT), ++k)
            if (!_stricmp(node_name(c), name)) return c;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static int glyph_of_node(DWORD node) {   // a Control node -> prompt picture, -1 not a pad binding
    int type = (int)RD32(node + CT_TYPE), code = (int)RD32(node + CT_CODE);
    if (type == 4 && code >= 0 && code < pad::V_COUNT) return g_pc.map.src[code];
    if (type == 5 && code >= 0 && code < 8) return code < 4 ? pad::G_LSTICK : pad::G_RSTICK;
    return -1;
}
static void apply_binds(DWORD setup) {   // [pad_bind] + the PC defaults: START opens the menu (like Esc), no stats pause
    int n = (int)RD32(setup + CS_COUNT); DWORD list = RD32(setup + CS_LIST);
    PadCfg::Bind all[32]; int na = 0;
    bool has_menu = false, has_pause = false;
    for (int b = 0; b < g_pc.nbinds; ++b) { has_menu |= !_stricmp(g_pc.binds[b].control, "menu_show"); has_pause |= !_stricmp(g_pc.binds[b].control, "pause"); }
    if (g_pc.pc_menus) {
        if (!has_menu) { strcpy_s(all[na].control, "menu_show"); all[na++].btn = pad::B_START; }
        if (!has_pause) { strcpy_s(all[na].control, "pause"); all[na++].btn = -2; }
    }
    for (int b = 0; b < g_pc.nbinds && na < 32; ++b) all[na++] = g_pc.binds[b];
    for (int b = 0; b < na; ++b) {
        int btn = all[b].btn, virt = btn >= 0 ? pad::map_virt_of(g_pc.map, btn) : -1, done = 0;
        if (btn >= 0 && virt < 0) { gp_log("pad: [pad_bind] %s = %s: no game button is fed by it (see map_*), ignored\n", all[b].control, pad::btn_name(btn)); continue; }
        for (int i = 0; i < n; ++i) {
            DWORD e = list + i * 0x20;
            const char* nm = (const char*)(uintptr_t)RD32(e + CE_NAME);
            if (!nm || _stricmp(nm, all[b].control)) continue;
            DWORD nodes = RD32(e + CE_NODES); int nn = (int)RD32(e + CE_NCOUNT);
            for (int k = 0; k < nn; ++k) {
                DWORD c = RD32(nodes + 4 * k); int t = (int)RD32(c + CT_TYPE);
                if (t != 4 && t != 5 && !(t == 2 && RD32(c + CT_CODE) == 0xFFFF)) continue;   // pad nodes (or ones we switched off)
                if (btn < 0) { *(volatile DWORD*)(uintptr_t)(c + CT_TYPE) = 2; *(volatile DWORD*)(uintptr_t)(c + CT_CODE) = 0xFFFF; }
                else { *(volatile DWORD*)(uintptr_t)(c + CT_TYPE) = 4; *(volatile DWORD*)(uintptr_t)(c + CT_CODE) = (DWORD)virt; }
                ++done;
            }
        }
        gp_log("pad: binding %s -> %s (game button %s): %d pad binding(s)\n", all[b].control, btn < 0 ? "none" : pad::btn_name(btn),
               btn < 0 ? "-" : pad::virt_name(virt), done);
    }
}
static void __fastcall CtlStartWrap(void* self, void* edx, const int* args) {
    ((Start_t)(uintptr_t)R_CTLSTART)(self, edx, args);
    DWORD s = (DWORD)(uintptr_t)self;
    __try {
        if (_stricmp(node_name(s), "PC")) return;
        DWORD parent = RD32(s + N_PARENT); DWORD xb = parent ? child_named(parent, "XBox") : 0;
        if (!xb) { gp_log("pad: ControlDict has no XBox setup next to PC: pad bindings NOT merged\n"); return; }
        int before = (int)RD32(s + CS_COUNT), nodes_before = 0, nodes_after = 0;
        for (int i = 0; i < before; ++i) nodes_before += (int)RD32(RD32(s + CS_LIST) + i * 0x20 + CE_NCOUNT);
        int folders = 0;
        for (DWORD c = RD32(xb + N_CHILD), k = 0; c && k < 64; c = RD32(c + N_NEXT), ++k) {
            if (!_stricmp(node_name(c), "cheats")) continue;   // console cheat buttons stay off
            ((StartImpl_t)(uintptr_t)P_START_IMPL)(self, nullptr, c);
            ++folders;
        }
        int after = (int)RD32(s + CS_COUNT);
        for (int i = 0; i < after; ++i) nodes_after += (int)RD32(RD32(s + CS_LIST) + i * 0x20 + CE_NCOUNT);
        apply_binds(s);
        ++g_merges;
        gp_log("pad: ControlSetup PC built: + the XBox setup (%d folders, cheats left out): %d -> %d controls, %d -> %d bindings\n",
               folders, before, after, nodes_before, nodes_after);
        if (g_merges == 1) {   // the table once, for the record
            DWORD list = RD32(s + CS_LIST);
            for (int i = 0; i < after; ++i) {
                DWORD e = list + i * 0x20; DWORD nodes = RD32(e + CE_NODES); int nn = (int)RD32(e + CE_NCOUNT);
                char line[256]; int L = sprintf_s(line, "pad:   %2d %-24s", i, (const char*)(uintptr_t)RD32(e + CE_NAME));
                for (int k = 0; k < nn && L < 200; ++k) {
                    DWORD c = RD32(nodes + 4 * k); int t = (int)RD32(c + CT_TYPE), code = (int)RD32(c + CT_CODE);
                    if (t == 4) L += sprintf_s(line + L, sizeof line - L, " pad:%s(%s)", pad::virt_name(code), pad::btn_name(g_pc.map.src[code >= 0 && code < 16 ? code : 0]));
                    else if (t == 5) L += sprintf_s(line + L, sizeof line - L, " stick:%d", code);
                    else L += sprintf_s(line + L, sizeof line - L, " t%d:%d", t, code);
                }
                gp_log("%s\n", line);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("pad: control merge faulted\n"); }
}

// ================================================================================================ prompts
// keyboard text ordinal -> key name (uk_Keyboard.txt keys; the ordinals are the same in every language)
#include "gp_pad_kb.inc"
typedef const char*(__cdecl* KeyName_t)(int);
static int key_of_ordinal(int ord, int* mouse) {   // DIK code (or mouse button in *mouse), -1 unknown
    *mouse = -1;
    if (ord < 0 || ord >= (int)(sizeof KB_ORDINAL / sizeof KB_ORDINAL[0])) return -1;
    const char* k = KB_ORDINAL[ord];
    if (!k[0]) return -1;
    static const struct { const char* n; int b; } MB[] = {{"LMB", 0}, {"RMB", 1}, {"MMB", 2}, {"MB4", 3}, {"MB5", 4}, {"MB6", 5}, {"MB7", 6}, {"MB8", 7}};
    for (auto& m : MB) if (!strcmp(k, m.n)) { *mouse = m.b; return -1; }
    __try {
        for (int d = 0; d < 256; ++d) {
            const char* n = ((KeyName_t)(uintptr_t)0x43A7D0)(d);
            if (n && !strcmp(n, k)) return d;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}
static int glyph_of_entry(DWORD e) {   // a ControlSetup entry -> the picture of its first pad binding, -1 none
    DWORD nodes = RD32(e + CE_NODES); int nn = (int)RD32(e + CE_NCOUNT);
    for (int k = 0; k < nn && k < 16; ++k) { int g = glyph_of_node(RD32(nodes + 4 * k)); if (g >= 0) return g; }
    return -1;
}
static int glyph_of_action(const char* name) {   // the picture of an action's pad binding (merged PC table), -1 none
    if (!name || !name[0]) return -1;
    static char cache_n[24][40]; static int cache_g[24]; static DWORD cache_frame[24]; static int ci = 0;
    for (int i = 0; i < 24; ++i) if (cache_n[i][0] && g_frame - cache_frame[i] < 120 && !_stricmp(cache_n[i], name)) return cache_g[i];
    int g = -1;
    __try {
        DWORD cd = gp_cache_data(); DWORD setup = cd ? RD32(cd + C_CONTROLLER) : 0;
        int n = setup ? (int)RD32(setup + CS_COUNT) : 0; DWORD list = setup ? RD32(setup + CS_LIST) : 0;
        for (int i = 0; i < n && g < 0; ++i) {
            DWORD e = list + i * 0x20; const char* nm = (const char*)(uintptr_t)RD32(e + CE_NAME);
            if (nm && !_stricmp(nm, name)) g = glyph_of_entry(e);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g = -1; }
    strncpy_s(cache_n[ci], name, _TRUNCATE); cache_g[ci] = g; cache_frame[ci] = g_frame; ci = (ci + 1) % 24;
    return g;
}
// The picture for a key parameter. The candidates are the actions bound to that key that have a pad binding (table
// order); `want` (the text's hint, padp) picks among them, else its first action is shown; no hint = the first
// candidate (the rule before the hints). *why (for the log): the chosen action's name.
static int glyph_for_ordinal(int ord, const char* want, char* why, int why_cap) {
    if (why && why_cap > 0) why[0] = 0;
    struct C { int ord; const char* want; int g; DWORD frame; char why[96]; };
    static C cache[16]; static int ci = 0;
    for (C& c : cache) if (c.ord == ord + 1 && c.want == want && g_frame - c.frame < 120) { if (why) strncpy_s(why, why_cap, c.why, _TRUNCATE); return c.g; }
    int mouse = -1, dik = key_of_ordinal(ord, &mouse), g = -1;
    const char* names[32]; int glyphs[32]; int nc = 0;
    __try {
        DWORD cd = gp_cache_data(); DWORD setup = cd ? RD32(cd + C_CONTROLLER) : 0;
        int n = setup ? (int)RD32(setup + CS_COUNT) : 0; DWORD list = setup ? RD32(setup + CS_LIST) : 0;
        for (int i = 0; i < n && nc < 32; ++i) {
            DWORD e = list + i * 0x20, nodes = RD32(e + CE_NODES); int nn = (int)RD32(e + CE_NCOUNT);
            bool key = false;
            for (int k = 0; k < nn; ++k) {
                DWORD c = RD32(nodes + 4 * k); int t = (int)RD32(c + CT_TYPE), code = (int)RD32(c + CT_CODE);
                if ((t == 1 && dik >= 0 && code == dik) || (t == 3 && mouse >= 0 && code == mouse)) key = true;
            }
            int pg = key ? glyph_of_entry(e) : -1;
            if (pg >= 0) { names[nc] = (const char*)(uintptr_t)RD32(e + CE_NAME); if (!names[nc]) names[nc] = ""; glyphs[nc++] = pg; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { nc = 0; }
    char line[96]; line[0] = 0;
    int p = padp::pick(names, nc, want);
    if (p >= 0) { g = glyphs[p]; sprintf_s(line, "%s%s", names[p], want ? " (hint)" : nc > 1 ? " (first of the key's actions)" : ""); }
    else if (want) {
        char first[40]; padp::list_first(want, first, sizeof first);
        g = glyph_of_action(first);
        sprintf_s(line, "%s (hint; the key drives %s)", first, nc ? names[0] : "no pad action");
    }
    C& c = cache[ci]; ci = (ci + 1) % 16;
    c.ord = ord + 1; c.want = want; c.g = g; c.frame = g_frame; strcpy_s(c.why, line);
    if (why) strncpy_s(why, why_cap, line, _TRUNCATE);
    return g;
}
// (TextBox, param) -> keyboard ordinal, filled by settextslotparamfromtextslot
struct ParamRec { DWORD tb; int i; int ord; };
static ParamRec g_params[96]; static int g_nparams = 0, g_pnext = 0;
static void __fastcall TxtParamWrap(void* self, void* edx, int i, DWORD slot, int index) {
    typedef void(__fastcall* F)(void*, void*, int, DWORD, int);
    ((F)(uintptr_t)R_TXTPARAM)(self, edx, i, slot, index);
    __try {
        DWORD cd = gp_cache_data(); DWORD tb = (DWORD)(uintptr_t)self;
        bool kb = cd && slot && slot == RD32(cd + C_KBSLOT);
        for (int k = 0; k < g_nparams; ++k) if (g_params[k].tb == tb && g_params[k].i == i) { g_params[k].ord = kb ? index : -1; return; }
        if (!kb) return;
        ParamRec& r = g_params[g_nparams < 96 ? g_nparams++ : (g_pnext++ % 96)];
        r.tb = tb; r.i = i; r.ord = index;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// font patching: the 18 glyphs of each font the prompts are drawn with
struct FontRec { DWORD font; DWORD glyph[pad::G_COUNT]; BYTE orig[pad::G_COUNT][0x20]; bool ok; int style; };
static FontRec g_fonts[6]; static int g_nfonts = 0;
static const int ATLAS_W = 512, ATLAS_H = 512, CELL_Y0 = 288;   // free rows of ODPC / ODPS2 (glyphs end at y 256)
struct CellPos { int x, y, w; };
static CellPos g_cells[3][pad::G_COUNT];
static void layout_cells() {
    int x = 0, y = CELL_Y0;
    for (int st = 1; st <= 2; ++st) {
        for (int g = 0; g < pad::G_COUNT; ++g) {
            int w = glyphs::width(st, g);
            if (x + w > ATLAS_W - 2) { x = 0; y += glyphs::CELL_H; }
            g_cells[st][g] = {x, y, w};
            x += w + 2;
        }
        x = 0; y += glyphs::CELL_H;
    }
}
typedef DWORD(__fastcall* Glyph_t)(DWORD font, void* edx, int ch);
static int g_atlas_step = 0; static DWORD g_ex_addr = 0, g_ex_code = 0;
static int ex_note(EXCEPTION_POINTERS* e) { g_ex_code = e->ExceptionRecord->ExceptionCode; g_ex_addr = (DWORD)(uintptr_t)e->ExceptionRecord->ExceptionAddress; return EXCEPTION_EXECUTE_HANDLER; }
static bool paint_atlas(DWORD font) {   // the pictures of both styles into the free rows, every mip level
    g_atlas_step = 0;
    __try {
        DWORD tex = RD32(font + FONT_TEX) & ~3u; if (!tex) return false;   // low bits are flags (like Texture +4)
        if (RD32(tex + 0x0C) != ATLAS_W || RD32(tex + 0x10) != ATLAS_H) { gp_log("pad: font %08X atlas is %ux%u, not 512x512: prompts stay text\n", font, RD32(tex + 0x0C), RD32(tex + 0x10)); return false; }
        IDirect3DTexture9* t = (IDirect3DTexture9*)(uintptr_t)(RD32(tex + 4) & ~3u);
        if (!t) return false;
        {   // a COM object of a loaded module (d3d9 or a proxy), not garbage, before any call
            DWORD vt = RD32((DWORD)(uintptr_t)t); DWORD fn = vt ? RD32(vt + 0x44) : 0; HMODULE m = nullptr;
            if (!fn || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)fn, &m)) {
                gp_log("pad: font %08X texture %08X: +4 is not a Direct3D texture: prompts stay text\n", font, tex); return false;
            }
        }
        g_atlas_step = 1;
        D3DSURFACE_DESC d; if (FAILED(t->GetLevelDesc(0, &d)) || d.Width != ATLAS_W || d.Height != ATLAS_H || d.Format != D3DFMT_A8R8G8B8) {
            gp_log("pad: font %08X atlas surface is not 512x512 A8R8G8B8: prompts stay text\n", font); return false;
        }
        static uint32_t img[4 * 32 * ATLAS_W];   // rows CELL_Y0 .. CELL_Y0 + 127 (4 rows of 32: both styles)
        g_atlas_step = 2;
        const int rows = 4 * glyphs::CELL_H; memset(img, 0, sizeof img);
        for (int st = 1; st <= 2; ++st)
            for (int g = 0; g < pad::G_COUNT; ++g) {
                CellPos c = g_cells[st][g];
                if (c.y + glyphs::CELL_H > CELL_Y0 + rows) return false;
                glyphs::paint(st, g, img + (c.y - CELL_Y0) * ATLAS_W + c.x, ATLAS_W);
            }
        g_atlas_step = 3;
        DWORD levels = t->GetLevelCount();
        g_atlas_step = 4;
        for (DWORD lv = 0; lv < levels && lv < 10; ++lv) {
            D3DLOCKED_RECT lr; int s = 1 << lv;
            RECT r = {0, CELL_Y0 / s, ATLAS_W / s, (CELL_Y0 + rows) / s};
            if (r.bottom <= r.top) break;
            if (FAILED(t->LockRect(lv, &lr, &r, 0))) { gp_log("pad: font %08X atlas level %lu cannot be locked (pool?): prompts stay text\n", font, lv); return lv > 0; }
            for (int y = 0; y < r.bottom - r.top; ++y) {
                uint32_t* dst = (uint32_t*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < ATLAS_W / s; ++x) {
                    if (s == 1) { dst[x] = img[y * ATLAS_W + x]; continue; }
                    unsigned a = 0, rr = 0, gg = 0, bb = 0;
                    for (int yy = 0; yy < s; ++yy) for (int xx = 0; xx < s; ++xx) {
                        uint32_t p = img[(y * s + yy) * ATLAS_W + x * s + xx]; unsigned pa = p >> 24;
                        a += pa; rr += ((p >> 16) & 255) * pa; gg += ((p >> 8) & 255) * pa; bb += (p & 255) * pa;
                    }
                    unsigned n = s * s;
                    dst[x] = a ? ((a / n) << 24) | ((rr / a) << 16) | ((gg / a) << 8) | (bb / a) : 0;
                }
            }
            t->UnlockRect(lv);
        }
        gp_log("pad: button pictures drawn into font %08X's atlas (rows %d..%d, %lu level(s))\n", font, CELL_Y0, CELL_Y0 + rows - 1, levels);
        return true;
    } __except (ex_note(GetExceptionInformation())) {
        DWORD base = (DWORD)(uintptr_t)GetModuleHandleA("TODGameplay.dll");
        gp_log("pad: font atlas write faulted at step %d (code %08X at %08X = DLL+%X)\n", g_atlas_step, g_ex_code, g_ex_addr, g_ex_addr - base);
        __try {
            char line[400]; int L = sprintf_s(line, "pad: font %08X words:", font);
            for (int i = 0; i < 0x64 / 4 && L < 380; ++i) L += sprintf_s(line + L, sizeof line - L, " %08X", RD32(font + 4 * i));
            gp_log("%s\n", line);
            DWORD tx = RD32(font + FONT_TEX) & ~3u;
            L = sprintf_s(line, "pad: font texture record %08X:", tx);
            for (int i = 0; i < 12; ++i) L += sprintf_s(line + L, sizeof line - L, " %08X", RD32(tx + 4 * i));
            gp_log("%s\n", line);
            for (int i = 0; i < 3; ++i) {
                DWORD p = RD32(tx + 4 * i) & ~3u; if (p < 0x10000) continue;
                L = sprintf_s(line, "pad:   word %d -> %08X:", i, p);
                __try { for (int k = 0; k < 12; ++k) L += sprintf_s(line + L, sizeof line - L, " %08X", RD32(p + 4 * k)); } __except (EXCEPTION_EXECUTE_HANDLER) { L += sprintf_s(line + L, sizeof line - L, " (unreadable)"); }
                gp_log("%s\n", line);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }
}
static FontRec* font_rec(DWORD font) {
    for (int i = 0; i < g_nfonts; ++i) if (g_fonts[i].font == font) return &g_fonts[i];
    if (g_nfonts >= 6) return nullptr;
    FontRec& f = g_fonts[g_nfonts++]; memset(&f, 0, sizeof f); f.font = font; f.style = -1;
    __try {
        for (int g = 0; g < pad::G_COUNT; ++g) {
            DWORD gl = ((Glyph_t)(uintptr_t)P_GLYPH)(font, nullptr, pad::glyph_code(g));
            if (!gl || *(volatile WORD*)(uintptr_t)gl != pad::glyph_code(g)) { gp_log("pad: font %08X has no glyph %04X: prompts stay text with it\n", font, pad::glyph_code(g)); return &f; }
            f.glyph[g] = gl; memcpy(f.orig[g], (void*)(uintptr_t)gl, 0x20);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return &f; }
    f.ok = paint_atlas(font);
    return &f;
}
static void apply_style(FontRec& f, int style) {   // glyph records -> the style's cells, or the originals
    if (!f.ok || f.style == style) return;
    __try {
        for (int g = 0; g < pad::G_COUNT; ++g) {
            BYTE* gl = (BYTE*)(uintptr_t)f.glyph[g];
            if (style == pad::STYLE_KEYBOARD) { memcpy(gl, f.orig[g], 0x20); continue; }
            CellPos c = g_cells[style == pad::STYLE_PS ? 2 : 1][g];
            *(float*)(gl + 0x04) = 0.0f;                         // x offset
            *(float*)(gl + 0x0C) = (float)c.w;                   // advance (px)
            *(float*)(gl + 0x10) = (float)c.x / ATLAS_W;         // U
            *(float*)(gl + 0x14) = (float)c.y / ATLAS_H;         // V
            *(float*)(gl + 0x18) = (float)c.w / ATLAS_W;         // U width
            *(DWORD*)(gl + 0x1C) = 1;                            // keeps its own colours (font flag 8)
        }
        f.style = style;
    } __except (EXCEPTION_EXECUTE_HANDLER) { f.ok = false; }
}
// literal key names of the PC texts (five languages) and what they stand for
static const struct { const char* text; int lit; } LIT_TEXTS[] = {
    {"[Enter]", padp::LIT_ENTER}, {"[Entr\xE9" "e]", padp::LIT_ENTER}, {"[Retorno]", padp::LIT_ENTER}, {"[Return]", padp::LIT_ENTER},
    {"[Esc]", padp::LIT_ESC}, {"[\xC9" "chap]", padp::LIT_ESC},
    {"[A]", padp::LIT_DELETE}, {"[Q]", padp::LIT_DELETE},
    {"[Space]", padp::LIT_SPACE}, {"[Leertaste]", padp::LIT_SPACE}, {"[Espace]", padp::LIT_SPACE}, {"[Espacio]", padp::LIT_SPACE}, {"[Spazio]", padp::LIT_SPACE}};
static const int N_LIT = (int)(sizeof LIT_TEXTS / sizeof LIT_TEXTS[0]);
static int glyph_of_literal(int lit, const padp::Hint* h) {   // the action's pad picture (follows [pad_bind] / map_*)
    int g = glyph_of_action(padp::literal_action(lit, h));
    return g >= 0 ? g : padp::literal_fallback_button(lit);
}
// Which text a TextBox shows: its slot (+B4h) and ordinal, the way the render 0x8FE280 picks it (REF PD_TB_TEXTID /
// PD_TB_SLOTIDX): no literal text (+B8h), not list mode (flags +D8h bit 12); bit 9 = the slot's current_index (+54h),
// else the 11-bit textslot_index (bits 16..26, -1 = 0). False when it is not a slot text.
struct SlotKind { DWORD slot; int kind; };
static SlotKind g_slotkinds[16]; static int g_nslotkinds = 0;
static int slot_kind(DWORD slot) {
    for (int i = 0; i < g_nslotkinds; ++i) if (g_slotkinds[i].slot == slot) return g_slotkinds[i].kind;
    int k = padp::slot_from_name(node_name(slot));
    if (g_nslotkinds < 16) { g_slotkinds[g_nslotkinds].slot = slot; g_slotkinds[g_nslotkinds++].kind = k; }
    return k;
}
static bool text_of_box(DWORD tb, int* slot, int* ordinal) {
    __try {
        if (RD32(tb + 0xB8)) return false;
        DWORD s = RD32(tb + 0xB4); if (!s) return false;
        DWORD fl = RD32(tb + TB_FLAGS);
        if (fl & 0x1000) return false;
        int idx = (fl & 0x200) ? (int)RD32(s + 0x54) : ((int)(fl << 5)) >> 21;
        if (idx == -1) idx = 0;
        *slot = slot_kind(s); *ordinal = idx;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The HUD use prompt's context (padp::use_prompt_action; REF PD_USE_VEH / PD_USE_ON / PD_USE_TB, checked here so a
// mismatch only drops this refinement): the TextBox's parent (+40h) is Mission_HUD, whose script data (+18h) holds the
// box at +54h and the vehicle / use flags at +38h / +24h. nullptr = not that box or neither flag.
// The HUD use prompt (Mission_HUD PC_Use_TB, Misc_PC #86 "[%1] use") serves both the usable things (CROSS: use)
// and the vehicles / mounted gun (TRIANGLE: use_vehicle). The two show functions are noted at their flag writes
// (PD_USE_VEH / PD_USE_ON, edi = the script data); the data's +54h is the box, +38h the vehicle flag, +24h the use flag.
static volatile DWORD g_use_data = 0;
static void __stdcall UseNoteC(DWORD data) { g_use_data = data; }
__declspec(naked) static void UseVehStub() {   // at 0x60D81B: mov eax,[esp+18h]; mov esi,[esp+14h] (8 bytes)
    __asm pushad
    __asm pushfd
    __asm push edi
    __asm call UseNoteC
    __asm popfd
    __asm popad
    __asm mov eax, dword ptr [esp + 0x18]
    __asm mov esi, dword ptr [esp + 0x14]
    __asm jmp dword ptr [R_USEVEH_BACK]
}
__declspec(naked) static void UseOnStub() {    // at 0x60D646: mov eax,[edi+34h]; mov esi,[esp+14h] (7 bytes)
    __asm pushad
    __asm pushfd
    __asm push edi
    __asm call UseNoteC
    __asm popfd
    __asm popad
    __asm mov eax, dword ptr [edi + 0x34]
    __asm mov esi, dword ptr [esp + 0x14]
    __asm jmp dword ptr [R_USEON_BACK]
}
static bool g_use_hooked = false;
static const char* use_prompt_context(DWORD tb) {
    if (!g_use_hooked) return nullptr;
    static DWORD logged[8]; static int nlogged = 0;
    __try {
        DWORD d = g_use_data;
        int veh = -1, use = -1; const char* act = nullptr;
        if (d && RD32(d + 0x54) == tb) { veh = (int)RD32(d + 0x38); use = (int)RD32(d + 0x24); act = padp::use_prompt_action(veh, use); }
        DWORD key = tb ^ (DWORD)((veh + 1) * 4 + (use + 1));
        bool known = false; for (int i = 0; i < nlogged; ++i) known |= logged[i] == key;
        if (!known && nlogged < 8) {
            logged[nlogged++] = key;
            if (veh >= 0) gp_log("prompts: use prompt %08X (Mission_HUD data %08X): vehicle %d use %d -> %s\n", tb, d, veh, use, act ? act : "hint");
            else gp_log("prompts: use prompt %08X is not the Mission_HUD box (data %08X): hint\n", tb, d);
        }
        return act;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static const char* ps_name(int g) {   // log names in PlayStation style
    static const char* const N[pad::G_COUNT] = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1", "L2", "R2", "L3", "R3",
                                                "SHARE", "OPTIONS", "UP", "DOWN", "LEFT", "RIGHT", "LSTICK", "RSTICK"};
    return g >= 0 && g < pad::G_COUNT ? N[g] : "?";
}
static uint16_t g_pstr[10][4];
static DWORD g_style_shown_frame = 0; static int g_glyph_draws = 0;
static void __fastcall ExpandWrap(void* self, void* edx, const uint16_t* text, uint16_t* out, int cap) {
    typedef void(__fastcall* F)(void*, void*, const uint16_t*, uint16_t*, int);
    int style = g_style;
    if (style == pad::STYLE_KEYBOARD || !out || cap <= 0) { ((F)(uintptr_t)R_EXPAND)(self, edx, text, out, cap); return; }
    DWORD tb = (DWORD)(uintptr_t)self; DWORD params = 0; DWORD saved[10]; int swapped = 0;
    int slot = padp::SLOT_OTHER, ordinal = -1;
    const padp::Hint* hint = text_of_box(tb, &slot, &ordinal) ? padp::find_hint(slot, ordinal) : nullptr;
    padp::Hint use_hint;
    if (hint && padp::is_use_prompt(slot, ordinal)) {   // the use prompt: vehicle (TRIANGLE) or other usable (CROSS)
        const char* act = use_prompt_context(tb);
        if (act) { use_hint = *hint; use_hint.param[0] = act; hint = &use_hint; }
    }
    char why[10][96]; int whyset = 0;
    __try {
        params = RD32(tb + TB_PARAMS);
        if (params) for (int k = 0; k < g_nparams; ++k) {
            if (g_params[k].tb != tb || g_params[k].ord < 0 || g_params[k].i < 0 || g_params[k].i > 9) continue;
            int i = g_params[k].i;
            const char* want = hint && i < padp::MAX_PARAMS ? hint->param[i] : nullptr;
            int g = glyph_for_ordinal(g_params[k].ord, want, why[i], sizeof why[i]);
            if (g < 0) continue;
            whyset |= 1 << i;
            if (!(swapped & (1 << i))) saved[i] = RD32(params + 4 * i);
            g_pstr[i][0] = pad::glyph_code(g); g_pstr[i][1] = 0;
            *(volatile DWORD*)(uintptr_t)(params + 4 * i) = (DWORD)(uintptr_t)g_pstr[i];
            swapped |= 1 << i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ((F)(uintptr_t)R_EXPAND)(self, edx, text, out, cap);
    __try { for (int i = 0; i < 10; ++i) if (swapped & (1 << i)) *(volatile DWORD*)(uintptr_t)(params + 4 * i) = saved[i]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    pad::Literal lits[N_LIT];
    int lit_g[padp::LIT_COUNT]; for (int l = 0; l < padp::LIT_COUNT; ++l) lit_g[l] = glyph_of_literal(l, hint);
    for (int i = 0; i < N_LIT; ++i) { lits[i].text = LIT_TEXTS[i].text; lits[i].glyph = lit_g[LIT_TEXTS[i].lit]; }
    size_t n = pad::post_process(out, (size_t)cap, lits, N_LIT);
    bool any = false;
    for (size_t i = 0; i < n; ++i) if (pad::glyph_of_code(out[i]) >= 0) { any = true; break; }
    if (!any) return;
    __try {
        DWORD fa = RD32(tb + TB_FONT); DWORD font = fa ? (RD32(fa + FA_FONT) & ~3u) : 0;
        if (font) { FontRec* f = font_rec(font); if (f) apply_style(*f, style); }
        *(volatile DWORD*)(uintptr_t)(tb + TB_FLAGS) |= 0x2000;   // use_glyph_ignore_color: the pictures keep their colours
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static uint32_t seen[96]; static int nseen = 0;
    uint32_t hsh = 2166136261u; for (size_t i = 0; i < n; ++i) hsh = (hsh ^ out[i]) * 16777619u; hsh ^= (uint32_t)style;
    bool known = false; for (int i = 0; i < nseen; ++i) if (seen[i] == hsh) known = true;
    if (!known && nseen < 96) {
        seen[nseen++] = hsh; ++g_glyph_draws; g_style_shown_frame = g_frame;
        char a[260]; int L = 0;
        for (size_t i = 0; i < n && L < 250; ++i) {
            uint16_t c = out[i]; int g = pad::glyph_of_code(c);
            L += g >= 0 ? sprintf_s(a + L, sizeof a - L, "<%s>", style == pad::STYLE_PS ? ps_name(g) : pad::btn_name(g))
                        : sprintf_s(a + L, sizeof a - L, "%c", c >= 32 && c < 127 ? (char)c : (c == 10 ? '|' : '?'));
        }
        char id[40]; if (ordinal >= 0) sprintf_s(id, "%s#%d%s", padp::slot_name(slot), ordinal, hint ? "" : " (no hint)"); else strcpy_s(id, "not a slot text");
        gp_log("prompts: %s style [%s]: \"%s\"\n", style == pad::STYLE_PS ? "PlayStation" : "Xbox", id, a);
        for (int i = 0; i < 10; ++i) if (whyset & (1 << i)) gp_log("prompts:   [%%%d] -> %s\n", i, why[i]);
        if (hint && hint->esc) gp_log("prompts:   [Esc] here = %s\n", hint->esc);
    }
}

// ================================================================================================ rumble
static float g_last_m0 = -1, g_last_m1 = -1;
static void __fastcall VibrateWrap(void* gp, void* edx, int motor, float force) {   // Gamepad::SetControllerVibration
    typedef void(__fastcall* F)(void*, void*, int, float);
    if (g_pc.rumble_log) gp_log("rumble: game set_vibration(motor %d, %.3f)\n", motor, force);
    ((F)(uintptr_t)R_VIBRATE)(gp, edx, motor, force);
}
static bool vibration_setting() {   // Setting_Allow_vibration (settings member 3), true when unreadable
    typedef void(__fastcall* Get_t)(DWORD, void*, int, void*);
    __try {
        DWORD cd = gp_cache_data(); DWORD st = cd ? RD32(cd + C_SETTINGS) : 0;
        if (!st) return true;
        int v = 1; ((Get_t)(uintptr_t)P_MEMBER_GET)(st, nullptr, 3, &v);
        return v != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
// weapon_effects spends a round (FUN_00834760, script context: [0xA3B75C] = the weapon_effects node, data +94h pOwner)
static volatile LONG g_shots = 0;
static void __cdecl ShotC() {
    __try {
        DWORD cur = RD32(A_CUR_NODE); DWORD d = cur ? RD32(cur + 0x18) : 0;
        DWORD owner = d ? RD32(d + 0x94) : 0, ram = ram_root();
        static int logged = 0;
        if (g_pc.log_state && logged < 8) { ++logged; gp_log("rumble: round spent by weapon_effects %08X, owner %08X (Ram %08X)\n", cur, owner, ram); }
        if (owner && owner == ram) InterlockedIncrement(&g_shots);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) static void ShotStub() {   // at 0x834760: sub esp,10h; mov ecx,[0xA3B58C] (9 bytes)
    __asm pushad
    __asm pushfd
    __asm call ShotC
    __asm popfd
    __asm popad
    __asm sub esp, 0x10
    __asm mov ecx, dword ptr ds:[0xA3B58C]
    __asm jmp dword ptr [R_SHOT_BACK]
}
static int g_prev_move = -1;
static DWORD g_cam_move = 0, g_cam_root = 0;
static void rumble_events(float dt) {
    if (!g_pc.rumble_events) return;
    LONG s = InterlockedExchange(&g_shots, 0);
    if (s > 0) { g_rum.pulse(g_pc.ev_shot * 0.6f, g_pc.ev_shot, g_pc.ev_shot_s); if (g_pc.rumble_log) gp_log("rumble: event shot x%ld\n", s); }
    __try {   // landing a shoot-dodge: MoveState leaves a dive state (0x1D..0x20, 0x24)
        DWORD r = ram_root(); DWORD rd = r ? RD32(r + 0x18) : 0; DWORD mc = rd ? RD32(rd + 0x48) : 0; DWORD md = mc ? RD32(mc + 0x18) : 0;
        int ms = md ? (int)RD32(md + 0x6C) : -1;
        auto dive = [](int m) { return (m >= 0x1B && m <= 0x20) || m == 0x24; };   // shoot-dodges (in game: 1B..1E, then 34h on landing)
        if (g_prev_move >= 0 && dive(g_prev_move) && ms >= 0 && !dive(ms)) {
            g_rum.pulse(g_pc.ev_land, g_pc.ev_land * 0.5f, g_pc.ev_land_s);
            if (g_pc.rumble_log) gp_log("rumble: event shoot-dodge landing (MoveState %02X -> %02X)\n", g_prev_move, ms);
        }
        if (g_pc.log_state && ms != g_prev_move) { static int n = 0; if (++n < 80) gp_log("pad: Ram MoveState %02X -> %02X\n", g_prev_move, ms); }
        g_prev_move = ms;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_prev_move = -1; }
    __try {   // camera shake (explosions near the player): GameCamera's MoveCtrl +30h offset, +3Ch time left
        DWORD cd = gp_cache_data(); DWORD cam = cd ? RD32(cd + C_GAMECAM) : 0;
        if (cam != g_cam_root) { g_cam_root = cam; g_cam_move = cam ? child_named(cam, "MoveCtrl") : 0; }
        DWORD md = g_cam_move ? RD32(g_cam_move + 0x18) : 0;
        if (md) {
            float t = RDF(md + 0x3C); float x = RDF(md + 0x30), y = RDF(md + 0x34), z = RDF(md + 0x38);
            float m = sqrtf(x * x + y * y + z * z);
            static float last_logged = 0;
            if ((t > 0.0f || RD32(md + 0x44)) && m > 0.005f && g_pc.ev_shake > 0) {
                float k = m * 6.0f * g_pc.ev_shake; if (k > 1) k = 1;
                g_rum.pulse(k, k * 0.7f, 0.12f);
                if (g_pc.rumble_log && fabsf(k - last_logged) > 0.15f) { last_logged = k; gp_log("rumble: event camera shake |%.3f| -> %.2f\n", m, k); }
            } else last_logged = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cam_root = g_cam_move = 0; }
    (void)dt;
}
static void rumble_out(float dt) {
    __try {
        if (g_pad_obj) { g_rum.game[0] = RDF(g_pad_obj + GP_MOTOR0); g_rum.game[1] = RDF(g_pad_obj + GP_MOTOR1); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (int i = 0; i < 2; ++i) if (!(g_rum.game[i] >= 0.0f && g_rum.game[i] <= 1.0f)) g_rum.game[i] = g_rum.game[i] > 1.0f ? 1.0f : 0.0f;
    rumble_events(dt);
    g_rum.step(dt);
    static bool setting = true; static DWORD next_check = 0;
    if (g_frame >= next_check) { bool s = vibration_setting(); if (s != setting) gp_log("rumble: VIBRATION setting %s\n", s ? "ON" : "OFF"); setting = s; next_check = g_frame + 30; }
    float big, little;
    g_rum.out(g_pc.rumble_strength, g_pc.rumble && setting, big, little);
    if (fabsf(big - g_last_m0) > 0.02f || fabsf(little - g_last_m1) > 0.02f || ((big == 0) != (g_last_m0 == 0)) || ((little == 0) != (g_last_m1 == 0))) {
        if (g_pc.rumble_log) gp_log("rumble: motors big %.2f little %.2f -> %s\n", big, little, g_src == 1 ? "XInput" : g_src == 2 ? "PlayStation pad" : g_src == 3 ? "scripted pad (log only)" : "no pad");
        g_last_m0 = big; g_last_m1 = little;
        if (g_src == 1 && g_xset && g_xslot >= 0) { XVIB v = {(WORD)(big * 65535.0f), (WORD)(little * 65535.0f)}; g_xset(g_xslot, &v); }
        if (g_src == 2) { g_hout[0] = (uint8_t)(big * 255.0f); g_hout[1] = (uint8_t)(little * 255.0f); InterlockedExchange(&g_hout_dirty, 1); }
    }
}

// ================================================================================================ per frame (main loop, after the input poll)
static float g_mprev[6]; static bool g_mprev_ok = false;
void pad_post_input(const BYTE* kb_edges, const DWORD* mouse, double now, float dt) {
    if (!g_installed) return;
    ++g_frame; g_now = now;
    attach_pad();
    poll_devices();
    // keyboard / mouse activity: a key or mouse button went down, or the mouse moved
    bool kbm = false;
    __try {
        if (kb_edges && kb_edges[0x200]) for (int i = 1; i < 256 && !kbm; ++i) if (kb_edges[i] & 1) kbm = true;
        if (mouse && (mouse[0x48 / 4] & 0xFF)) {   // the game reads the mouse (+48h ShouldBeProcessed): window active
            for (int b = 0; b < 8 && !kbm; ++b) if (mouse[b] & 2) kbm = true;
            float m[6] = {(float)(int)mouse[0x2C / 4], (float)(int)mouse[0x30 / 4], (float)(int)mouse[0x34 / 4], (float)(int)mouse[0x38 / 4], (float)(int)mouse[0x3C / 4], (float)(int)mouse[0x40 / 4]};
            if (g_mprev_ok) {
                float d = fabsf(m[0] - g_mprev[0]) + fabsf(m[1] - g_mprev[1]) + fabsf(m[4] - g_mprev[4]) + fabsf(m[5] - g_mprev[5]);
                if (d > g_pc.kb_mouse_threshold) kbm = true;
            }
            memcpy(g_mprev, m, sizeof m); g_mprev_ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_act.step(g_state, kbm, now)) {
        gp_log("pad: device used last: %s\n", g_act.active ? (g_act.last_kind == pad::KIND_PS ? "PlayStation pad (game's gamepad procedure)" : "Xbox pad (game's gamepad procedure)") : "keyboard / mouse");
    }
    InterlockedExchange(&g_pad_mode, g_act.active);
    int kind = g_pc.pad_style ? g_pc.pad_style : (g_state.connected ? g_state.kind : g_act.last_kind);
    int st = pad::resolve_style(g_pc.prompts, g_act.active, kind);
    if (st != g_style) { gp_log("prompts: style %s\n", st == pad::STYLE_KEYBOARD ? "keyboard" : st == pad::STYLE_PS ? "PlayStation" : "Xbox"); g_style = st; }
    if (g_style != g_style_applied) { for (int i = 0; i < g_nfonts; ++i) apply_style(g_fonts[i], g_style); g_style_applied = g_style; }
    rumble_out(dt);
    if (g_pc.log_state && g_frame % 60 == 0 && g_state.connected)
        gp_log("pad: state buttons %05X L %.2f %.2f R %.2f %.2f T %.2f %.2f -> lX %d lY %d lZ %d lRz %d mode %d\n", g_state.buttons, g_state.lx, g_state.ly,
               g_state.rx, g_state.ry, g_state.lt, g_state.rt, g_di.lX, g_di.lY, g_di.lZ, g_di.lRz, (int)g_pad_mode);
}
WORD pad_xinput_buttons() {   // the pad as XInput wButtons (for [bullettime] pad_button)
    if (!g_installed || !g_state.connected) return 0;
    static const struct { int b; WORD bit; } T[] = {{pad::B_UP, 1}, {pad::B_DOWN, 2}, {pad::B_LEFT, 4}, {pad::B_RIGHT, 8}, {pad::B_START, 0x10},
        {pad::B_BACK, 0x20}, {pad::B_LS, 0x40}, {pad::B_RS, 0x80}, {pad::B_LB, 0x100}, {pad::B_RB, 0x200}, {pad::B_A, 0x1000},
        {pad::B_B, 0x2000}, {pad::B_X, 0x4000}, {pad::B_Y, 0x8000}};
    WORD w = 0;
    for (auto& t : T) if (g_state.down(t.b)) w |= t.bit;
    return w;
}

// ================================================================================================ stubs
__declspec(naked) static void PadPresentStub() {   // thiscall(script globals, int* out), ret 4 (replaces 0x4853D0)
    __asm mov eax, dword ptr [esp + 4]
    __asm mov ecx, dword ptr [g_pad_mode]
    __asm mov dword ptr [eax], ecx
    __asm ret 4
}

// ================================================================================================ config / install
static const char* PAD_REFS[] = {"PD_PRESENT", "PD_COUNT", "PD_POLL", "PD_AXES", "PD_AXES2", "PD_FF", "PD_MOTORS", "PD_CTOR",
    "PD_NODE_NAME", "PD_NODE_KIDS", "PD_TABLE", "PD_CTL_TYPE", "PD_START", "PD_KBSLOT", "PD_TXT_SLOT", "PD_EXPAND", "PD_TB_FONT",
    "PD_TB_GLYPHCOL", "PD_TB_TEXTID", "PD_TB_SLOTIDX", "PD_GLYPH", "PD_GLYPH_RET", "PD_KEYNAME", "PD_MEMBER_GET", "PD_PARAMS", "PD_MOVESTATE", "PD_SHAKE", "PD_VIB_SETTING", "SV_CAM_RELEASE", "FIX_WPN_OWNERRD"};
void pad_load_config() {
    const gp::Ini& ini = gp_ini();
    PadCfg& c = g_pc;
    c.enabled = ini.geti("pad", "enabled", 0) != 0;
    if (!c.enabled) return;
    c.xinput = ini.geti("pad", "xinput", 1) != 0;
    c.hid = ini.geti("pad", "playstation_hid", 1) != 0;
    if (const char* v = ini.get("pad", "prompts")) { int m = pad::mode_from_name(v); if (m < 0) gp_log("ini [pad] prompts = %s: unknown (auto keyboard xbox playstation), auto\n", v); else c.prompts = m; }
    if (const char* v = ini.get("pad", "glyphs")) { int m = pad::mode_from_name(v); c.pad_style = m == pad::MODE_XBOX ? pad::KIND_XBOX : m == pad::MODE_PS ? pad::KIND_PS : 0; }
    c.scripts = ini.geti("pad", "tell_scripts", 0) != 0;
    c.pc_menus = ini.geti("pad", "start_opens_menu", 1) != 0;
    c.shape.dz_left = ini.getf("pad", "deadzone_left", 0.20f);
    c.shape.dz_right = ini.getf("pad", "deadzone_right", 0.15f);
    c.shape.trigger = ini.getf("pad", "trigger_threshold", 0.25f);
    c.shape.look_exp = ini.getf("pad", "look_curve", 2.0f);
    c.shape.look_sens_x = ini.getf("pad", "look_sensitivity_x", 1.0f);
    c.shape.look_sens_y = ini.getf("pad", "look_sensitivity_y", 1.0f);
    c.shape.invert_x = ini.geti("pad", "invert_x", 0) != 0;
    c.shape.invert_y = ini.geti("pad", "invert_y", 0) != 0;
    c.shape.compensate = ini.geti("pad", "compensate_game_deadzone", 1) != 0;
    float* dz[2] = {&c.shape.dz_left, &c.shape.dz_right};
    for (float* f : dz) { if (*f < 0) *f = 0; if (*f > 0.9f) *f = 0.9f; }
    c.rumble = ini.geti("pad", "rumble", 1) != 0;
    c.rumble_strength = ini.getf("pad", "rumble_strength", 1.0f);
    if (c.rumble_strength < 0) c.rumble_strength = 0; if (c.rumble_strength > 2) c.rumble_strength = 2;
    c.rumble_events = ini.geti("pad", "rumble_events", 1) != 0;
    c.rumble_log = ini.geti("pad", "rumble_log", 1) != 0;
    c.ev_shot = ini.getf("pad", "rumble_shot", 0.25f);
    c.ev_land = ini.getf("pad", "rumble_landing", 0.55f);
    c.ev_shake = ini.getf("pad", "rumble_shake", 1.0f);
    c.kb_mouse_threshold = ini.getf("pad", "mouse_switch_counts", 3.0f);
    c.log_state = ini.geti("pad", "log_state", 0) != 0;
    c.map = pad::default_map();
    c.nbinds = 0;
    for (int i = 0; i < ini.n; ++i) {
        const gp::IniEntry& e = ini.e[i];
        if (!_stricmp(e.section, "pad") && !_strnicmp(e.key, "map_", 4)) {
            if (!pad::map_set(c.map, e.key + 4, e.value)) gp_log("ini [pad] %s = %s: unknown game button or pad button, ignored\n", e.key, e.value);
        } else if (!_stricmp(e.section, "pad_bind") && c.nbinds < 24) {
            int b = !_stricmp(e.value, "none") ? -2 : pad::btn_from_name(e.value);
            if (b != -2 && (b < 0 || b >= pad::B_COUNT)) { gp_log("ini [pad_bind] %s = %s: unknown pad button, ignored\n", e.key, e.value); continue; }
            strncpy_s(c.binds[c.nbinds].control, e.key, _TRUNCATE); c.binds[c.nbinds].btn = b; ++c.nbinds;
        }
    }
    if (const char* v = ini.get("debug", "pad_script")) strncpy_s(c.script, v, _TRUNCATE);
}
bool pad_enabled() { return g_pc.enabled; }
bool pad_wants_post() { return g_pc.enabled; }
static void read_script() {
    if (!g_pc.script[0]) return;
    char p[MAX_PATH * 2]; GetModuleFileNameA(nullptr, p, MAX_PATH); char* s = strrchr(p, '\\'); if (s) *s = 0;
    strcat_s(p, "\\"); strcat_s(p, g_pc.script);
    FILE* f = _fsopen(p, "rb", _SH_DENYNO);
    if (!f) { gp_log("pad: [debug] pad_script %s not found\n", g_pc.script); return; }
    static char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    g_script = g_ps.parse(text) || g_ps.n > 0;
    gp_log("pad: [debug] pad script %s: %d commands, %d errors (TEST COPY ONLY)\n", g_pc.script, g_ps.n, g_ps.errors);
}
int pad_install(bool post_ok) {
    if (!g_pc.enabled) return 0;
    if (!post_ok) { gp_log("pad: needs the per-frame hook (POST_INPUT): OFF\n"); g_pc.enabled = false; return 0; }
    for (const char* r : PAD_REFS) if (!gp_ref_ok(r)) { gp_log("pad: reference %s differs: OFF\n", r); g_pc.enabled = false; return 0; }
    const char* need[] = {"PD_SEL_PROC", "PD_SEL_LOOK", "PD_CTLSTART"};
    for (const char* s : need) if (!gp_site_ok(s)) { gp_log("pad: site %s differs: OFF\n", s); g_pc.enabled = false; return 0; }
    int n = 0;
    n += gp_patch_call("PD_SEL_PROC", (void*)&PadPresentStub);
    n += gp_patch_call("PD_SEL_LOOK", (void*)&PadPresentStub);
    if (g_pc.scripts) n += gp_patch_call("PD_SEL_CMD", (void*)&PadPresentStub);
    DWORD w = (DWORD)(uintptr_t)&CtlStartWrap; n += gp_patch_bytes("PD_CTLSTART", &w, 4);
    if (g_pc.prompts != pad::MODE_KEYBOARD) {
        if (gp_site_ok("PD_TXTPARAM") && gp_site_ok("PD_EXPAND1") && gp_site_ok("PD_EXPAND2") && gp_site_ok("PD_EXPAND3")) {
            n += gp_patch_call("PD_TXTPARAM", (void*)&TxtParamWrap);
            n += gp_patch_call("PD_EXPAND1", (void*)&ExpandWrap); n += gp_patch_call("PD_EXPAND2", (void*)&ExpandWrap); n += gp_patch_call("PD_EXPAND3", (void*)&ExpandWrap);
            layout_cells();
            if (gp_site_ok("PD_USE_VEH") && gp_site_ok("PD_USE_ON") && gp_ref_ok("PD_USE_TB")) {
                int u = gp_patch_call("PD_USE_VEH", (void*)&UseVehStub); u += gp_patch_call("PD_USE_ON", (void*)&UseOnStub);
                n += u; g_use_hooked = u == 2;
            } else gp_log("pad: use prompt sites differ: the use prompt keeps its hint\n");
        } else gp_log("pad: text sites differ: prompts stay keyboard\n");
    }
    if (g_pc.rumble_log) n += gp_patch_call("PD_VIBRATE", (void*)&VibrateWrap);
    if (g_pc.rumble_events) n += gp_patch_call("PD_SHOT", (void*)&ShotStub);
    if (g_pc.hid) {
        InitializeCriticalSection(&g_hcs);
        g_hrun = 1; g_hthread = CreateThread(nullptr, 0, hid_thread, nullptr, 0, nullptr);
    }
    read_script();
    g_installed = true;
    gp_log("pad: ON - XInput %d, PlayStation HID %d, prompts %s, dead zones %.2f / %.2f, look curve %.1f, rumble %d x%.2f (events %d), %d binding override(s), %d site(s)\n",
           g_pc.xinput, g_pc.hid, g_pc.prompts == pad::MODE_AUTO ? "auto" : g_pc.prompts == pad::MODE_KEYBOARD ? "keyboard" : g_pc.prompts == pad::MODE_XBOX ? "xbox" : "playstation",
           g_pc.shape.dz_left, g_pc.shape.dz_right, g_pc.shape.look_exp, g_pc.rumble, g_pc.rumble_strength, g_pc.rumble_events, g_pc.nbinds, n);
    char m[256]; int L = sprintf_s(m, "pad: game buttons <- pad:");
    for (int v = 0; v < pad::V_COUNT; ++v) L += sprintf_s(m + L, sizeof m - L, " %s<-%s", pad::virt_name(v), g_pc.map.src[v] >= 0 ? pad::btn_name(g_pc.map.src[v]) : "-");
    gp_log("%s\n", m);
    return n;
}
void* pad_test_stub(const char* name) {
    if (!strcmp(name, "pad_present")) return (void*)&PadPresentStub;
    if (!strcmp(name, "pad_mode")) return (void*)&g_pad_mode;
    if (!strcmp(name, "pad_fake_device")) return (void*)&g_fake;
    if (!strcmp(name, "pad_ctlstart")) return (void*)&CtlStartWrap;
    if (!strcmp(name, "pad_expand")) return (void*)&ExpandWrap;
    if (!strcmp(name, "pad_shot")) return (void*)&ShotStub;
    if (!strcmp(name, "pad_use_veh")) return (void*)&UseVehStub;
    if (!strcmp(name, "pad_use_on")) return (void*)&UseOnStub;
    if (!strcmp(name, "pad_shots")) return (void*)&g_shots;
    return nullptr;
}
