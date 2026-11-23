// gp_music.cpp - the streamed-sound fix of TODGameplay.dll ([fixes] stream_limits) and its
// trace ([debug] music_trace). Maths in gp_music_logic.cpp (tests/dll/test_music.cpp).
//
// The players' report: "sometimes the music is missing unless you load another level" (the same happens in Chili Con
// Carnage, the same engine). Cause (static RE of the GOG TOD.exe): every streamed sound (music, ambient
// loops, dialogue, power-move stings) is a StreamedSoundBuffer, created by its SoundSlot the moment it first plays or is
// told to allocate_stream. The constructor 0x445770 refuses to create one when a console-era quota is hit:
//   * a stereo stream (all music) while ANY other stereo stream, or an aux mono stream, exists;
//   * a 6th normal mono stream (ambient loops, dialogue, speech);
//   * a 3rd aux mono stream, or one while a stereo stream exists;
//   * an 8th stream of any kind (the pool of 7 tokens at 0xA09314: "TERMINATED (Sorry)");
//   * a stream whose sound file's data is already used by another stream (silently).
// The SoundSlot that asked just stays silent. Nothing ever asks again: a looped music cue is played once by the mission
// script, so it stays missing until a level load frees every stream. A stream is only freed when its slot is destroyed or
// told to deallocate_stream, so a stopped stereo stream (the previous music cue, a power-move sting) keeps its quota.
//
// The fix (second version, the owner's question: can it make music overlap?) removes only the bug: a stream that is stopped (and not
// playing or asked to play) is still allocated and still counted by the engine, which is what refuses the next cue. The refusal
// stub recounts the manager's list WITHOUT the stopped streams and applies the vanilla caps to the rest (1 stereo, 5 normal mono,
// 2 aux mono, aux next to stereo; the movie rule on the manager's own total). While a stream really plays the answer is the
// vanilla one, so a second music cue during playing music is refused as before and nothing overlaps. The stopped stream is not
// freed (its slot still points at it); the pool of 7 tokens grows to 16 only so that the 8th stream is not refused. The
// manager's per-frame "XBOX WARNING: More than 7 StreamedSoundBuffers" line is silenced.
//
// [fixes] stream_limits = 1            the fix (default on; 0 = vanilla)
//         stream_pool = 16             streams alive at once (vanilla 7)
// [debug] music_trace = 1              test copy: a log line for every change of the stream picture (which streams exist,
//                                      what each is doing, the quota counts, the pool) and for every refusal
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "gp_glue.h"
#include "gp_music_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RD8(a)  (*(volatile BYTE*)(uintptr_t)(a))

namespace {
const DWORD A_MGR = 0xA35EC0;        // StreamedSoundBuffers manager (REF MU_Q_*)
const DWORD A_MOVIE = 0xA35ED4;      // fullscreen movie allocated
const DWORD A_POOL = 0xA09314, A_POOL_N = 0xA09318;

bool g_fix = true, g_trace = false;
music::Caps g_caps;
bool g_fix_on = false, g_pool_on = false, g_warn_on = false, g_dup_on = false, g_thread_on = false;
volatile LONG g_relaxed = 0, g_kept = 0, g_dups = 0;
volatile BYTE g_relax_ret = 0;
DWORD g_pool_cap = 16;

// jump targets of the stubs (check_sites.py proves each against a REF row)
DWORD R_MU_OK = 0x445B77, R_MU_FAIL = 0x445AEC;
DWORD R_MU_POOL_CREATE = 0x445F31, R_MU_POOL_FAIL = 0x445EE7;
DWORD R_MU_DUP_CONT = 0x445934, R_MU_DUP_EXIT = 0x4458ED;

bool rd_ok(DWORD a, DWORD* out) {
    __try { *out = RD32(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void copy_name(DWORD p, char* out, int cap) {   // a C string of the game, printable only
    out[0] = 0;
    __try {
        if (!p) return;
        int i = 0;
        for (; i < cap - 1; ++i) { BYTE c = RD8(p + i); if (!c) break; out[i] = (c >= 32 && c < 127) ? (char)c : '?'; }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}
const char* short_name(const char* n) {   // the file name part of "/data/sounds/..."
    const char* s = strrchr(n, '/'); return s ? s + 1 : n;
}

// ---------------------------------------------------------------------------------------------- the stream picture
bool read_counts(music::Counts* c, int* renderer) {
    DWORD mgr = 0;
    if (!rd_ok(A_MGR, &mgr) || !mgr) return false;
    __try {
        c->total = (int)RD32(mgr + 0x70); c->stereo = (int)RD32(mgr + 0x38); c->mono = (int)RD32(mgr + 0x3C); c->aux = (int)RD32(mgr + 0x40);
        if (renderer) *renderer = (int)RD32(mgr + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// the counts of the streams that are really there (stopped ones left out), read from the manager's list
bool read_active(music::Counts* out, int* renderer) {
    DWORD mgr = 0;
    if (!rd_ok(A_MGR, &mgr) || !mgr) return false;
    __try {
        int n = (int)RD32(mgr + 0x70); DWORD list = RD32(mgr + 0x6C);
        if (n < 0 || n > 64 || (n && !list)) return false;
        music::Live lv[64]; int m = 0;
        for (int i = 0; i < n; ++i) {
            DWORD p = RD32(list + 4 * i);
            if (!p) continue;
            DWORD fl = RD32(p + 0x50);
            lv[m].channels = (int)(signed char)RD8(p + 8); lv[m].aux = ((fl >> 1) & 1) != 0; lv[m].flags = fl; ++m;
        }
        *out = music::count_active(lv, m, n);
        if (renderer) *renderer = (int)RD32(mgr + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool read_snapshot(music::Snapshot* s) {
    DWORD mgr = 0;
    if (!rd_ok(A_MGR, &mgr) || !mgr) return false;
    __try {
        s->counts.total = (int)RD32(mgr + 0x70); s->counts.stereo = (int)RD32(mgr + 0x38); s->counts.mono = (int)RD32(mgr + 0x3C);
        s->counts.aux = (int)RD32(mgr + 0x40); s->renderer = (int)RD32(mgr + 0x18);
        DWORD list = RD32(mgr + 0x6C); int n = s->counts.total;
        if (n < 0 || n > 200) return false;
        s->n = 0;
        for (int i = 0; i < n && s->n < 24; ++i) {
            DWORD p = RD32(list + 4 * i);
            if (!p) continue;
            music::StreamRow& r = s->row[s->n++];
            r.ptr = p; r.channels = (signed char)RD8(p + 8); r.flags = RD32(p + 0x50); r.ready = RD8(p + 0x60) != 0;
            r.aux = ((r.flags >> 1) & 1) != 0;
            r.state = RD32(p + 0x7C); r.t0 = RD32(p + 0x5C);
            DWORD wav = RD32(p + 0x74);
            char full[128]; copy_name(wav ? RD32(wav + 0x3C) : 0, full, sizeof full);
            strncpy_s(r.name, short_name(full), _TRUNCATE);
        }
        DWORD pool = RD32(A_POOL); int pn = (int)RD32(A_POOL_N);
        s->pool_entries = pn; s->pool_in_use = 0;
        if (pn >= 0 && pn < 1000 && pool) for (int i = 0; i < pn; ++i) if (RD8(pool + 8 * i + 4)) ++s->pool_in_use;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A non-looped stream that reached its end of file (flag 100h) is stopped by Update 0x4443D0 once the clock passes the time stamp
// +5Ch plus +E8h seconds; the script that waits for that stop (the next music cue) never goes on when it does not come.
// Reports a stream that stays playing with the end flag long after.
struct EndWatch { DWORD ptr = 0, since = 0; int reports = 0; DWORD lf = 0, lb8 = 0, lbc = 0; };
// raw words of a stream object (test-copy diagnostics): +40h .. +F0h
void dump_stream(const char* tag, DWORD ptr) {
    char b[1024]; int n = 0; b[0] = 0;
    __try { for (DWORD o = 0x40; o < 0xF0 && n < 1000; o += 4) n += _snprintf_s(b + n, sizeof b - n, _TRUNCATE, "%02X:%08X ", o, RD32(ptr + o)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("music_trace: %s stream %08X raw %s\n", tag, ptr, b);
}
void watch_ends(const music::Snapshot& cur, EndWatch* w, int nw) {
    DWORD now = timeGetTime();
    for (int i = 0; i < cur.n; ++i) {
        const music::StreamRow& r = cur.row[i];
        bool pending = (r.flags & 0x80) && (r.flags & 0x100) && !(r.flags & 0x200);
        EndWatch* e = nullptr;
        for (int k = 0; k < nw; ++k) if (w[k].ptr == r.ptr) e = &w[k];
        if (e) {
            DWORD b8 = 0, bc = 0; __try { b8 = RD32(r.ptr + 0xB8); bc = RD32(r.ptr + 0xBC); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (r.flags != e->lf || b8 != e->lb8 || bc != e->lbc) {
                gp_log("music_trace: watch %08X: flags %08X b8 %08X bc %08X (+%u ms after the end flag)\n", r.ptr, r.flags, b8, bc, now - e->since);
                e->lf = r.flags; e->lb8 = b8; e->lbc = bc;
            }
        }
        if (!pending) { if (e) { gp_log("music_trace: watch %08X: left the pending state, flags %08X (+%u ms)\n", r.ptr, r.flags, now - e->since); e->ptr = 0; } continue; }
        if (!e) { for (int k = 0; k < nw && !e; ++k) if (!w[k].ptr) e = &w[k]; if (!e) continue; e->ptr = r.ptr; e->since = now; e->reports = 0; gp_log("music_trace: EOF flag seen: '%s' flags %08X clock %u\n", r.name, r.flags, now); dump_stream("EOF", r.ptr); }
        DWORD age = now - e->since;
        if (age > 4000 + 3000u * e->reports && e->reports < 8) {
            float dur = 0.0f; DWORD tick_dummy = 0; (void)tick_dummy;
            __try { dur = *(volatile float*)(uintptr_t)(r.ptr + 0xE8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            gp_log("music_trace: END PENDING %u ms: stream %08X '%s' is still playing with the end flag: flags %08X fill state %u t0 %u (clock now %u, t0 + %.2f s = %.0f) -> the stop that the script waits for has not come\n",
                   age, r.ptr, r.name, r.flags, r.state, r.t0, now, dur, (double)r.t0 + dur * 1000.0);
            if (e->reports == 0) dump_stream("PENDING", r.ptr);
            ++e->reports;
        }
    }
}

DWORD WINAPI TraceThread(LPVOID) {
    music::Snapshot prev, cur; bool have = false;
    EndWatch ew[8];
    for (;;) {
        Sleep(40);
        memset(&cur, 0, sizeof cur);
        if (!read_snapshot(&cur)) { if (!have) Sleep(400); continue; }
        watch_ends(cur, ew, 8);
        if (!have || music::snapshot_changed(prev, cur)) {
            char line[2048]; music::snapshot_text(cur, line, sizeof line);
            gp_log("music_trace: %s\n", line);
            prev = cur; have = true;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------- stubs
BYTE __cdecl RefuseRelaxC(DWORD self, DWORD aux, DWORD sound_file) {
    music::Counts c; int rend = 0; DWORD wav = 0; char name[128]; name[0] = 0;
    if (!read_counts(&c, &rend)) return 0;
    int channels = 1;
    __try { wav = RD32(self + 0x74); channels = wav ? (int)RD32(wav + 0x14) : 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    copy_name(sound_file ? RD32(sound_file + 0x2C) : 0, name, sizeof name);
    music::Request r; r.channels = channels; r.aux = (aux & 0xFF) != 0; r.movie = RD8(A_MOVIE) != 0;
    music::Reason v = music::vanilla_refusal(c, r);
    music::Counts act = c;
    if (!read_active(&act, nullptr)) act = c;
    music::Reason x = music::active_refusal(act, r);
    bool allow = g_fix_on && v != music::OK && x == music::OK;
    if (allow) {
        LONG k = InterlockedIncrement(&g_relaxed);
        if (k <= 40 || k % 100 == 0)
            gp_log("fixes: stream_limits: '%s' (%d ch%s) would be refused (%s: stereo %d, mono %d, aux %d, streams %d; playing or pending: stereo %d, mono %d, aux %d): created, the streams in the way were stopped (%ld so far)\n",
                   short_name(name), channels, r.aux ? ", aux" : "", music::reason_text(v), c.stereo, c.mono, c.aux, c.total, act.stereo, act.mono, act.aux, k);
    } else {
        LONG k = InterlockedIncrement(&g_kept);
        if (g_trace || k <= 10)
            gp_log("%s: stream '%s' (%d ch%s) REFUSED by the engine (%s; stereo %d, mono %d, aux %d, streams %d; playing or pending: stereo %d, mono %d, aux %d, so the refusal stands)\n",
                   g_trace ? "music_trace" : "fixes: stream_limits", short_name(name), channels, r.aux ? ", aux" : "",
                   music::reason_text(v), c.stereo, c.mono, c.aux, c.total, act.stereo, act.mono, act.aux);
    }
    return allow ? 1 : 0;
}

// MU_REFUSE at 0x445AE2: cmp [esp+2Ch],ebx / je 0x445B77. ebx = 0; ebp = the new StreamedSoundBuffer; the ctor's arguments
// are at [esp+88h] (sound file), [esp+98h] (prealloc byte), [esp+9Ch] (aux byte).
__declspec(naked) void StreamRefuseStub() {
    __asm {
        cmp dword ptr [esp + 0x2C], ebx
        je go_on
        pushad
        mov eax, dword ptr [esp + 0x20 + 0x88]
        push eax
        movzx eax, byte ptr [esp + 0x24 + 0x9C]
        push eax
        push ebp
        call RefuseRelaxC
        add esp, 12
        mov g_relax_ret, al
        popad
        cmp g_relax_ret, 0
        je refuse
        mov dword ptr [esp + 0x2C], 0
    go_on:
        jmp dword ptr [R_MU_OK]
    refuse:
        jmp dword ptr [R_MU_FAIL]
    }
}

// MU_POOL at 0x445EDE: cmp byte [esp+98h],bl / jne 0x445F31. eax is free here.
__declspec(naked) void StreamPoolStub() {
    __asm {
        cmp byte ptr [esp + 0x98], bl
        jne make
        mov eax, dword ptr ds:[0xA09318]
        cmp eax, g_pool_cap
        jge full
    make:
        jmp dword ptr [R_MU_POOL_CREATE]
    full:
        jmp dword ptr [R_MU_POOL_FAIL]
    }
}

void __cdecl DupLogC(DWORD self) {
    LONG k = InterlockedIncrement(&g_dups);
    char name[128]; DWORD wav = 0; name[0] = 0;
    __try { wav = RD32(self + 0x74); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    copy_name(wav ? RD32(wav + 0x3C) : 0, name, sizeof name);
    gp_log("music_trace: stream '%s' REFUSED: the sound file's stream data is already used by another stream (%ld so far)\n", short_name(name), k);
}
// MU_DUP at 0x4458E6: test al,al / je 0x445934 / mov [ebp+74h],ebx
__declspec(naked) void StreamDupStub() {
    __asm {
        test al, al
        je free_file
        pushad
        push ebp
        call DupLogC
        add esp, 4
        popad
        mov dword ptr [ebp + 0x74], ebx
        jmp dword ptr [R_MU_DUP_EXIT]
    free_file:
        jmp dword ptr [R_MU_DUP_CONT]
    }
}
}   // namespace

// ================================================================================================ interface
static bool stress_config(int n);   // [debug] music_stress (below)
static bool stress_on();
void music_load_config() {
    g_fix = gp_ini().geti("fixes", "stream_limits", 1) != 0;
    music::Caps c;
    c.pool = gp_ini().geti("fixes", "stream_pool", c.pool);
    g_caps = music::clamp_caps(c);
    g_pool_cap = (DWORD)g_caps.pool;
    g_trace = gp_ini().geti("debug", "music_trace", 0) != 0;
    if (stress_config(gp_ini().geti("debug", "music_stress", 0))) g_trace = true;   // the stress needs the trace sites and the trace thread
}
bool music_trace_wanted() { return g_trace; }
bool music_wants_post() { return stress_on(); }

int music_install(bool real_exe) {
    int n = 0;
    if (g_fix || g_trace) {   // the refusal stub also runs for the trace (it logs every refusal; it relaxes nothing when the fix is off)
        bool refs = gp_ref_ok("MU_Q_MOVIE") && gp_ref_ok("MU_Q_STEREO") && gp_ref_ok("MU_Q_MONO") && gp_ref_ok("MU_Q_AUX") &&
                    gp_ref_ok("MU_OK_TARGET") && gp_ref_ok("MU_FAIL_TARGET");
        if (!refs) gp_log("fixes: stream_limits: reference bytes differ: OFF\n");
        else if (!gp_site_ok("MU_REFUSE")) gp_log("fixes: stream_limits: site differs (another mod there?): OFF\n");
        else if (gp_patch_call("MU_REFUSE", (void*)&StreamRefuseStub)) { g_fix_on = g_fix; ++n; }
    }
    if (g_fix) {
        if (gp_ref_ok("MU_POOL_FIND") && gp_ref_ok("MU_POOL_CREATE") && gp_ref_ok("MU_POOL_FAIL") && gp_site_ok("MU_POOL") &&
            gp_patch_call("MU_POOL", (void*)&StreamPoolStub)) { g_pool_on = true; ++n; }
        else if (g_fix_on) gp_log("fixes: stream_limits: pool site differs: the 7-stream pool stays\n");
        if (g_pool_on || g_fix_on) {
            const BYTE cap = 0x7F;   // the manager's per-frame 'More than 7 StreamedSoundBuffers' warning (the list never gets that long)
            if (gp_site_ok("MU_WARN") && gp_patch_bytes("MU_WARN", &cap, 1)) { g_warn_on = true; ++n; }
        }
        if (g_fix_on || g_pool_on)
            gp_log("fixes: stream_limits on: a stopped stream no longer holds a place (vanilla caps 1 stereo / 5 mono / 2 aux for the streams that play), %d streams alive (7); movie rule unchanged\n",
                   g_caps.pool);
    }
    if (g_trace) {
        if (gp_ref_ok("MU_DUP_CONT") && gp_ref_ok("MU_DUP_EXIT") && gp_site_ok("MU_DUP") && gp_patch_call("MU_DUP", (void*)&StreamDupStub)) { g_dup_on = true; ++n; }
        if (!g_thread_on && real_exe) {   // no thread in the non-game test host
            HANDLE h = CreateThread(nullptr, 0, TraceThread, nullptr, 0, nullptr);
            if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); g_thread_on = true; gp_log("music_trace: on (a line for every change of the stream picture)\n"); }
        }
    }
    return n;
}

// ---- test copy only: [debug] music_stress = N. Plays N short non-looped sounds one after the other through the game's own global
// stream (AllocateGlobalStreamedSound 0x89CF90 / PlayGlobalStreamedSound 0x89CED0 / StopGlobalStreamedSound 0x89CF50: what the
// script commands of the music use) from the game thread, once per frame (POST_INPUT), and checks each one like a mission script
// does: the stream must start playing, and it must stop by itself when the sound is over. A stream that never starts (a lost
// play request) or never stops (a missed end: the script waiting for it never goes on) is logged with the raw state words and
// then forced to stop so the run goes on. With CPU pressure (the harness's `mark burn N SECONDS`) the races between the game thread
// and the stream threads show up within minutes.
namespace {
struct Stress {
    bool on = false, armed = false; int total = 0, cycle = 0;
    int state = 0;                       // 0 idle, 1 waiting for the start, 2 waiting for the end
    double t_next = 0, t_play = 0, t_start = 0, dur = 0;
    int ok = 0, lost_start = 0, missed_end = 0, alloc_fail = 0;
    DWORD stream_at_play = 0;
} g_str;
struct Snd { const char* path; int aux; double dur; };
const Snd g_snd[] = {
    {"/data/sounds/streamedpowermoves/deathman21.ogg", 0, 0.364},
    {"/data/sounds/mission/mm010_ernestocruz/mm010_mission_start.ogg", 0, 1.0},
    {"/data/sounds/mission/mm010_ernestocruz/mm010_music_100.ogg", 1, 1.912},
    {"/data/sounds/mission/mm010_ernestocruz/mm010_music_120.ogg", 1, 1.731},
    {"/data/sounds/mission/mm010_ernestocruz/mm010_music_017.ogg", 1, 4.965},
};
typedef BYTE(__cdecl* GAlloc_t)(const char* path, int a, int looped);
typedef BYTE(__cdecl* GPlay_t)();
typedef BYTE(__cdecl* GStop_t)();
typedef void(__fastcall* VStop_t)(DWORD self, void* edx, DWORD idx);
void stress_raw(DWORD st, char* out, int cap) {
    DWORD fl = 0, f7c = 0, t0 = 0, f60 = 0; float dur = 0;
    __try { fl = RD32(st + 0x50); f7c = RD32(st + 0x7C); t0 = RD32(st + 0x5C); f60 = RD8(st + 0x60); dur = *(volatile float*)(uintptr_t)(st + 0xE8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    _snprintf_s(out, cap, _TRUNCATE, "flags %08X fill state %u t0 %u stop-finished %u dur %.2f (clock %u)", fl, f7c, t0, f60, dur, timeGetTime());
}
}
static bool stress_config(int n) { g_str.total = n; g_str.on = n > 0; return g_str.on; }
static bool stress_on() { return g_str.on; }
bool music_stress_wanted() { return g_str.on; }
void music_post_input(double now) {
    if (!g_str.on || !g_str.armed) return;
    DWORD st = 0; __try { st = RD32(0xA3DD8C); } __except (EXCEPTION_EXECUTE_HANDLER) { st = 0; }
    DWORD fl = 0; if (st) { __try { fl = RD32(st + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) { fl = 0; } }
    char raw[200];
    if (g_str.state == 0) {
        if (g_str.cycle >= g_str.total) {
            g_str.on = false;
            gp_log("music_stress: DONE %d cycles: %d ok, %d lost play requests, %d missed ends, %d allocation failures\n", g_str.cycle, g_str.ok, g_str.lost_start, g_str.missed_end, g_str.alloc_fail);
            return;
        }
        if (now < g_str.t_next) return;
        const Snd& s = g_snd[g_str.cycle % (sizeof g_snd / sizeof g_snd[0])];
        ++g_str.cycle; g_str.dur = s.dur;
        BYTE a = 0;
        __try { a = ((GAlloc_t)(uintptr_t)0x89CF90)(s.path, 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { a = 0; }
        if (!a) { ++g_str.alloc_fail; gp_log("music_stress: cycle %d '%s': the stream could not be allocated\n", g_str.cycle, s.path); g_str.t_next = now + 0.5; return; }
        BYTE pl = 0;
        __try { pl = ((GPlay_t)(uintptr_t)0x89CED0)(); } __except (EXCEPTION_EXECUTE_HANDLER) { pl = 0; }
        g_str.t_play = now; g_str.state = 1;
        if (g_str.cycle <= 3) gp_log("music_stress: cycle %d: '%s' allocated and played (play returned %d)\n", g_str.cycle, s.path, pl);
        return;
    }
    if (g_str.state == 1) {
        if (fl & 0x80) { g_str.state = 2; g_str.t_start = now; return; }
        if (st && (fl & 0x100)) { g_str.state = 2; g_str.t_start = now; return; }   // a very short sound already at its end
        if (now - g_str.t_play > 3.0) {
            ++g_str.lost_start; stress_raw(st, raw, sizeof raw);
            gp_log("music_stress: cycle %d: the stream never started playing within 3 s (a lost play request): %s\n", g_str.cycle, raw);
            __try { ((GStop_t)(uintptr_t)0x89CF50)(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_str.state = 0; g_str.t_next = now + 0.3;
        }
        return;
    }
    // state 2: playing; it must stop by itself
    if (!st || !(fl & 0x80)) { ++g_str.ok; g_str.state = 0; g_str.t_next = now + 0.25; if (g_str.cycle % 25 == 0) gp_log("music_stress: %d cycles: %d ok, %d lost starts, %d missed ends\n", g_str.cycle, g_str.ok, g_str.lost_start, g_str.missed_end); return; }
    if (now - g_str.t_start > g_str.dur + 3.0) {
        ++g_str.missed_end; stress_raw(st, raw, sizeof raw);
        gp_log("music_stress: cycle %d: the stream is still playing %.1f s after the sound is over (a missed end): %s\n", g_str.cycle, now - g_str.t_start - g_str.dur, raw);
        __try { ((VStop_t)(uintptr_t)(RD32(RD32(st)+0x2C)))(st, nullptr, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_str.state = 0; g_str.t_next = now + 0.3;
    }
}

// ---- test copy only: hold a stream on purpose (the way the script command allocate_stream(entity, truth) does), to reproduce a
// quota refusal on demand. "mu_hog IDX [aux]": SoundSlot IDX of the SoundDict list (cache +3D8h) allocates its stream
// (SoundSlot::AllocateStream 0x89D450, aux = the script's truth); "mu_free IDX": the slot's destroy 0x89D0C0.
namespace {
DWORD list_stride_of(DWORD type_global) { __try { DWORD t = RD32(type_global); return t ? RD32(t + 0x24) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
DWORD sounddict_node(int idx) {
    DWORD cd = gp_cache_data();
    __try {
        DWORD L = cd ? RD32(cd + 0x3D8) : 0;
        if (!L || idx < 0 || idx >= (int)RD32(L + 8)) return 0;
        DWORD k = list_stride_of(0xA3B8A8); if (!k) return 0;
        DWORD a = RD32(L + 4); DWORD d = a ? RD32(a + 4) : 0; if (!d) return 0;
        return RD32(d + (DWORD)idx * 4 * k);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
typedef void(__fastcall* SlotAlloc_t)(DWORD self, void* edx, DWORD entity, DWORD aux);
typedef void(__fastcall* SlotFree_t)(DWORD self);
}
// "mu_quota [tag]": the quota refusal on demand, with the game's own functions. A stereo stream that was stopped is not freed
// (only its slot's destruction, `deallocate_stream` or a level unload frees it) and keeps the one stereo place of the manager.
// (1) every stream of the manager is stopped (what a script's `stop` does; they stay allocated, silent); (2) the next music cue
// (a stereo ogg, looped) asks for the global stream (AllocateGlobalStreamedSound 0x89CF90 + PlayGlobalStreamedSound 0x89CED0, what
// the music scripts use): vanilla refuses it ("Cannot allocate stereo stream ... already a stereo or one or more aux mono streams
// allocated") and the cue stays silent; with [fixes] stream_limits it is created and plays.
static void quota_repro(const char* tag, bool stop_first) {
    typedef BYTE(__cdecl* GAlloc_t)(const char* path, int a, int looped);
    typedef BYTE(__cdecl* GPlay_t)();
    typedef void(__fastcall* VStop_t)(DWORD self, void* edx, DWORD idx);
    music::Snapshot sn; memset(&sn, 0, sizeof sn);
    __try {
        DWORD mgr = RD32(A_MGR);
        int n = mgr ? (int)RD32(mgr + 0x70) : 0; DWORD list = mgr ? RD32(mgr + 0x6C) : 0;
        int stopped = 0;
        for (int i = 0; i < n && i < 32; ++i) {
            DWORD p = RD32(list + 4 * i);
            if (!p) continue;
            if (stop_first) { ((VStop_t)(uintptr_t)RD32(RD32(p) + 0x2C))(p, nullptr, 0); ++stopped; }
        }
        music::Counts c; read_counts(&c, nullptr);
        gp_log("music_trace: mu_quota %s: (1) %d stream(s) stopped by the script, still allocated (streams %d, stereo %d, mono %d, aux %d)\n", tag, stopped, c.total, c.stereo, c.mono, c.aux);
        BYTE a = ((GAlloc_t)(uintptr_t)0x89CF90)("/data/sounds/mission/mm010_ernestocruz/mm010_music_140.ogg", 0, 1);
        BYTE pl = a ? ((GPlay_t)(uintptr_t)0x89CED0)() : 0;
        read_counts(&c, nullptr);
        gp_log("music_trace: mu_quota %s: (2) the next music cue (stereo, looped): stream %s, play %d (streams %d, stereo %d, mono %d, aux %d)\n", tag, a ? "CREATED" : "REFUSED", pl, c.total, c.stereo, c.mono, c.aux);
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("music_trace: mu_quota %s: exception\n", tag); }
}

void music_debug_mark(const char* text) {
    if (!strncmp(text, "mu_quota", 8)) { quota_repro(text[8] == ' ' ? text + 9 : "run", true); return; }
    // "mu_overlap": the same request while the music is really PLAYING (nothing stopped): must be refused exactly as in vanilla
    if (!strncmp(text, "mu_overlap", 10)) { quota_repro(text[10] == ' ' ? text + 11 : "run", false); return; }
    if (!strncmp(text, "mu_stress", 9)) { if (g_str.on) { g_str.armed = true; g_str.t_next = 0; gp_log("music_stress: started (%d cycles)\n", g_str.total); } return; }
    int idx = -1, aux = 0;
    bool hog = sscanf_s(text, "mu_hog %d %d", &idx, &aux) >= 1;
    bool fr = !hog && sscanf_s(text, "mu_free %d", &idx) == 1;
    if (!hog && !fr) return;
    DWORD node = sounddict_node(idx), data = node;   // the SoundSlot methods take the entity itself (members at +60h / +78h / +80h)
    { DWORD info = 0, st = 0; __try { info = RD32(data + 0x78); st = RD32(data + 0x80); } __except (EXCEPTION_EXECUTE_HANDLER) {}
      gp_log("music_trace: %s: SoundDict %d = node %08X, streaming info %08X, stream %08X\n", hog ? "mu_hog" : "mu_free", idx, data, info, st); }
    if (!data) { gp_log("music_trace: %s: SoundDict slot %d not found\n", hog ? "mu_hog" : "mu_free", idx); return; }
    __try {
        if (hog) { ((SlotAlloc_t)(uintptr_t)0x89D450)(data, nullptr, 0, (DWORD)aux); gp_log("music_trace: mu_hog: SoundDict %d allocates its stream (aux %d): slot stream %08X\n", idx, aux, RD32(data + 0x80)); }
        else { ((SlotFree_t)(uintptr_t)0x89D0C0)(data); gp_log("music_trace: mu_free: SoundDict %d stream destroyed\n", idx); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { gp_log("music_trace: %s %d: exception\n", hog ? "mu_hog" : "mu_free", idx); }
}

// ---- test host only
static LONG __cdecl TestRelaxed() { return g_relaxed; }
static void __cdecl TestRedirect(DWORD ok, DWORD fail, DWORD pool_create, DWORD pool_fail, DWORD dup_cont, DWORD dup_exit) {
    R_MU_OK = ok; R_MU_FAIL = fail; R_MU_POOL_CREATE = pool_create; R_MU_POOL_FAIL = pool_fail; R_MU_DUP_CONT = dup_cont; R_MU_DUP_EXIT = dup_exit;
}
void* music_test_stub(const char* name) {
    if (!strcmp(name, "music_refuse")) return (void*)&StreamRefuseStub;
    if (!strcmp(name, "music_pool")) return (void*)&StreamPoolStub;
    if (!strcmp(name, "music_dup")) return (void*)&StreamDupStub;
    if (!strcmp(name, "music_relaxed")) return (void*)&TestRelaxed;
    if (!strcmp(name, "music_redirect")) return (void*)&TestRedirect;
    return nullptr;
}
