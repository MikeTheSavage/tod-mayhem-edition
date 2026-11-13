// gp_hang.cpp - the hang watchdog of TODGameplay.dll ([debug] hang_watchdog = 1, on by default).
// The freezes it is for: the picture stops, the ambient sound goes on (the sound thread), nothing crashes. That is the
// game thread stuck in a loop or a wait. A low-priority thread here watches the frame counter the game thread bumps on
// every main-loop pass (POST_INPUT, 0x43C516). When it stands still for [debug] hang_seconds (5 s; 60 s while a load
// runs), the window is up and the game had the foreground, the watchdog
//   1. stops the game thread for a moment, copies its registers and stack, lets it go again;
//   2. writes the stuck thread's EIP and its return addresses to TODGameplay.log, each as module+offset, the DLL's own
//      functions by name from TODGameplay.map / TODPhysics.map next to the DLL (when they are there);
//   3. writes TODGameplay_hang_<date>_<time>.dmp next to the log: a minidump of every thread (stacks, thread info, the
//      data sections of TOD.exe and the mod's DLLs, memory the stacks point at; a few MB), read with
//      dll/tools/hang_report.py;
//   4. logs "back after N s" if the frames come back.
// One report per stall, at most [debug] hang_max_dumps dumps per run. Inert on any exe but the GOG one (started by
// install(), which only runs there), without dbghelp.dll it logs and skips the dump. A minimised window, an alt-tab, a
// debugger and a main thread outside the main loop (boot, shut-down) are not hangs.
// Test copy only: [debug] debug_stall_key = KEY stalls the game thread for [debug] debug_stall_seconds (8 s) in
// hang_debug_stall(), [debug] hang_ignore_focus = 1 counts a background window as in front (the test window is).
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include "gp_glue.h"
#include "gp_hang_logic.h"

namespace {

bool g_on = false, g_started = false, g_ignore_focus = false, g_log_on = true;
hang::Cfg g_c;
int g_max_dumps = 3, g_dumps = 0, g_stalls = 0;
int g_stall_key = -1; float g_stall_s = 8.0f;
volatile LONG g_beat = 0, g_loading = 0;
const char* volatile g_where = "";
DWORD g_main_tid = 0; HANDLE g_main_thread = nullptr; DWORD g_stack_base = 0, g_stack_limit = 0;
HWND g_hwnd = nullptr;
HMODULE g_self = nullptr, g_phys = nullptr;
char g_dir[MAX_PATH] = "";
HANDLE g_logh = INVALID_HANDLE_VALUE;
LARGE_INTEGER g_qpf;

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDump_t g_mdwd = nullptr;

// the maps of the mod's own DLLs (TODGameplay.dll, TODPhysics.dll)
struct ModMap { HMODULE mod; hang::MapSyms m; bool ok; bool stale; };
ModMap g_maps[2];

// the stuck thread's copy (static: nothing is allocated while the game thread may hold a heap lock)
const DWORD STACK_COPY = 256 * 1024;
DWORD g_stack[STACK_COPY / 4];
DWORD g_stack_n = 0;
CONTEXT g_ctx;
char g_report[24 * 1024];
size_t g_rlen = 0;

// TOD.exe main loop (0x43C4C2..0x43C53F): the return addresses its calls leave on the stack. One of them on the
// game thread's stack = the thread is inside a main-loop pass (not booting, not shutting down).
const DWORD LOOP_RETS[] = {0x43C4E2 /* message pump */, 0x43C4ED /* window state */, 0x43C4F6 /* minimised wait */,
                           0x43C507, 0x43C516 /* mouse / keyboard poll */, 0x43C51B /* pad poll + POST_INPUT */,
                           0x43C51D /* the frame (update + render) */, 0x43C530, 0x43C53F};

double now_s() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (double)q.QuadPart / (double)g_qpf.QuadPart; }

// ---------------------------------------------------------------------------------------------- the log (no CRT locks)
void wlog(const char* fmt, ...) {
    if (!g_log_on || g_logh == INVALID_HANDLE_VALUE) return;
    char buf[2048]; SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf_s(buf, _TRUNCATE, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) n = 0;
    va_list a; va_start(a, fmt); int m = _vsnprintf_s(buf + n, sizeof buf - n, _TRUNCATE, fmt, a); va_end(a);
    if (m < 0) m = (int)strlen(buf + n);
    DWORD w = 0; WriteFile(g_logh, buf, (DWORD)(n + m), &w, nullptr);
}
void rep(const char* fmt, ...) {   // a report line: into the log and into the dump's comment
    char buf[1024];
    va_list a; va_start(a, fmt); int m = _vsnprintf_s(buf, _TRUNCATE, fmt, a); va_end(a);
    if (m < 0) m = (int)strlen(buf);
    wlog("%s", buf);
    if (g_rlen + (size_t)m < sizeof g_report - 1) { memcpy(g_report + g_rlen, buf, (size_t)m); g_rlen += (size_t)m; g_report[g_rlen] = 0; }
}

// ---------------------------------------------------------------------------------------------- modules (no loader lock)
struct Mod { DWORD base, size; char name[48]; };
Mod g_mods[96]; int g_nmods = 0;
const Mod* mod_of(DWORD a) {
    for (int i = 0; i < g_nmods; ++i) if (a - g_mods[i].base < g_mods[i].size) return &g_mods[i];
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void*)(uintptr_t)a, &mb, sizeof mb) || mb.Type != MEM_IMAGE || !mb.AllocationBase) return nullptr;
    DWORD base = (DWORD)(uintptr_t)mb.AllocationBase, size = 0;
    __try {
        const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)(uintptr_t)base;
        if (d->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(uintptr_t)(base + d->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        size = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (g_nmods >= 96) return nullptr;
    Mod& m = g_mods[g_nmods++];
    m.base = base; m.size = size; strcpy_s(m.name, "?");
    char p[MAX_PATH];
    if (K32GetMappedFileNameA(GetCurrentProcess(), (void*)(uintptr_t)base, p, MAX_PATH)) {
        const char* s = strrchr(p, '\\'); strncpy_s(m.name, s ? s + 1 : p, _TRUNCATE);
    }
    return &m;
}
bool executable(DWORD a) {
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void*)(uintptr_t)a, &mb, sizeof mb) || mb.State != MEM_COMMIT) return false;
    return (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
// "TODGameplay.dll+0x1234 hang_debug_stall+0x45" / "TOD.exe+0x3C51D (0x0083C51D)" / "0x12345678"
void describe(DWORD a, char* out, size_t cap) {
    const Mod* m = mod_of(a);
    if (!m) { _snprintf_s(out, cap, _TRUNCATE, "0x%08X", a); return; }
    DWORD off = a - m->base;
    if (m->base == 0x400000) { _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X (0x%08X)", m->name, off, a); return; }
    for (const ModMap& mm : g_maps) {
        if (!mm.ok || (DWORD)(uintptr_t)mm.mod != m->base) continue;
        uint32_t so = 0; const hang::Sym* s = mm.m.find(off, &so);
        if (s) {
            char nm[160]; hang::short_name(mm.m.name_of(s), nm, sizeof nm);
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X %s+0x%X (%s)", m->name, off, nm, so, mm.m.obj_of(s));
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X", m->name, off);
}
bool is_return_address(DWORD v) {
    if (v < 0x10000 || v >= 0x80000000u) return false;
    const Mod* m = mod_of(v);
    if (!m || v - m->base < 0x1000 || !executable(v)) return false;
    __try { return hang::looks_like_call((const uint8_t*)(uintptr_t)(v - 7)); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------------------------- the game window
BOOL CALLBACK pick_window(HWND h, LPARAM l) {
    if (!IsWindowVisible(h) || GetParent(h)) return TRUE;
    RECT r; if (!GetClientRect(h, &r)) return TRUE;
    LONG* best = (LONG*)l; LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area >= best[0]) { best[0] = area; g_hwnd = h; }
    return TRUE;
}
HWND game_window() {
    if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
    g_hwnd = nullptr;
    if (!g_main_tid) return nullptr;
    LONG best[1] = {-1};
    EnumThreadWindows(g_main_tid, pick_window, (LPARAM)best);
    return g_hwnd;
}
bool usable() {
    HWND h = game_window();
    if (!h) return false;
    if (IsIconic(h)) return false;
    if (g_ignore_focus) return true;
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------------------------------------- the dump
bool keeps_data(ULONG64 base) {
    return base == 0x400000 || base == (ULONG64)(uintptr_t)g_self || (g_phys && base == (ULONG64)(uintptr_t)g_phys);
}
BOOL CALLBACK dump_cb(PVOID, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    if (!in || !out) return TRUE;
    if (in->CallbackType == ModuleCallback && !keeps_data(in->Module.BaseOfImage)) out->ModuleWriteFlags &= ~ModuleWriteDataSeg;
    return TRUE;
}
void write_dump() {
    if (!g_mdwd) { rep("hang watchdog: dbghelp.dll / MiniDumpWriteDump not found: no dump written\n"); return; }
    if (g_dumps >= g_max_dumps) { rep("hang watchdog: %d dumps written this run already (hang_max_dumps): no dump\n", g_dumps); return; }
    SYSTEMTIME st; GetLocalTime(&st);
    char path[MAX_PATH + 64];
    _snprintf_s(path, _TRUNCATE, "%s\\TODGameplay_hang_%04d%02d%02d_%02d%02d%02d.dmp", g_dir, st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { wlog("hang watchdog: cannot create %s (error %lu)\n", path, GetLastError()); return; }
    MINIDUMP_USER_STREAM us; us.Type = CommentStreamA; us.BufferSize = (ULONG)(g_rlen + 1); us.Buffer = g_report;
    MINIDUMP_USER_STREAM_INFORMATION usi; usi.UserStreamCount = 1; usi.UserStreamArray = &us;
    MINIDUMP_CALLBACK_INFORMATION cb; cb.CallbackRoutine = dump_cb; cb.CallbackParam = nullptr;
    MINIDUMP_TYPE t = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                      MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData |
                                      MiniDumpWithFullMemoryInfo);
    double t0 = now_s();
    BOOL ok = g_mdwd(GetCurrentProcess(), GetCurrentProcessId(), f, t, nullptr, &usi, &cb);
    DWORD err = ok ? 0 : GetLastError();
    LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(f, &sz);
    CloseHandle(f);
    if (!ok) { wlog("hang watchdog: MiniDumpWriteDump failed (error 0x%08lX)\n", err); DeleteFileA(path); return; }
    ++g_dumps;
    const char* s = strrchr(path, '\\');
    wlog("hang watchdog: dump written: %s (%.1f MB in %.0f ms) - send it with TODGameplay.log\n", s ? s + 1 : path,
         sz.QuadPart / 1048576.0, (now_s() - t0) * 1000.0);
}

// ---------------------------------------------------------------------------------------------- the report
bool capture_main() {
    g_stack_n = 0;
    if (!g_main_thread) return false;
    if (SuspendThread(g_main_thread) == (DWORD)-1) return false;
    // nothing that takes a lock between Suspend and Resume: registers and a memcpy of the stack
    memset(&g_ctx, 0, sizeof g_ctx); g_ctx.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(g_main_thread, &g_ctx);
    if (ok) {
        DWORD sp = g_ctx.Esp, top = g_stack_base;
        if (sp >= g_stack_limit && sp < top) {
            DWORD n = top - sp; if (n > STACK_COPY) n = STACK_COPY;
            __try { memcpy(g_stack, (const void*)(uintptr_t)sp, n); g_stack_n = n / 4; } __except (EXCEPTION_EXECUTE_HANDLER) { g_stack_n = 0; }
        }
    }
    ResumeThread(g_main_thread);
    return ok != FALSE;
}
void report(double silent) {
    ++g_stalls;
    g_rlen = 0; g_report[0] = 0;
    if (!capture_main()) { wlog("hang watchdog: the frames stopped %.1f s ago, but the game thread could not be read\n", silent); return; }
    bool in_loop = false;
    for (DWORD i = 0; i < g_stack_n && !in_loop; ++i) for (DWORD r : LOOP_RETS) if (g_stack[i] == r) { in_loop = true; break; }
    if (!in_loop) {
        char e[200]; describe(g_ctx.Eip, e, sizeof e);
        wlog("hang watchdog: no frame for %.1f s, the game thread is outside the main loop (booting / closing?) at %s: not a hang, no dump\n", silent, e);
        return;
    }
    int pm = gp_playmode();
    char e[200]; describe(g_ctx.Eip, e, sizeof e);
    rep("hang watchdog: HANG #%d: no frame for %.1f s (frame %ld, playmode %d, %s, window %s) - the game thread is stuck\n",
        g_stalls, silent, g_beat, pm, g_loading ? "in a load" : "not loading", g_ignore_focus ? "focus ignored (test)" : "in front");
    rep("hang watchdog:   last mod step entered: %s\n", g_where && *g_where ? g_where : "-");
    rep("hang watchdog:   EIP %s\n", e);
    rep("hang watchdog:   eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X efl %08X\n", g_ctx.Eax, g_ctx.Ebx,
        g_ctx.Ecx, g_ctx.Edx, g_ctx.Esi, g_ctx.Edi, g_ctx.Ebp, g_ctx.Esp, g_ctx.EFlags);
    int shown = 0;
    for (DWORD i = 0; i < g_stack_n && shown < 48; ++i) {
        DWORD v = g_stack[i];
        if (!is_return_address(v)) continue;
        char d[200]; describe(v, d, sizeof d);
        rep("hang watchdog:   #%02d esp+%05X %s\n", shown, i * 4, d);
        ++shown;
    }
    rep("hang watchdog:   %d return addresses in %lu KB of stack (a scan: some may be stale)\n", shown, g_stack_n * 4 / 1024);
    write_dump();
}

// ---------------------------------------------------------------------------------------------- the thread
void load_map(int slot, HMODULE mod, const char* map_name) {
    ModMap& mm = g_maps[slot]; mm.mod = mod; mm.ok = false; mm.stale = false;
    if (!mod) return;
    char p[MAX_PATH + 64]; _snprintf_s(p, _TRUNCATE, "%s\\%s", g_dir, map_name);
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 || size > (64u << 20)) { CloseHandle(f); return; }
    char* text = (char*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    bool rd = text && ReadFile(f, text, size, &got, nullptr) && got == size;
    CloseHandle(f);
    if (rd) {   // the map must be this build's: its time stamp is the module's PE time stamp
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mod;
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const BYTE*)mod + dos->e_lfanew);
        uint32_t ts = hang::map_timestamp(text, size);
        if (ts != nt->FileHeader.TimeDateStamp) {
            mm.stale = true; rd = false;
            wlog("hang watchdog: %s is from another build (time stamp %08X, the DLL's %08lX): no names from it\n",
                 map_name, ts, (unsigned long)nt->FileHeader.TimeDateStamp);
        }
    }
    if (rd) {
        int cap = 32768; uint32_t tcap = 2u << 20;
        hang::Sym* sym = (hang::Sym*)VirtualAlloc(nullptr, sizeof(hang::Sym) * cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        char* names = (char*)VirtualAlloc(nullptr, tcap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (sym && names && mm.m.parse(text, size, sym, cap, names, tcap) > 0) mm.ok = true;
    }
    if (text) VirtualFree(text, 0, MEM_RELEASE);
}
DWORD WINAPI watch_thread(void*) {
    // after DllMain: dbghelp and the maps are loaded here, never under the loader lock
    char lp[MAX_PATH + 32]; _snprintf_s(lp, _TRUNCATE, "%s\\TODGameplay.log", g_dir);
    g_logh = CreateFileA(lp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    char sysdir[MAX_PATH], dp[MAX_PATH + 16];
    HMODULE dh = nullptr;
    if (GetSystemDirectoryA(sysdir, MAX_PATH)) { _snprintf_s(dp, _TRUNCATE, "%s\\dbghelp.dll", sysdir); dh = LoadLibraryA(dp); }
    if (!dh) dh = LoadLibraryA("dbghelp.dll");
    g_mdwd = dh ? (MiniDumpWriteDump_t)GetProcAddress(dh, "MiniDumpWriteDump") : nullptr;
    g_phys = GetModuleHandleA("TODPhysics.dll");
    load_map(0, g_self, "TODGameplay.map");
    load_map(1, g_phys, "TODPhysics.map");
    wlog("hang watchdog: on (a report after %.1f s without a frame, %.0f s during a load; at most %d dumps; %s; names: %s%s)\n",
         g_c.seconds, g_c.load_seconds, g_max_dumps, g_mdwd ? "dbghelp ok" : "NO dbghelp.dll: log lines only",
         g_maps[0].ok ? "TODGameplay.map" : "no TODGameplay.map", g_maps[1].ok ? " + TODPhysics.map" : "");
    hang::Watch w;
    for (;;) {
        Sleep(250);
        hang::Sample s;
        s.beat = (uint32_t)g_beat; s.now = now_s(); s.loading = g_loading != 0; s.debugger = IsDebuggerPresent() != FALSE;
        s.usable = usable();
        hang::Act a = w.poll(g_c, s);
        if (a == hang::STALL) report(w.stall_s);
        else if (a == hang::RECOVERED) wlog("hang watchdog: the game thread is back after %.1f s without a frame (hang #%d)\n", w.stall_s, g_stalls);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------- the module interface
void hang_load_config() {
    const gp::Ini& ini = gp_ini();
    g_on = ini.geti("debug", "hang_watchdog", 1) != 0;
    g_log_on = ini.geti("general", "log", 1) != 0;
    g_c.seconds = ini.getf("debug", "hang_seconds", 5.0f);
    if (g_c.seconds < 2.0f) g_c.seconds = 2.0f; if (g_c.seconds > 120.0f) g_c.seconds = 120.0f;
    g_c.load_seconds = ini.getf("debug", "hang_load_seconds", 60.0f);
    if (g_c.load_seconds < g_c.seconds) g_c.load_seconds = g_c.seconds;
    g_max_dumps = ini.geti("debug", "hang_max_dumps", 3);
    if (g_max_dumps < 0) g_max_dumps = 0; if (g_max_dumps > 20) g_max_dumps = 20;
    g_ignore_focus = ini.geti("debug", "hang_ignore_focus", 0) != 0;
    g_stall_key = gp_key_cfg("debug", "debug_stall_key", "NONE");
    g_stall_s = ini.getf("debug", "debug_stall_seconds", 8.0f);
    if (g_stall_s < 0.5f) g_stall_s = 0.5f; if (g_stall_s > 30.0f) g_stall_s = 30.0f;
}
bool hang_enabled() { return g_on; }

void hang_start() {
    if (!g_on || g_started) return;
    g_started = true;
    QueryPerformanceFrequency(&g_qpf);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&hang_start, &g_self);
    GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
    char* s = strrchr(g_dir, '\\'); if (s) *s = 0;
    HANDLE t = CreateThread(nullptr, 64 * 1024, watch_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) { gp_log("hang watchdog: thread not started (error %lu): off\n", GetLastError()); g_on = false; return; }
    CloseHandle(t);
}

void hang_beat() {
    if (!g_started) return;
    if (!g_main_tid) {   // the first pass: this is the game thread
        g_main_tid = GetCurrentThreadId();
        g_main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        g_stack_base = (DWORD)(uintptr_t)tib->StackBase;
        // the copy is esp..StackBase (at most STACK_COPY); StackLimit moves down as the stack grows, so only a floor
        g_stack_limit = g_stack_base > 0x1010000 ? g_stack_base - 0x1000000 : 0x10000;
    }
    InterlockedIncrement(&g_beat);
}

void hang_where(const char* what) { g_where = what; }
void hang_loading(bool on) { InterlockedExchange(&g_loading, on ? 1 : 0); }

// the test-copy stall: the game thread spins here, so the report has to name this function
extern "C" __declspec(noinline) void hang_debug_stall(float seconds) {
    double t0 = now_s(), until = t0 + seconds;
    volatile DWORD spins = 0;
    while (now_s() < until) { ++spins; YieldProcessor(); }
}
void hang_post_input(const BYTE* kb_edges) {
    if (!g_started || g_stall_key < 0 || g_stall_key >= 256 || !kb_edges) return;
    if (!(kb_edges[g_stall_key] & 1)) return;
    gp_log("hang watchdog: debug stall key: the game thread stops for %.1f s in hang_debug_stall (test copy)\n", g_stall_s);
    hang_where("hang_debug_stall (debug_stall_key)");
    hang_debug_stall(g_stall_s);
    hang_where("");
    gp_log("hang watchdog: debug stall over\n");
}
