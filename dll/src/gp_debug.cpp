// gp_debug.cpp - [debug] of TODGameplay.dll (crash investigation tools). Everything here is OFF unless
// [debug] asks for it, and it is meant for a private test copy of the game, never for play:
//   keys_file = TODGameplay_keys.txt   a key script played into the game's own keyboard arrays (menus included), so a
//                                     test run can load a save without SendInput or window focus
//   heap_check = 1                     TOD.exe's HeapAlloc / HeapFree / HeapReAlloc imports go through a tracker: every
//                                     live block is known, the last 64K frees keep their callers; a free of a block
//                                     that is not live is logged with both call chains and NOT passed on (2 = log and
//                                     pass on); HeapValidate runs before and after every UpdateLoadedBlocks
//   stress_swaps = N                   N automatic walk-border swaps between stress_a and stress_b (submap names), each
//                                     stress_min..stress_max seconds after the previous one is back in play, sent the
//                                     way a walk loader sends them (seamless tags them when [seamless] is on)
//   stress_quit = 1                    end the process after the stress run (and after a key-script 'quit')
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include <intrin.h>
#include "gp_glue.h"
#include "gp_debug_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))

static const DWORD A_SCENE = 0xA3DCBC, A_CRT_HEAP = 0xA614DC;
static const DWORD TEXT_LO = 0x401000, TEXT_HI = 0x9B3000;
// TOD.exe import slots (KERNEL32): checked against GetProcAddress before they are replaced
static const DWORD IAT_HEAPALLOC = 0x9B31DC, IAT_HEAPFREE = 0x9B31D0, IAT_HEAPREALLOC = 0x9B31E0;

static bool g_on = false;
static dbg::KeyScript g_ks; static dbg::KeyPlayer g_kp; static bool g_keys = false;
static int g_heap = 0; static bool g_heap_on = false;
static dbg::Stress g_st; static char g_sa[32] = "Downtown 01", g_sb[32] = "Downtown 02";
static float g_st_first = 6.0f, g_st_timeout = 30.0f; static bool g_quit = false;

// ================================================================================================ heap tracker
typedef LPVOID(WINAPI* HeapAlloc_t)(HANDLE, DWORD, SIZE_T);
typedef BOOL(WINAPI* HeapFree_t)(HANDLE, DWORD, LPVOID);
typedef LPVOID(WINAPI* HeapReAlloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
static HeapAlloc_t o_alloc; static HeapFree_t o_free; static HeapReAlloc_t o_realloc;
static CRITICAL_SECTION g_hcs;
static dbg::LiveSet g_live, g_live2; static dbg::FreedRing g_ring;
static volatile LONG g_bad = 0, g_frees = 0, g_allocs = 0, g_full = 0;
static volatile LONG g_swapno = 0;
static DWORD g_frame = 0;

static int capture(DWORD ebp_hint, uint32_t* out, int cap) {   // TOD.exe return addresses on the stack, nearest first
    (void)ebp_hint;
    int n = 0;
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    DWORD* top = (DWORD*)tib->StackBase;
    for (int i = 0; sp + i < top && i < 1024 && n < cap; ++i) {
        DWORD v = sp[i];
        if (v < TEXT_LO + 6 || v >= TEXT_HI) continue;
        if (dbg::looks_like_call((const uint8_t*)(uintptr_t)(v - 6))) out[n++] = v;
    }
    return n;
}
static void fmt_stack(const uint32_t* s, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < 8 && s[i]; ++i) { char b[12]; sprintf_s(b, "%s%06X", i ? " " : "", s[i]); strcat_s(out, cap, b); }
}
// bad frees per calling site: the return address after the allocator dispatch (0x4778D0 returns to 0x477930 /
// 0x47793B / 0x47792C), else the nearest captured frame
struct SiteCount { uint32_t site; LONG n; };
static SiteCount g_sites[32]; static int g_nsites = 0;
static uint32_t site_of(const uint32_t* st) {
    for (int i = 0; i < 7 && st[i]; ++i)
        if ((st[i] == 0x477930 || st[i] == 0x47793B || st[i] == 0x47792C) && st[i + 1]) return st[i + 1];
    return st[0];
}
static void count_site(uint32_t site) {
    for (int i = 0; i < g_nsites; ++i) if (g_sites[i].site == site) { ++g_sites[i].n; return; }
    if (g_nsites < 32) { g_sites[g_nsites].site = site; g_sites[g_nsites].n = 1; ++g_nsites; }
}
static void bad_free(const char* what, LPVOID p) {
    uint32_t now_st[8] = {0}; capture(0, now_st, 8);
    const dbg::FreedRec* f = g_ring.find((uint32_t)(uintptr_t)p);
    char a[128], b[128]; fmt_stack(now_st, a, sizeof a);
    LONG k = InterlockedIncrement(&g_bad);
    count_site(site_of(now_st));
    if (k > 40) return;
    if (f) {
        fmt_stack(f->stack, b, sizeof b);
        gp_log("HEAPCHECK %s %08X (size %u): ALREADY FREED %.3f s ago (swap %u, frame %u) by [%s]; now (swap %ld, frame %u) by [%s]\n",
               what, (DWORD)(uintptr_t)p, f->size, gp_now() - f->t, f->swap, f->frame, b, g_swapno, g_frame, a);
    } else gp_log("HEAPCHECK %s %08X: NOT A LIVE BLOCK (never allocated through TOD's imports, or freed long ago); now (swap %ld) by [%s]\n",
                  what, (DWORD)(uintptr_t)p, g_swapno, a);
}
static void track_alloc(LPVOID p, SIZE_T n) {
    if (!p) return;
    if (g_live.needs_rebuild()) { g_live.rebuild_into(g_live2); dbg::LiveSet t = g_live; g_live = g_live2; g_live2 = t; }
    if (!g_live.insert((uint32_t)(uintptr_t)p, (uint32_t)n) && InterlockedIncrement(&g_full) == 1) gp_log("HEAPCHECK: live table full: tracking incomplete\n");
}
static LPVOID WINAPI HkAlloc(HANDLE h, DWORD f, SIZE_T n) {
    LPVOID p = o_alloc(h, f, n);
    EnterCriticalSection(&g_hcs); track_alloc(p, n); ++g_allocs; LeaveCriticalSection(&g_hcs);
    return p;
}
static BOOL WINAPI HkFree(HANDLE h, DWORD f, LPVOID p) {
    if (!p) return o_free(h, f, p);
    EnterCriticalSection(&g_hcs);
    uint32_t sz = 0;
    bool live = g_live.erase((uint32_t)(uintptr_t)p, &sz);
    if (live) {
        dbg::FreedRec& r = g_ring.push();
        r.p = (uint32_t)(uintptr_t)p; r.size = sz; r.swap = (uint32_t)g_swapno; r.frame = g_frame; r.t = gp_now();
        capture(0, r.stack, 8);
        ++g_frees;
    } else bad_free("HeapFree", p);
    LeaveCriticalSection(&g_hcs);
    if (!live && g_heap < 2) return TRUE;           // keep the run going: the block is not freed twice
    return o_free(h, f, p);
}
static LPVOID WINAPI HkReAlloc(HANDLE h, DWORD f, LPVOID p, SIZE_T n) {
    EnterCriticalSection(&g_hcs);
    uint32_t sz = 0;
    bool live = p && g_live.find((uint32_t)(uintptr_t)p, &sz);
    if (p && !live) bad_free("HeapReAlloc", p);
    LeaveCriticalSection(&g_hcs);
    if (p && !live && g_heap < 2) {                 // never hand a dead block to the heap: make a new one
        LPVOID q = o_alloc(h, f & ~HEAP_REALLOC_IN_PLACE_ONLY, n);
        EnterCriticalSection(&g_hcs); track_alloc(q, n); LeaveCriticalSection(&g_hcs);
        return q;
    }
    LPVOID q = o_realloc(h, f, p, n);
    EnterCriticalSection(&g_hcs);
    if (q) {
        uint32_t s2 = 0;
        if (p && g_live.erase((uint32_t)(uintptr_t)p, &s2) && q != p) {   // moved: the heap released the old block
            dbg::FreedRec& r = g_ring.push();
            r.p = (uint32_t)(uintptr_t)p; r.size = s2; r.swap = (uint32_t)g_swapno; r.frame = g_frame; r.t = gp_now();
            r.stack[0] = 0xEEEEEEEE; capture(0, r.stack + 1, 7);   // EEEEEEEE marks "moved by HeapReAlloc"
        }
        track_alloc(q, n);
    }
    LeaveCriticalSection(&g_hcs);
    return q;
}
static bool patch_iat(DWORD slot, const char* fn, void* hook, void** orig) {
    void* real = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), fn);
    DWORD cur = RD32(slot);
    if (!real || cur != (DWORD)(uintptr_t)real) { gp_log("debug: import slot %06X is not kernel32!%s (%08X vs %p): heap_check OFF\n", slot, fn, cur, real); return false; }
    *orig = real;
    DWORD old; if (!VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old)) return false;
    *(volatile DWORD*)(uintptr_t)slot = (DWORD)(uintptr_t)hook;
    VirtualProtect((void*)(uintptr_t)slot, 4, old, &old);
    return true;
}
static void heap_install() {
    const uint32_t CAP = 1u << 21, RING = 1u << 16;
    BYTE* m = (BYTE*)VirtualAlloc(nullptr, CAP * 16 + RING * sizeof(dbg::FreedRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) { gp_log("debug: no memory for the heap tracker: heap_check OFF\n"); return; }
    InitializeCriticalSectionAndSpinCount(&g_hcs, 4000);
    g_live.init((uint32_t*)m, (uint32_t*)(m + CAP * 4), CAP);
    g_live2.init((uint32_t*)(m + CAP * 8), (uint32_t*)(m + CAP * 12), CAP);
    g_ring.init((dbg::FreedRec*)(m + CAP * 16), RING);
    void *a = nullptr, *f = nullptr, *r = nullptr;
    // HeapFree first: a block allocated before the alloc hook would otherwise count as unknown
    if (!patch_iat(IAT_HEAPALLOC, "HeapAlloc", (void*)&HkAlloc, &a)) return;
    o_alloc = (HeapAlloc_t)a;
    if (!patch_iat(IAT_HEAPREALLOC, "HeapReAlloc", (void*)&HkReAlloc, &r)) return;
    o_realloc = (HeapReAlloc_t)r;
    if (!patch_iat(IAT_HEAPFREE, "HeapFree", (void*)&HkFree, &f)) return;
    o_free = (HeapFree_t)f;
    g_heap_on = true;
    gp_log("debug: heap_check %d: TOD.exe HeapAlloc / HeapFree / HeapReAlloc tracked (2M live slots, last 64K frees)%s\n", g_heap,
           g_heap < 2 ? "; bad frees are logged and skipped" : "; bad frees are logged and passed on");
}

// ================================================================================================ called by the modules
void gp_debug_swap(bool begin) {
    if (!g_on) return;
    if (begin) InterlockedIncrement(&g_swapno);
    if (begin) {   // fragment pools the asset loader picks from (chooser 0x85D6D0: vtable +50h = free bytes)
        static const DWORD pools[4] = {0xA3B478, 0xA3B130, 0xA3B4C0, 0xA3B2D0};
        static const char* pn[4] = {"cutscene/rewind", "defragmenting", "mission", "renderlist"};
        char t[256] = ""; typedef int(__fastcall* Free_t)(DWORD self, void* edx);
        for (int i = 0; i < 4; ++i) {
            int fr = -1;
            __try { DWORD vt = RD32(pools[i]); fr = ((Free_t)(uintptr_t)RD32(vt + 0x50))(pools[i], nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { fr = -1; }
            char one[64]; sprintf_s(one, "%s%s %d KB", i ? ", " : "", pn[i], fr < 0 ? -1 : fr / 1024); strcat_s(t, one);
        }
        BYTE flag = 0; __try { flag = *(volatile BYTE*)(uintptr_t)0xA3CE60; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        gp_log("POOLS before swap %ld: free %s; loading flag %u\n", g_swapno, t, flag);
    } else if (g_heap_on && g_nsites) {
        char t[400] = "";
        EnterCriticalSection(&g_hcs);
        for (int i = 0; i < g_nsites; ++i) { char one[32]; sprintf_s(one, "%s%06X x%ld", i ? ", " : "", g_sites[i].site, g_sites[i].n); strcat_s(t, one); }
        LeaveCriticalSection(&g_hcs);
        gp_log("HEAPCHECK bad frees by site so far: %s\n", t);
    }
    if (!g_heap_on) return;
    HANDLE h = (HANDLE)(uintptr_t)RD32(A_CRT_HEAP);
    double t0 = gp_now();
    BOOL ok = h ? HeapValidate(h, 0, nullptr) : TRUE;
    gp_log("HEAPCHECK %s swap %ld: HeapValidate(CRT heap %p) %s in %.0f ms; live %u, frees %ld, bad %ld\n", begin ? "before" : "after",
           g_swapno, h, ok ? "OK" : "CORRUPT", (gp_now() - t0) * 1000.0, g_live.used, g_frees, g_bad);
}

// ================================================================================================ key script (main loop, after the input poll)
// window_offscreen = 1: the test game's window is moved off the desktop (no activation), so the desktop mouse cannot
// hover-select menu items under the key script and the window does not cover other work
static bool g_offscreen = false;
static BOOL CALLBACK OffscreenCb(HWND h, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    RECT r;
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindowRect(h, &r) && r.right - r.left > 200 && r.left > -3000) {
        SetWindowPos(h, nullptr, -3800, 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        gp_log("debug: window moved off the desktop (%ld,%ld -> -3800,40)\n", r.left, r.top);
    }
    return TRUE;
}
// waitlog: the game's own log (configpc logdumpfile, [debug] game_log), re-read at most 4 times a second
static char g_game_log[64] = "tod_test_log.txt";
static bool __cdecl GameLogHas(const char* text, void*) {
    static double last = -1.0; static bool last_hit = false; static char last_text[48] = "";
    double now = gp_now();
    if (strcmp(last_text, text)) { strcpy_s(last_text, text); last = -1.0; }
    if (last >= 0.0 && now - last < 0.25) return last_hit;
    last = now; last_hit = false;
    char p[MAX_PATH * 2]; GetModuleFileNameA(nullptr, p, MAX_PATH); char* sl = strrchr(p, '\\'); if (sl) *sl = 0;
    strcat_s(p, "\\"); strcat_s(p, g_game_log);
    FILE* f = _fsopen(p, "rb", _SH_DENYNO); if (!f) return false;
    static char buf[1 << 21]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    for (size_t i = 0; i < n; ++i) if (!buf[i]) buf[i] = ' ';
    int want = 1; const char* t = text;   // "N*TEXT": TEXT at least N times
    if (t[0] >= '1' && t[0] <= '9' && t[1] == '*') { want = t[0] - '0'; t += 2; }
    int got = 0;
    for (const char* q = strstr(buf, t); q && got < want; q = strstr(q + 1, t)) ++got;
    last_hit = got >= want;
    return last_hit;
}
// key-script marks seen so far ("keys:NAME" in another script's waitlog = this key script reached `mark NAME`)
static char g_marks[16][48]; static int g_nmarks = 0;
bool gp_debug_log_has(const char* text) {
    if (!strncmp(text, "keys:", 5)) {
        for (int i = 0; i < g_nmarks && i < 16; ++i) if (!strcmp(g_marks[i], text + 5)) return true;
        return false;
    }
    return GameLogHas(text, nullptr);
}
void gp_debug_post_input(BYTE* edges, BYTE* state, double now) {
    ++g_frame;
    if (g_offscreen && g_frame % 30 == 1 && g_frame < 20000) EnumWindows(OffscreenCb, 0);
    if (!g_keys || !edges || !state) return;
    dbg::KeyOut o = g_kp.step(g_ks, now, &GameLogHas, nullptr);
    if (o.mark) {
        DWORD fpid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fpid);
        gp_log("debug keys: mark %s (game window %s, keyboard acquired %u)\n", o.mark,
               fpid == GetCurrentProcessId() ? "in front" : "NOT in front", (unsigned)edges[0x200]);
        strncpy_s(g_marks[g_nmarks % 16], o.mark, _TRUNCATE); ++g_nmarks;
    }
    // the keyboard object's "acquired" byte (+200h): without the window in front DirectInput is not acquired and the
    // menus ignore the arrays; while the script runs it is set so the injected keys count (the next poll resets it)
    if (!g_kp.done) edges[0x200] = 1;
    if (o.press >= 0) { edges[o.press] = 1; state[o.press] = 0x80; }
    if (o.held >= 0) { state[o.held] = 0x80; }
    if (o.release >= 0) { edges[o.release] = 2; state[o.release] = 0; }
    if (o.quit) { gp_log("debug keys: quit\n"); if (g_quit) TerminateProcess(GetCurrentProcess(), 0); }
}

// ================================================================================================ stress (input tick)
bool  seam_debug_request(DWORD folder);          // seam_game.cpp
DWORD seam_debug_find_submap(DWORD cur, const char* name);
const char* seam_debug_name(DWORD folder);
static int g_done = 0; static int g_phase = 0; static DWORD g_target = 0; static double g_t = 0, g_t_first = -1;
void gp_debug_tick(double now) {
    if (!g_on || g_st.total <= 0 || g_phase == 9) return;
    DWORD sc = 0, cur = 0;
    __try { sc = RD32(A_SCENE); cur = sc ? RD32(sc + 0x1B4) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!cur) return;
    if (g_t_first < 0) { g_t_first = now; gp_log("stress: first play tick, %d swaps %s <-> %s start in %.1f s (intervals %.2f..%.2f s)\n",
                                                 g_st.total, g_sa, g_sb, g_st_first, g_st.imin, g_st.imax); }
    if (now - g_t_first < g_st_first) return;
    if (g_phase == 1) {                                  // requested: wait for the target to be the loaded submap
        if (cur == g_target) { g_phase = 2; gp_log("stress: swap %d/%d in play (%s) %.0f ms after the request\n", g_done, g_st.total, seam_debug_name(cur), (now - g_t) * 1000.0); g_t = now; }
        else if (now - g_t > g_st_timeout) { gp_log("stress: swap %d never reached play in %.0f s: stopped\n", g_done, g_st_timeout); g_phase = 9; }
        return;
    }
    if (g_phase == 2 && g_done >= g_st.total) {
        if (now - g_t > 3.0) {
            gp_log("stress: DONE - %d border swaps, the game is still running; bad frees %ld\n", g_done, g_bad);
            g_phase = 9;
            if (g_quit) { gp_log("stress: ending the process (stress_quit)\n"); TerminateProcess(GetCurrentProcess(), 0); }
        }
        return;
    }
    if (g_phase == 2 && now - g_t < g_st.interval(g_done)) return;
    DWORD a = seam_debug_find_submap(cur, g_sa), b = seam_debug_find_submap(cur, g_sb);
    if (!a || !b || (cur != a && cur != b)) { gp_log("stress: loaded submap %s is not %s / %s (or a folder is missing): stopped\n", seam_debug_name(cur), g_sa, g_sb); g_phase = 9; return; }
    g_target = cur == a ? b : a;
    ++g_done;
    gp_log("stress: swap %d/%d: %s -> %s\n", g_done, g_st.total, seam_debug_name(cur), seam_debug_name(g_target));
    if (!seam_debug_request(g_target)) { gp_log("stress: request failed: stopped\n"); g_phase = 9; return; }
    g_phase = 1; g_t = now;
}

// ================================================================================================ config / install
static void read_keys_file(const char* name) {
    char p[MAX_PATH * 2]; GetModuleFileNameA(nullptr, p, MAX_PATH); char* s = strrchr(p, '\\'); if (s) *s = 0;
    strcat_s(p, "\\"); strcat_s(p, name);
    FILE* f = _fsopen(p, "rb", _SH_DENYNO);
    if (!f) { gp_log("debug: key script %s not found\n", name); return; }
    static char text[8192]; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    g_keys = g_ks.parse(text);
    gp_log("debug: key script %s: %d commands, %d errors\n", name, g_ks.n, g_ks.errors);
}
bool gp_debug_load_config() {
    const gp::Ini& ini = gp_ini();
    g_heap = ini.geti("debug", "heap_check", 0);
    g_st.total = ini.geti("debug", "stress_swaps", 0);
    g_st.imin = ini.getf("debug", "stress_min", 1.2f); g_st.imax = ini.getf("debug", "stress_max", g_st.imin);
    if (g_st.imin < 0.05f) g_st.imin = 0.05f; if (g_st.imax < g_st.imin) g_st.imax = g_st.imin;
    g_st_first = ini.getf("debug", "stress_first", 6.0f);
    g_quit = ini.geti("debug", "stress_quit", 0) != 0;
    g_offscreen = ini.geti("debug", "window_offscreen", 0) != 0;
    if (const char* v = ini.get("debug", "game_log")) strncpy_s(g_game_log, v, sizeof g_game_log - 1);
    if (const char* v = ini.get("debug", "stress_a")) strncpy_s(g_sa, v, sizeof g_sa - 1);
    if (const char* v = ini.get("debug", "stress_b")) strncpy_s(g_sb, v, sizeof g_sb - 1);
    const char* kf = ini.get("debug", "keys_file");
    g_on = g_heap > 0 || g_st.total > 0 || (kf && *kf);
    if (!g_on) return false;
    gp_log("debug: ON (test copy only): heap_check %d, stress_swaps %d, keys_file %s, stress_quit %d\n", g_heap, g_st.total, kf ? kf : "-", g_quit);
    if (kf && *kf) read_keys_file(kf);
    if (g_heap > 0) heap_install();
    return true;
}
bool gp_debug_on() { return g_on; }
bool gp_debug_wants_input() { return g_on && (g_keys || g_st.total > 0 || g_offscreen); }
