// gp_menu.cpp - shared helper for the game's own option menus.
// A feature module registers a handler for one OptionValueType1 (the entry's option type:
// 0 gore, 3 difficulty, 6 subtitles, 8 vibration, ...) and gets two moments:
//
//   on_get(node, data)  - right after the game set the entry's CurrentIntegerValue (+4Ch) from the settings node
//                         (FUN_007FE420, called by the entry's initialize_local at block load and by command_reset_data
//                         whenever a menu page opens). The handler may change CurrentIntegerValue (which value is
//                         shown) and may append values to the entry (gp_menu_append_value).
//   on_set(type_value)  - right before the game applies a chosen value (FUN_006BA420(thread, int* {OptionValueType1,
//                         index}), the only writer of the settings from the menus; callers 0x6C4B80, 0x6C5448,
//                         0x6C5C14, 0x6D5E51). The handler may change index (what the game stores) - the entry keeps
//                         showing the chosen value.
//
// Sites (sites.h MN_*): MN_GET_INIT 0x7FF75C and MN_GET_RESET 0x7FE7C8 (the two calls of FUN_007FE420, retargeted to
// MenuGetWrap: the original, then the handler), MN_SET 0x6BA420 (the setter's entry, 8 bytes, jmp to MenuSetStub: the
// handler, then the two original instructions). Installed only when a module registered a handler; nothing else.
//
// Trigger_Root_Menu data (script class reg 0x7FF830; offsets from the code that reads them, REF rows MN_*):
//   +20h OptionValueType1, +1Ch/+24h/+2Ch/+34h/+3Ch ValueString1..5 (text ordinals into <lang>_Menu.txt; > 0 = offered),
//   +38h OptionValueType4 (never read by the code), +4Ch CurrentIntegerValue, +50h OriginalIntegerValue,
//   +58h StringValueList (list(integer) of the offered ordinals, built from ValueString1..5 by initialize_local
//   0x7FF5F0 through the list type [0xA3BB04] vtable +44h op 3 clear / op 9 append).
#include <windows.h>
#include <stdio.h>
#include "gp_glue.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))

static const DWORD A_CUR_NODE = 0xA3B75C, A_LIST_INT = 0xA3BB04;
static DWORD R_MN_GETTER = 0x7FE420;   // MN_GET_INIT / MN_GET_RESET original call target
static DWORD R_MN_SET = 0x6BA428;      // MN_SET: the setter after its first two instructions

struct Handler { int type; GpMenuGetFn get; GpMenuSetFn set; const char* who; };
static Handler g_h[16];
static int g_nh = 0;
static bool g_installed = false, g_tried = false;
static volatile LONG g_gets = 0, g_sets = 0;

bool gp_menu_register(int type, GpMenuGetFn get, GpMenuSetFn set, const char* who) {
    if (g_nh >= 16) return false;
    for (int i = 0; i < g_nh; ++i) if (g_h[i].type == type) { gp_log("menu: option type %d already has a handler (%s): %s refused\n", type, g_h[i].who, who); return false; }
    g_h[g_nh++] = Handler{type, get, set, who};
    return true;
}
bool gp_menu_installed() { return g_installed; }

int gp_menu_value_count(DWORD data) {
    __try {
        DWORD L = data ? RD32(data + 0x58) : 0; DWORD t = RD32(A_LIST_INT); DWORD k = t ? RD32(t + 0x24) : 0;
        if (!L || !k) return -1;
        return (int)(RD32(L + 8) / k);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int gp_menu_value_at(DWORD data, int i) {
    __try {
        DWORD L = data ? RD32(data + 0x58) : 0; DWORD t = RD32(A_LIST_INT); DWORD k = t ? RD32(t + 0x24) : 0;
        int n = (L && k) ? (int)(RD32(L + 8) / k) : 0;
        if (i < 0 || i >= n) return -1;
        DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
        return el ? (int)RD32(el + (DWORD)i * 4 * k) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
typedef void(__fastcall* ListOp_t)(DWORD type, void* edx, int op, void* args);   // thiscall, ret 8
// appends text ordinal `ordinal` as the next value of the entry: ValueString<n+1> = ordinal (so a rebuild of the list by
// initialize_local keeps it) and the list type's own append (op 9, as initialize_local does). No member-write notify:
// menu entries never appear in saves and a later load must not carry our value. Returns the new value count or -1.
int gp_menu_append_value(DWORD data, int ordinal) {
    int n = gp_menu_value_count(data);
    if (n < 0 || n >= 5 || ordinal <= 0) return -1;
    __try {
        DWORD vs = data + 0x1C + 8 * (DWORD)n;              // ValueString(n+1)
        if ((int)RD32(vs) > 0 && (int)RD32(vs) != ordinal) return -1;   // the slot is taken by another value
        WR32(vs, ordinal);
        DWORD t = RD32(A_LIST_INT); DWORD vt = t ? RD32(t) : 0; DWORD fn = vt ? RD32(vt + 0x44) : 0;
        if (!fn) return -1;
        struct { int value; DWORD list; } a = {ordinal, RD32(data + 0x58)};
        ((ListOp_t)(uintptr_t)fn)(t, nullptr, 9, &a);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return gp_menu_value_count(data);
}

// replaces the entry's whole StringValueList with `n` values (the list type's own op 3 clear, then op 9 appends, as
// initialize_local builds it). ValueString1..5 are NOT touched: a block reload rebuilds the vanilla list from them and the
// option type's on_get runs right after (MN_GET_INIT), so a handler that calls this from on_get keeps its list. The values
// may be any integers the handler understands (the [display] mode row uses its own codes, not text ordinals).
int gp_menu_set_values(DWORD data, const int* values, int n) {
    if (!data || n < 0 || (n && !values)) return -1;
    __try {
        DWORD t = RD32(A_LIST_INT); DWORD vt = t ? RD32(t) : 0; DWORD fn = vt ? RD32(vt + 0x44) : 0;
        DWORD L = RD32(data + 0x58);
        if (!fn || !L) return -1;
        DWORD clr = L;
        ((ListOp_t)(uintptr_t)fn)(t, nullptr, 3, &clr);
        for (int i = 0; i < n; ++i) {
            struct { int value; DWORD list; } a = {values[i], RD32(data + 0x58)};
            ((ListOp_t)(uintptr_t)fn)(t, nullptr, 9, &a);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return gp_menu_value_count(data);
}

static void dispatch_get(DWORD node) {
    __try {
        DWORD data = node ? RD32(node + 0x18) : 0;
        if (!data) return;
        int type = (int)RD32(data + 0x20);
        for (int i = 0; i < g_nh; ++i)
            if (g_h[i].type == type && g_h[i].get) { InterlockedIncrement(&g_gets); g_h[i].get(node, data); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
typedef void(__cdecl* Getter_t)(void* thread, int zero);
// MN_GET_INIT / MN_GET_RESET: `push 0; push thread; mov [0xA3B75C], entry; call 0x7FE420` -> the original, then the
// handler of the entry's option type. [0xA3B75C] is the entry node when the call starts (the getter reads it first).
static void __cdecl MenuGetWrap(void* thread, int zero) {
    DWORD node = 0;
    __try { node = RD32(A_CUR_NODE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ((Getter_t)(uintptr_t)R_MN_GETTER)(thread, zero);
    if (node) dispatch_get(node);
}
static void __cdecl MenuSetC(int* tv) {
    __try {
        if (!tv) return;
        for (int i = 0; i < g_nh; ++i)
            if (g_h[i].type == tv[0] && g_h[i].set) { InterlockedIncrement(&g_sets); g_h[i].set(tv); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// MN_SET at the setter's entry 0x6BA420 (sub esp,28h; mov eax,[0xA3B58C]): [esp+4] thread, [esp+8] int* {type, index}.
// Flags are dead at a function entry; every register is kept for the original.
__declspec(naked) void MenuSetStub() {
    __asm pushad
    __asm push dword ptr [esp + 32 + 8]
    __asm call MenuSetC
    __asm add esp, 4
    __asm popad
    __asm sub esp, 0x28
    __asm mov eax, dword ptr ds:[0xA3B58C]
    __asm jmp dword ptr [R_MN_SET]
}

int gp_menu_install() {
    if (g_tried) return g_installed ? 3 : 0;
    g_tried = true;
    if (!g_nh) return 0;
    if (!gp_ref_ok("MN_GETTER") || !gp_ref_ok("MN_CURRENT") || !gp_ref_ok("MN_LISTBUILD") || !gp_ref_ok("MN_VS4") ||
        !gp_ref_ok("MN_LISTREAD") || !gp_ref_ok("MN_SETPARAM") || !gp_ref_ok("MN_SETDIFF")) {
        gp_log("menu: reference bytes differ: menu hooks OFF\n"); return 0;
    }
    if (!gp_site_ok("MN_GET_INIT") || !gp_site_ok("MN_GET_RESET") || !gp_site_ok("MN_SET")) {
        gp_log("menu: sites differ (another mod there?): menu hooks OFF\n"); return 0;
    }
    bool ok = gp_patch_call("MN_GET_INIT", (void*)&MenuGetWrap) && gp_patch_call("MN_GET_RESET", (void*)&MenuGetWrap) &&
              gp_patch_call("MN_SET", (void*)&MenuSetStub);
    g_installed = ok;
    gp_log("menu: %s (%d handler(s): %s%s%s)\n", ok ? "hooks on" : "hooks FAILED", g_nh, g_h[0].who,
           g_nh > 1 ? ", " : "", g_nh > 1 ? g_h[1].who : "");
    return ok ? 3 : 0;
}

// ---- test host only
static void __cdecl TestMenuRedirect(DWORD getter, DWORD set_cont) { R_MN_GETTER = getter; R_MN_SET = set_cont; }
static LONG __cdecl TestMenuCounts() { return g_gets * 1000 + g_sets; }
void* gp_menu_test_stub(const char* name) {
    if (!strcmp(name, "menu_set")) return (void*)&MenuSetStub;
    if (!strcmp(name, "menu_get")) return (void*)&MenuGetWrap;
    if (!strcmp(name, "menu_redirect")) return (void*)&TestMenuRedirect;
    if (!strcmp(name, "menu_counts")) return (void*)&TestMenuCounts;
    return nullptr;
}
