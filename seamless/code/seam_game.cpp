// seam_game.cpp - the [seamless] feature of TODGameplay.dll: border swaps triggered by a walk loader play
// without a load screen. Off unless [seamless] enabled = 1; when off nothing here touches the game.
//
// Research: the author's reverse-engineering notes on seamless transitions and level streaming (not included). Sites and REF rows: dll/src/sites.h section "Seamless"; every
// hook in dll/HOOKS.md section "Seamless". How a walk-border swap runs now:
//   1. SL_WALKREQ  the ActionType 21 action body sends command_load_block(action) to LoadCtrl: the entries that call
//                  appended to Load_pending_list are tagged 'walk' (and the action's target folder too).
//   2. SL_LSPUSH   the processing state is about to run the load-screen state 0x748840: the pending batch is
//                  classified (seam_logic classify: every entry walk-tagged, no teleport, map/submap blocks only).
//   3. SL_LSSKIP   inside the load-screen state, after the world pause (wind + pause mode stay on) and before the
//                  fade-out: a seamless batch takes the state's own no-fade-node path (no fade-out, no 0.3 s wait at
//                  0x748AAC, no 2 load-screen frames); the kill-text / HUD / rumble messages and the AmbientSoundCtrl
//                  Shutdown_for_loading(1) still run (the post-load state sends Resume_after_Loading).
//      SL_LSSKIP2 right after those messages: the state exits instead of choosing and showing a load screen.
//   4. SL_PROGRESS the progress bar's SetProgress (vtable slot) draws and presents nothing during the swap, so the last
//                  rendered game frame stays on screen while UpdateLoadedBlocks runs (one long frame, no black).
//   5. SL_SWAP     UpdateLoadedBlocks is timed (every load: border swaps, missions, taxis, deaths; SL_SWAP_RG: save
//                  loads) and logged.
//   6. SL_POSTFADE post-load, fade-after-load branch: no black fade node, no 0.2 s black wait; the HUD message, the
//                  unpause and the rest of the post-load state run as usual.
//   7. SL_FADEIN   the fade-from-black message is not sent (nothing was faded); the swap ends here (timing logged).
// Everything else (mission starts, taxis, ZAP, cutscenes, deaths, menu loads, corridor teleports) takes the vanilla
// path because its batch is not walk-tagged or has a teleport.
//
// Prefetch (optional, [seamless] prefetch = 1): near a loader volume of TODGameplay_seamless.txt the blocks it loads
// are read into the OS file cache on a background thread (plain file reads of their byte range in the archive; the
// engine stays single-threaded and untouched), so the swap's read hits the cache.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include "../../dll/src/gp_glue.h"
#include "seam_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define SL_SAVE_XMM  __asm sub esp, 0x80 __asm movdqu [esp+0x00], xmm0 __asm movdqu [esp+0x10], xmm1 __asm movdqu [esp+0x20], xmm2 \
                     __asm movdqu [esp+0x30], xmm3 __asm movdqu [esp+0x40], xmm4 __asm movdqu [esp+0x50], xmm5 \
                     __asm movdqu [esp+0x60], xmm6 __asm movdqu [esp+0x70], xmm7
#define SL_LOAD_XMM  __asm movdqu xmm0, [esp+0x00] __asm movdqu xmm1, [esp+0x10] __asm movdqu xmm2, [esp+0x20] \
                     __asm movdqu xmm3, [esp+0x30] __asm movdqu xmm4, [esp+0x40] __asm movdqu xmm5, [esp+0x50] \
                     __asm movdqu xmm6, [esp+0x60] __asm movdqu xmm7, [esp+0x70] __asm add esp, 0x80

// ---- game addresses (each proven by a sites.h row; check_sites.py compares the R_ / P_ constants with the sites)
static const DWORD A_PLAYER_HOLDER = 0xA3B590, A_LISTTYPE = 0xA3B8F0;
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764;
static DWORD R_SL_GETPOS = 0x88CA80, R_SL_GETPROP = 0x86A6A0;   // REF F_GETPOS / F_GETPROP (test host redirects them)
static const int   PROP_SPECIFIC_ID = 0xA9F;
// stub targets / returns (patchable by the test host through seam_test_stub("redirect"))
static DWORD R_SL_SENDCMD = 0x86A340;     // SL_WALKREQ original call target (Node::SendCommand, ret 8)
static DWORD R_SL_COPUSH = 0x48E700;      // SL_LSPUSH original call target (push a coroutine state)
static DWORD R_SL_LS_TEXT = 0x748B6E;     // SL_LSSKIP seamless path: past the fade-out, the 0.3 s wait and 2 frames, to the
                                           // state's own text / HUD / rumble / sound-shutdown messages (REF SL_LS_TEXT)
static DWORD R_SL_LS_BACK = 0x7489A6;     // SL_LSSKIP normal path: after the re-executed mov eax,[0xA3B58C]
static DWORD R_SL_LS_EXIT = 0x74A801;     // SL_LSSKIP2 seamless path: the state's +84h check and exit (no load screen)
static DWORD R_SL_LS2_BACK = 0x748D17;    // SL_LSSKIP2 normal path: after the re-executed push 0; mov ecx,ebp; call touch
static DWORD R_SL_TOUCH = 0x86B4B0;       // the touch call SL_LSSKIP2 re-executes (REF F_TOUCH)
static DWORD R_SL_PROG = 0x87B830;        // SL_PROGRESS original slot value (SetProgress, ret 8)
static DWORD R_SL_POST_HUD = 0x747DBE;    // SL_POSTFADE seamless path: post-load case 0xB body (HUD, fade-in site, unpause)
static DWORD R_SL_POST_BACK = 0x747CAB;   // SL_POSTFADE normal path: after the re-executed mov edx,[0xA3B58C]
static DWORD R_SL_SENDMSG = 0x869E60;     // SL_FADEIN original call target (Node::SendMessage, ret 8)
static DWORD R_SL_SWAP = 0x8986E0;        // SL_SWAP / SL_SWAP_RG original call target (UpdateLoadedBlocks_Impl, ret 8)

typedef void(__fastcall* Get_t)(DWORD node, void* edx, int id, void* out);   // ret 8
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);            // ret 4

// ---------------------------------------------------------------------------------------------- state
static sl::Cfg g_c;
static bool g_installed = false, g_prefetch_on = false;
static sl::Tags g_tags;
static sl::Swap g_swap;
static sl::Stats g_stats;
static int g_swaps_seamless = 0, g_batches = 0;
static double g_swap_t0 = 0.0; static int g_swap_kind = 0;
static double g_last_ms = 0.0;
static char g_batch_text[256] = "";
// flags read by the naked stubs
static volatile BYTE g_ls_pending = 0;     // a seamless batch was decided; the next load-screen state skips its visuals
static volatile BYTE g_hold = 0;           // seamless swap running: progress bar off, post-load fade skipped
static volatile BYTE g_fade_skip = 0;
// walk request bracket (main thread only)
static DWORD g_wr_list = 0, g_wr_before[32]; static int g_wr_n = 0;

static DWORD rd(DWORD a) { __try { return RD32(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

// Load_pending_list of a LoadCtrl node: data +0Ch -> list; count = [list+8] / [[0xA3B8F0]+24h]; element i =
// [[[list+4]+4] + 4*es*i] = s_BlockLoad* {+0 folder, +4 sTeleportName char*} (REF SL_LIST, SL_LIST_EL, SL_ENTRY_TP)
static int read_pending(DWORD node, sl::Pending* out, int cap, DWORD* plist) {
    __try {
        DWORD data = node ? RD32(node + 0x18) : 0; if (!data) return -1;
        DWORD list = RD32(data + 0x0C); if (!list) return -1;
        if (plist) *plist = list;
        DWORD lt = RD32(A_LISTTYPE); if (!lt) return -1;
        int es = (int)RD32(lt + 0x24); if (es <= 0 || es > 16) return -1;
        int n = (int)RD32(list + 8) / es; if (n < 0 || n > 64) return -1;
        DWORD a = RD32(list + 4); DWORD base = a ? RD32(a + 4) : 0;
        if (n && !base) return -1;
        int k = 0;
        for (int i = 0; i < n && k < cap; ++i) {
            DWORD p = RD32(base + 4 * es * i);
            sl::Pending& e = out[k++];
            e.entry = p; e.folder = p ? RD32(p) : 0;
            DWORD tp = p ? RD32(p + 4) : 0;
            e.teleport = tp && *(volatile char*)(uintptr_t)tp != 0;
            e.kind = e.folder ? ((int)(RD32(e.folder + 0x50) << 3) >> 3) : -1;   // REF SL_FOLDER_KIND
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int specific_id(DWORD folder) {   // GetProperty(Specific_ID) as the load-screen state does (0x748840)
    if (!folder || !gp_ref_ok("F_GETPROP")) return -1;
    int v = -1;
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    __try { ((Get_t)(uintptr_t)R_SL_GETPROP)(folder, nullptr, PROP_SPECIFIC_ID, &v); } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1; }
    if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
    if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
    if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
    return v;
}

static void describe(const sl::Pending* p, int n, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        int id = specific_id(p[i].folder);
        char one[64];
        if (p[i].kind == 2) sprintf_s(one, "%s%s", i ? " + " : "", sl::submap_name(id));
        else if (p[i].kind == 1) sprintf_s(one, "%s%s", i ? " + " : "", sl::map_name(id));
        else sprintf_s(one, "%sblock kind %d id %d", i ? " + " : "", p[i].kind, id);
        strcat_s(out, cap, one);
        if (p[i].teleport) strcat_s(out, cap, " (teleport)");
    }
}

// ---------------------------------------------------------------------------------------------- C halves of the stubs
static void __cdecl WalkPreC(DWORD loadctrl, DWORD action_data) {
    double now = gp_now();
    g_wr_n = 0; g_wr_list = 0;
    sl::Pending p[32];
    int n = read_pending(loadctrl, p, 32, nullptr);
    for (int i = 0; i < n; ++i) g_wr_before[g_wr_n++] = p[i].entry;
    DWORD target = rd(action_data + 0x24);              // TargetEntity (REF SL_AT21)
    if (target) g_tags.add_folder(target, now);
    g_wr_list = loadctrl;
}
static void __cdecl WalkPostC() {
    double now = gp_now();
    sl::Pending p[32];
    int n = read_pending(g_wr_list, p, 32, nullptr);
    int added = 0;
    for (int i = 0; i < n; ++i) {
        bool old = false;
        for (int j = 0; j < g_wr_n; ++j) if (g_wr_before[j] == p[i].entry) old = true;
        if (!old && p[i].entry) { g_tags.add_entry(p[i].entry, now); ++added; }
    }
    if (g_c.log_swaps && added == 0 && n >= 0) gp_log("seamless: a walk loader asked for a block, nothing new pending (already queued or loading)\n");
}
static void __cdecl LsPushC(DWORD loadctrl) {
    double now = gp_now();
    if (g_swap.active) { gp_log("seamless: the previous border swap never reached its post-load end: dropped\n"); g_swap.active = false; g_hold = 0; }
    sl::Pending p[16];
    int n = read_pending(loadctrl, p, 16, nullptr);
    sl::Decision d;
    if (n < 0) { d.v = sl::V_NORMAL; d.why = "pending list unreadable"; n = 0; }
    else d = sl::classify(g_c, p, n, g_tags, now);
    double trig = g_tags.first_time();
    g_tags.clear();
    ++g_batches;
    if (g_c.log_swaps) describe(p, n, g_batch_text, sizeof g_batch_text);
    if (d.v == sl::V_SEAMLESS) {
        g_swap.begin(trig, now, n);
        g_ls_pending = 1;
        if (g_c.log_swaps) gp_log("seamless: load #%d: %s -> SEAMLESS border swap (%d walk entries, %d by folder); %.0f ms since the trigger\n",
                                  g_batches, g_batch_text, d.walk_by_entry, d.walk_by_folder, (now - g_swap.t_trigger) * 1000.0);
    } else {
        g_ls_pending = 0;
        if (g_c.log_swaps) gp_log("seamless: load #%d: %s -> normal load (%s)\n", g_batches, g_batch_text, d.why);
    }
}
static volatile BYTE g_skip2 = 0;
static BYTE __cdecl LsSkipC() {
    if (!g_ls_pending) return 0;
    g_ls_pending = 0;
    if (!g_swap.active) return 0;
    g_hold = 1; g_skip2 = 1;
    return 1;
}
static BYTE __cdecl LsSkip2C() {
    if (!g_skip2) return 0;
    g_skip2 = 0;
    return g_hold && g_swap.active ? 1 : 0;
}
void gp_debug_swap(bool begin);   // dll/src/gp_debug.cpp ([debug] heap_check: HeapValidate around every swap)
static void __cdecl SwapBeginC(int kind) {
    hang_loading(true);   // [debug] hang_watchdog: one long frame by design (its longer limit applies)
    gp_debug_swap(true);
    g_swap_t0 = gp_now();
    g_swap_kind = kind == 2 ? 2 : (g_hold ? 1 : 0);
    if (g_swap_kind == 1) g_swap.t_swap0 = g_swap_t0;
}
static void __cdecl SwapEndC() {
    double t1 = gp_now(), ms = (t1 - g_swap_t0) * 1000.0;
    g_last_ms = ms;
    g_stats.add(g_swap_kind, ms);
    if (g_swap_kind == 1) g_swap.t_swap1 = t1;
    gp_debug_swap(false);
    hang_loading(false);
    static const char* kn[3] = {"normal load", "seamless border swap", "save load"};
    if (g_c.log_swaps)
        gp_log("seamless: UpdateLoadedBlocks %.0f ms (%s; %d so far, avg %.0f ms, min %.0f, max %.0f)\n", ms, kn[g_swap_kind],
               g_stats.n[g_swap_kind], g_stats.avg(g_swap_kind), g_stats.min_ms[g_swap_kind], g_stats.max_ms[g_swap_kind]);
}
static void __cdecl FadeInC() {
    g_fade_skip = 0;
    if (!g_hold || !g_swap.active) return;
    g_fade_skip = 1;
    double now = gp_now();
    g_swap.t_end = now; g_swap.active = false; g_hold = 0;
    ++g_swaps_seamless;
    if (g_c.log_swaps) {
        double sw = g_swap.t_swap1 > g_swap.t_swap0 ? (g_swap.t_swap1 - g_swap.t_swap0) * 1000.0 : -1.0;
        gp_log("seamless: border swap #%d done (%s): blocks swapped in %.0f ms, trigger to play %.0f ms (decided %.0f ms "
               "after the trigger); skipped: fade-out 0.25 s, wait 0.3 s, load screen + progress bar, black 0.2 s, fade-in 0.3 s\n",
               g_swaps_seamless, g_batch_text, sw, (now - g_swap.t_trigger) * 1000.0, (g_swap.t_decide - g_swap.t_trigger) * 1000.0);
    }
}

// ---------------------------------------------------------------------------------------------- the stubs
// SL_WALKREQ call site 0x7E69EA: thiscall SendCommand(LoadCtrl ecx, 0x273, &{action}) ret 8; esi = action data.
__declspec(naked) void WalkReqStub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm push esi
    __asm push ecx
    __asm call WalkPreC
    __asm add esp, 8
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm push dword ptr [esp + 8]      // args pointer
    __asm push dword ptr [esp + 8]      // command id (0x273)
    __asm call dword ptr [R_SL_SENDCMD] // ecx kept (pushad / popad): the original call, pops its 2 args
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm call WalkPostC
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm ret 8
}
// SL_LSPUSH call site 0x74A985 (ebx = LoadCtrl node): classify, then the original push of the load-screen state.
__declspec(naked) void LsPushStub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm push ebx
    __asm call LsPushC
    __asm add esp, 4
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm jmp dword ptr [R_SL_COPUSH]
}
// SL_LSSKIP jmp at 0x7489A1 (mov eax,[0xA3B58C]), load-screen state case 3/4 after the world pause: a seamless swap
// jumps to 0x748B6E, the state's own path when there is no fade node (skipping the fade-out, the 0.3 s wait and the 2
// load-screen frames). Same function, same frame: ebx thread, esi data, ebp node, edi slot are what it expects.
static volatile BYTE g_skip_now = 0;
__declspec(naked) void LsSkipStub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm call LsSkipC
    __asm mov g_skip_now, al
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm cmp byte ptr [g_skip_now], 0
    __asm je normal
    __asm jmp dword ptr [R_SL_LS_TEXT]
normal:
    __asm mov eax, dword ptr ds:[0x00A3B58C]
    __asm jmp dword ptr [R_SL_LS_BACK]
}
// SL_LSSKIP2 jmp at 0x748D0E (push 0; mov ecx,ebp; call touch), after the text / HUD / rumble / AmbientSoundCtrl
// Shutdown_for_loading(1) messages ran (the post-load state sends their counterparts): a seamless swap leaves the
// state at its +84h check / exit instead of picking and showing a load screen.
__declspec(naked) void LsSkip2Stub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm call LsSkip2C
    __asm mov g_skip_now, al
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm cmp byte ptr [g_skip_now], 0
    __asm je normal
    __asm jmp dword ptr [R_SL_LS_EXIT]
normal:
    __asm push 0
    __asm mov ecx, ebp
    __asm call dword ptr [R_SL_TOUCH]
    __asm jmp dword ptr [R_SL_LS2_BACK]
}
// SL_PROGRESS vtable slot: thiscall SetProgress(float, bool) ret 8. Nothing drawn or presented during a seamless swap.
__declspec(naked) void ProgressStub() {
    __asm cmp byte ptr [g_hold], 0
    __asm je pass
    __asm ret 8
pass:
    __asm jmp dword ptr [R_SL_PROG]
}
// SL_SWAP / SL_SWAP_RG call sites: thiscall UpdateLoadedBlocks_Impl(scene ecx, int, entity) ret 8, timed.
__declspec(naked) void SwapTimeStub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm push 0
    __asm call SwapBeginC
    __asm add esp, 4
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm push dword ptr [esp + 8]
    __asm push dword ptr [esp + 8]
    __asm call dword ptr [R_SL_SWAP]
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm call SwapEndC
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm ret 8
}
__declspec(naked) void SwapTimeRgStub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm push 2
    __asm call SwapBeginC
    __asm add esp, 4
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm push dword ptr [esp + 8]
    __asm push dword ptr [esp + 8]
    __asm call dword ptr [R_SL_SWAP]
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm call SwapEndC
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm ret 8
}
// SL_POSTFADE jmp at 0x747CA5 (mov edx,[0xA3B58C]) after the fade-after-load flag was cleared.
__declspec(naked) void PostFadeStub() {
    __asm cmp byte ptr [g_hold], 0
    __asm je normal
    __asm jmp dword ptr [R_SL_POST_HUD]
normal:
    __asm mov edx, dword ptr ds:[0x00A3B58C]
    __asm jmp dword ptr [R_SL_POST_BACK]
}
// SL_FADEIN call site 0x747E3A: thiscall SendMessage(fader, -5 fade from black, &{fade node, 0.3}) ret 8.
__declspec(naked) void FadeInStub() {
    __asm pushad
    __asm pushfd
    SL_SAVE_XMM
    __asm call FadeInC
    SL_LOAD_XMM
    __asm popfd
    __asm popad
    __asm cmp byte ptr [g_fade_skip], 0
    __asm je pass
    __asm xor eax, eax
    __asm ret 8
pass:
    __asm jmp dword ptr [R_SL_SENDMSG]
}

// ---------------------------------------------------------------------------------------------- prefetch
static sl::Table g_tab;
static sl::Scheduler g_sched;
static char g_gamedir[MAX_PATH];
static CRITICAL_SECTION g_q_cs; static HANDLE g_q_ev = nullptr; static HANDLE g_q_thread = nullptr;
static int g_q[64]; static volatile LONG g_qn = 0;
static double g_next_tick = 0.0;
static bool g_pos_ok = false;

static DWORD WINAPI PrefetchThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low I/O + CPU priority
    static BYTE buf[1 << 20];
    for (;;) {
        WaitForSingleObject(g_q_ev, INFINITE);
        for (;;) {
            int b = -1;
            EnterCriticalSection(&g_q_cs);
            if (g_qn > 0) { b = g_q[0]; memmove(g_q, g_q + 1, sizeof(int) * (g_qn - 1)); --g_qn; }
            LeaveCriticalSection(&g_q_cs);
            if (b < 0) break;
            const sl::Block& B = g_tab.b[b];
            const sl::Archive& A = g_tab.a[B.archive];
            char path[MAX_PATH * 2]; sprintf_s(path, "%s\\%s", g_gamedir, A.name);
            LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&f);
            HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) { gp_log("seamless prefetch: cannot open %s\n", A.name); continue; }
            LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
            if ((uint64_t)sz.QuadPart != A.bytes || (uint64_t)B.offset + B.size > (uint64_t)sz.QuadPart) {
                gp_log("seamless prefetch: %s is %lld bytes, the table was made for %llu: skipped (rebuild TODGameplay_seamless.txt)\n",
                       A.name, sz.QuadPart, A.bytes);
                CloseHandle(h); continue;
            }
            LARGE_INTEGER pos; pos.QuadPart = B.offset;
            DWORD left = B.size, got = 0; bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
            while (ok && left) {
                DWORD want = left < sizeof buf ? left : (DWORD)sizeof buf;
                if (!ReadFile(h, buf, want, &got, nullptr) || !got) { ok = false; break; }
                left -= got;
            }
            CloseHandle(h);
            QueryPerformanceCounter(&t1);
            const char* sh = strrchr(B.entry, '/');
            gp_log("seamless prefetch: %s %.1f MB from %s in %.0f ms%s\n", sh ? sh + 1 : B.entry, B.size / 1048576.0, A.name,
                   (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, ok ? "" : " (read error)");
        }
    }
}

static bool player_pos(float* v) {
    if (!g_pos_ok) return false;
    DWORD h = rd(A_PLAYER_HOLDER), d = h ? rd(h + 0x18) : 0, root = d ? rd(d + 0x84) : 0;   // REF SL_PLAYER
    if (!root) return false;
    __try { ((Vec_t)(uintptr_t)R_SL_GETPOS)(root, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return v[0] == v[0] && v[2] == v[2];
}

static void load_table() {
    char p[MAX_PATH + 40]; sprintf_s(p, "%s\\TODGameplay_seamless.txt", g_gamedir);
    FILE* f = _fsopen(p, "rb", _SH_DENYNO);
    if (!f) { gp_log("seamless: no TODGameplay_seamless.txt next to the exe: prefetch off\n"); return; }
    static char text[1 << 16]; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    if (!g_tab.parse(text)) { gp_log("seamless: TODGameplay_seamless.txt has no usable loader rows (%d errors): prefetch off\n", g_tab.errors); return; }
    gp_log("seamless: prefetch table: %d archives, %d blocks, %d loader volumes (%d lines ignored)\n", g_tab.na, g_tab.nb, g_tab.nl, g_tab.errors);
    InitializeCriticalSection(&g_q_cs);
    g_q_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_q_thread = g_q_ev ? CreateThread(nullptr, 0, PrefetchThread, nullptr, 0, nullptr) : nullptr;
    g_prefetch_on = g_q_thread != nullptr;
}

// ---------------------------------------------------------------------------------------------- the module interface
void seam_load_config() {
    const gp::Ini& ini = gp_ini();
    g_c.enabled = ini.geti("seamless", "enabled", 0) != 0;
    g_c.prefetch = ini.geti("seamless", "prefetch", 1) != 0;
    g_c.prefetch_distance = ini.getf("seamless", "prefetch_distance", 60.0f);
    if (g_c.prefetch_distance < 0.0f) g_c.prefetch_distance = 0.0f; if (g_c.prefetch_distance > 500.0f) g_c.prefetch_distance = 500.0f;
    g_c.prefetch_repeat = ini.getf("seamless", "prefetch_repeat", 180.0f);
    if (g_c.prefetch_repeat < 10.0f) g_c.prefetch_repeat = 10.0f;
    g_c.log_swaps = ini.geti("seamless", "log", 1) != 0;
}
bool seam_enabled() { return g_c.enabled; }
bool seam_wants_tick() { return g_c.enabled && g_c.prefetch; }

static const char* const SEAM_SITES[] = {"SL_WALKREQ", "SL_LSPUSH", "SL_LSSKIP", "SL_LSSKIP2", "SL_PROGRESS", "SL_POSTFADE", "SL_FADEIN", "SL_SWAP", "SL_SWAP_RG"};
static const char* const SEAM_REFS[] = {"SL_LIST", "SL_LIST_EL", "SL_ENTRY_TP", "SL_APPEND", "SL_LS_EXIT84", "SL_LS_TEXT", "SL_LS_SOUND", "F_TOUCH", "SL_FOLDER_KIND",
                                        "SL_POST_HUD", "SL_PROG_FN", "SL_PROG_RET", "SL_PROG_OBJ", "SL_PROG_VT", "F_SWAP",
                                        "SL_SWAP_RET", "SL_AT21", "SL_FADE_SEND", "F_CMD", "F_SENDMSG"};

bool seam_install(bool tick_ok) {
    if (!g_c.enabled) return false;
    for (const char* r : SEAM_REFS) if (!gp_ref_ok(r)) { gp_log("seamless: reference %s differs: OFF, nothing patched\n", r); return false; }
    for (const char* s : SEAM_SITES) if (!gp_site_ok(s)) { gp_log("seamless: site %s differs (another mod there?): OFF, nothing patched\n", s); return false; }
    bool ok = gp_patch_call("SL_WALKREQ", (void*)&WalkReqStub) && gp_patch_call("SL_LSPUSH", (void*)&LsPushStub) &&
              gp_patch_call("SL_LSSKIP", (void*)&LsSkipStub) && gp_patch_call("SL_LSSKIP2", (void*)&LsSkip2Stub) && gp_patch_call("SL_POSTFADE", (void*)&PostFadeStub) &&
              gp_patch_call("SL_FADEIN", (void*)&FadeInStub) && gp_patch_call("SL_SWAP", (void*)&SwapTimeStub) &&
              gp_patch_call("SL_SWAP_RG", (void*)&SwapTimeRgStub);
    DWORD prog = (DWORD)(uintptr_t)&ProgressStub;
    ok = ok && gp_patch_bytes("SL_PROGRESS", &prog, 4);
    if (!ok) { gp_log("seamless: a patch failed half way: the feature may be partial, see the site lines above\n"); return false; }
    g_installed = true;
    GetModuleFileNameA(nullptr, g_gamedir, MAX_PATH);
    char* s = strrchr(g_gamedir, '\\'); if (s) *s = 0;
    gp_log("seamless: on (9 sites): walk-border swaps skip the fade-out, the 0.3 s wait, the load screen and the black fade-in; "
           "the last frame stays on screen during the swap; every swap is timed\n");
    if (g_c.prefetch) {
        g_pos_ok = tick_ok && gp_ref_ok("SL_PLAYER") && gp_ref_ok("F_GETPOS");
        if (!g_pos_ok) gp_log("seamless: no input tick / player position references: prefetch off\n");
        else load_table();
    }
    return true;
}

void seam_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed) return;
    if (g_swap.expired(now, g_c.active_timeout)) { gp_log("seamless: border swap timed out before its post-load end: dropped\n"); g_swap.active = false; g_hold = 0; }
    if (!g_prefetch_on || now < g_next_tick) return;
    g_next_tick = now + 0.25;
    float v[3];
    if (!player_pos(v)) return;
    int due[16];
    int n = g_sched.due(g_tab, v[0], v[2], now, g_c.prefetch_distance, g_c.prefetch_repeat, due, 16);
    if (!n) return;
    EnterCriticalSection(&g_q_cs);
    for (int i = 0; i < n && g_qn < 64; ++i) g_q[g_qn++] = due[i];
    LeaveCriticalSection(&g_q_cs);
    SetEvent(g_q_ev);
}

// ---------------------------------------------------------------------------------------------- [debug] stress
// The stress run of dll/src/gp_debug.cpp asks for border swaps the way the ActionType 21 action body does
// (0x7E69D5..0x7E6A1A: command_load_block 0x273 with {block} to LoadCtrl, then command_fade_from_black_after_load 0x274),
// bracketed like SL_WALKREQ so a seamless batch is recognised. Works with [seamless] off too (vanilla swaps).
typedef void(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);   // Node::SendCommand, ret 8
static const DWORD A_CACHE_HOLDER = 0xA3B58C, A_FOLDER_TYPE = 0xA3D810;
static DWORD loadctrl_node() {   // [[[[0xA3B58C]+18h]+254h]+18h]+4Ch (as the action body reads it)
    DWORD c = rd(A_CACHE_HOLDER), d = c ? rd(c + 0x18) : 0, m = d ? rd(d + 0x254) : 0, md = m ? rd(m + 0x18) : 0;
    return md ? rd(md + 0x4C) : 0;
}
static bool is_folder(DWORD node) {   // type chain node +4, base type +24h, Folder type [0xA3D810] (Folder_::_87E640)
    DWORD ft = rd(A_FOLDER_TYPE); if (!ft || !node) return false;
    for (DWORD t = rd(node + 4), k = 0; t && k < 16; t = rd(t + 0x24), ++k) if (t == ft) return true;
    return false;
}
const char* seam_debug_name(DWORD folder) {
    if (!folder) return "(none)";
    int id = specific_id(folder);
    return sl::submap_name(id);
}
DWORD seam_debug_find_submap(DWORD cur, const char* name) {   // a sibling submap folder of the loaded one, by name
    DWORD parent = rd(cur + 0x40);
    for (DWORD c = parent ? rd(parent + 0x44) : 0, k = 0; c && k < 256; c = rd(c + 0x34), ++k) {
        if (!is_folder(c) || (((int)(rd(c + 0x50) << 3)) >> 3) != 2) continue;
        if (!_stricmp(seam_debug_name(c), name)) return c;
    }
    return 0;
}
bool seam_debug_request(DWORD folder) {
    DWORD lc = loadctrl_node();
    if (!lc || !folder || !gp_ref_ok("F_CMD")) return false;
    DWORD s1 = RD32(A_CUR_NODE), s2 = RD32(A_CUR_2), s3 = RD32(A_CUR_3);
    bool ok = true;
    if (g_installed) WalkPreC(lc, 0);
    DWORD args[2] = {folder, 0};
    __try { ((Cmd_t)(uintptr_t)R_SL_SENDCMD)(lc, nullptr, 0x273, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (g_installed) { g_tags.add_folder(folder, gp_now()); WalkPostC(); }
    __try { ((Cmd_t)(uintptr_t)R_SL_SENDCMD)(lc, nullptr, 0x274, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (RD32(A_CUR_NODE) != s1) WR32(A_CUR_NODE, s1);
    if (RD32(A_CUR_2) != s2) WR32(A_CUR_2, s2);
    if (RD32(A_CUR_3) != s3) WR32(A_CUR_3, s3);
    return ok;
}

// ---------------------------------------------------------------------------------------------- test host only
static void __cdecl TestRedirect(DWORD sendcmd, DWORD copush, DWORD ls_exit, DWORD ls_back, DWORD prog, DWORD post_hud,
                                 DWORD post_back, DWORD sendmsg, DWORD swap) {
    R_SL_SENDCMD = sendcmd; R_SL_COPUSH = copush; R_SL_LS_EXIT = ls_exit; R_SL_LS_BACK = ls_back; R_SL_PROG = prog;
    R_SL_POST_HUD = post_hud; R_SL_POST_BACK = post_back; R_SL_SENDMSG = sendmsg; R_SL_SWAP = swap;
}
static void __cdecl TestRedirect2(DWORD getprop, DWORD getpos) { R_SL_GETPROP = getprop; R_SL_GETPOS = getpos; }
static void __cdecl TestRedirect3(DWORD ls_text, DWORD ls2_back, DWORD touch) { R_SL_LS_TEXT = ls_text; R_SL_LS2_BACK = ls2_back; R_SL_TOUCH = touch; }
static int __cdecl TestState(int what) {
    switch (what) {
    case 0: return g_installed;
    case 1: return g_ls_pending;
    case 2: return g_hold;
    case 3: return g_swap.active;
    case 4: return g_swaps_seamless;
    case 5: return (int)(g_last_ms + 0.5);
    case 6: return g_stats.n[0] + g_stats.n[1] + g_stats.n[2];
    case 7: return g_tags.n_entry;
    case 8: return g_tags.n_folder;
    case 9: return g_batches;
    }
    return -1;
}
static void __cdecl TestEnable(int on) { g_c.enabled = on != 0; g_c.log_swaps = true; }
void* seam_test_stub(const char* name) {
    if (!strcmp(name, "seam_walkreq")) return (void*)&WalkReqStub;
    if (!strcmp(name, "seam_lspush")) return (void*)&LsPushStub;
    if (!strcmp(name, "seam_lsskip")) return (void*)&LsSkipStub;
    if (!strcmp(name, "seam_progress")) return (void*)&ProgressStub;
    if (!strcmp(name, "seam_swap")) return (void*)&SwapTimeStub;
    if (!strcmp(name, "seam_postfade")) return (void*)&PostFadeStub;
    if (!strcmp(name, "seam_fadein")) return (void*)&FadeInStub;
    if (!strcmp(name, "seam_redirect")) return (void*)&TestRedirect;
    if (!strcmp(name, "seam_redirect2")) return (void*)&TestRedirect2;
    if (!strcmp(name, "seam_redirect3")) return (void*)&TestRedirect3;
    if (!strcmp(name, "seam_lsskip2")) return (void*)&LsSkip2Stub;
    if (!strcmp(name, "seam_state")) return (void*)&TestState;
    if (!strcmp(name, "seam_enable")) return (void*)&TestEnable;
    return nullptr;
}
