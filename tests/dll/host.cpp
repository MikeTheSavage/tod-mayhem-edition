// host.cpp - non-game test host for TODGameplay.dll and the dsound.dll loader. It never starts or loads the game.
//
// It maps a READ-ONLY-SOURCED copy of TOD.exe's sections (read from the file, never written back) at their virtual
// addresses inside this process (the host itself is linked at 0x60000000), so the DLL sees the shipped bytes where the
// game would have them. Nothing in that image is executed except the self-contained stubs listed in test 6.
//   1. DllMain in a non-game process: the exe md5 does not match -> REFUSED, the image is byte-identical, the shared
//      memory block says REFUSED.
//   2. TestInstall with every feature off -> 0 patches, image identical.
//   3. TestInstall with every feature on -> every site of sites.h patched exactly as described (call/jmp into the DLL,
//      operand forms), every other byte of the image identical to the file.
//   4. (fresh image + reloaded DLL) only clock_fix -> exactly the 3 clock sites.
//   5. A site whose bytes were changed (simulating another mod or another exe) is skipped, the rest still installs.
//   6. Executes FtolGameStub (x87 24-bit precision, register/stack contract) and ArmDiveStub (suppressed path).
//   7. dsound.dll loader: all 12 ordinals resolve; DirectSoundEnumerateA through the proxy works.
//   9. Smart AI: SmartInitWrap and the tick run on fake enemy nodes (init / touch / position redirected to the host).
//  16. [difficulty]: off = image unchanged; on = the 3 menu sites + the input tick + [fixes]; the menu setter stub
//      (BRUTAL index 3 -> Setting_difficulty 2 + flag, registers / stack, the two moved instructions) and the getter
//      wrap (BRUTAL appended to a fake entry, shown value 3) on fake data.
//  13. [display]: off = image unchanged; defaults = exactly the display sites + the cull fix + [fixes]; the sprite
//      matrix, font scale, DrawString offset, mouse, load bar and is_wide_screen stubs on fake frames (registers,
//      stack, flags, both continuations).
//  14. [pad]: off = nothing; on = exactly its sites (+ [fixes]); the "pad present" stub; the fake DirectInput device.
//  15. [physics]: off = nothing; on = its 3 call sites + the input tick + [fixes]; the three wrappers on stand-ins.
//  17. [mayhem_title]: off = nothing; on = its 4 sites + [fixes]; the sprite / list-execute wrappers on stand-ins
//      (thiscall kept, registers kept, the title sprite's submitted list remembered and consumed, nothing drawn
//      without a device).
//  18. [psycho]: off = nothing; on = no site of its own (input tick + post-input + [fixes] only), installed; an ini
//      whose moves list names no known move refuses.
//  24. [bigger_fights]: off = nothing; on = the 5 save slot sites + the shared input tick + fixes.
//  25. [drops]: off = nothing; on = no site of its own (input tick + post-input + [fixes] only), installed, every
//      reference matches; the ammo factor replaces the difficulty's (and is the difficulty's own while off).
//  19. [gang_wars]: off = nothing; on = no site of its own (the shared input tick + [fixes]), every GW_ reference matches,
//      ini values read (gangs, spares with the bad ones refused).
//  20. [minimap]: off = nothing; on = exactly their sites (+ [fixes]); the end-of-frame wrapper on a
//      stand-in (thiscall, 3 arguments, ret 0Ch, registers kept); the radar match on fake nodes; blips read through a fake
//      list with the position call redirected.
//  21. [mayhem_heat]: off = nothing; on = its 4 event sites + the shared overlay sprite hooks + the input tick + [fixes]; the
//      kill / destroy / car stubs on stand-ins (registers, flags and stack kept, the original reached, events queued);
//      the score wrapper (HT_SCORE) on a stand-in body: 38 x the heat multiplier at levels 1 / 3 / 5
//  22. [mayhem] props: chain reactions / breakable cover need [physics]: alone = nothing; with it = the physics sites + the 2 car
//      explosion sites + the input tick + [fixes]; the car wrapper on a stand-in (cdecl, arguments, result).
//  23. [ragdoll]: off = nothing; alone = the physics world (no props) + its character-update / take_a_hit / corpse sites;
//      the corpse-delay stub on stand-ins.
// usage: host.exe <TOD.exe path> <dir with TODGameplay.dll and dsound.dll>
#include <windows.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <initializer_list>
#include "../../dll/src/todgameplay_shared.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum Kind { CALL, JMP, OPER, DATA };
struct Site { const char* id; const char* feature; DWORD va; const char* hex; int off; int len; Kind kind; };
#define SITE(id, feature, va, hex, off, len, kind, what) { #id, feature, va, hex, off, len, kind },
static const Site g_sites[] = {
#include "../../dll/src/sites.h"
};
static const int NS = sizeof(g_sites) / sizeof(g_sites[0]);

// ---------------------------------------------------------------- the simulated image
static BYTE* g_file = nullptr; static DWORD g_fsize = 0;
struct Sec { DWORD va, vsz, raw, rsz; };
static Sec g_sec[16]; static int g_nsec = 0; static DWORD g_imgsize = 0;
static bool load_file(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    g_fsize = GetFileSize(h, nullptr);
    g_file = (BYTE*)malloc(g_fsize);
    DWORD got = 0; ReadFile(h, g_file, g_fsize, &got, nullptr); CloseHandle(h);
    if (got != g_fsize) return false;
    DWORD pe = *(DWORD*)(g_file + 0x3C);
    WORD n = *(WORD*)(g_file + pe + 6), opt = *(WORD*)(g_file + pe + 20);
    g_imgsize = *(DWORD*)(g_file + pe + 24 + 56);
    BYTE* s = g_file + pe + 24 + opt;
    for (int i = 0; i < n && i < 16; ++i, s += 40) {
        g_sec[i].vsz = *(DWORD*)(s + 8); g_sec[i].va = 0x400000 + *(DWORD*)(s + 12);
        g_sec[i].rsz = *(DWORD*)(s + 16); g_sec[i].raw = *(DWORD*)(s + 20);
        g_nsec = i + 1;
    }
    return true;
}
static BYTE* g_img = nullptr;
static bool map_image() {
    // the parent reserved 0x400000.. in this (then suspended) process before the heap could take it: commit it now
    if (!g_img) g_img = (BYTE*)VirtualAlloc((void*)0x400000, g_imgsize, MEM_COMMIT, PAGE_READWRITE);
    if (g_img != (BYTE*)0x400000) return false;
    DWORD op; VirtualProtect(g_img, g_imgsize, PAGE_READWRITE, &op);
    memset(g_img, 0, g_imgsize);
    memcpy(g_img, g_file, 0x1000);
    for (int i = 0; i < g_nsec; ++i) {
        DWORD n = g_sec[i].rsz < g_sec[i].vsz ? g_sec[i].rsz : g_sec[i].vsz;
        if (g_sec[i].vsz == 0) n = g_sec[i].rsz;
        memcpy((void*)(uintptr_t)g_sec[i].va, g_file + g_sec[i].raw, n);
    }
    VirtualProtect(g_img, g_imgsize, PAGE_READONLY, &op);   // the DLL must VirtualProtect before writing, like in the game
    return true;
}
static BYTE file_byte_at(DWORD va) {   // expected (pristine) byte at a VA
    if (va < 0x401000) return g_file[va - 0x400000];
    for (int i = 0; i < g_nsec; ++i)
        if (va >= g_sec[i].va && va < g_sec[i].va + (g_sec[i].vsz ? g_sec[i].vsz : g_sec[i].rsz))
            return (va - g_sec[i].va) < g_sec[i].rsz ? g_file[g_sec[i].raw + (va - g_sec[i].va)] : 0;
    return 0;
}
// number of image bytes that differ from the file, ignoring [skip ranges]
static int diff_count(int* first_diff_va) {
    int n = 0; *first_diff_va = 0;
    for (DWORD va = 0x400000; va < 0x400000 + g_imgsize; ++va)
        if (g_img[va - 0x400000] != file_byte_at(va)) { if (!n) *first_diff_va = (int)va; ++n; }
    return n;
}

// ---------------------------------------------------------------- DLL handling
typedef DWORD(__cdecl* Status_t)(); typedef DWORD(__cdecl* TestInstall_t)(const char*, const char*);
typedef void*(__cdecl* TestStub_t)(const char*);
static HMODULE g_dll = nullptr; static char g_dir[MAX_PATH];
static DWORD g_dll_lo = 0, g_dll_hi = 0;
static bool load_dll() {
    char p[MAX_PATH + 32]; sprintf_s(p, "%s\\TODGameplay.dll", g_dir);
    g_dll = LoadLibraryA(p);
    if (!g_dll) return false;
    BYTE* b = (BYTE*)g_dll; DWORD pe = *(DWORD*)(b + 0x3C);
    g_dll_lo = (DWORD)(uintptr_t)b; g_dll_hi = g_dll_lo + *(DWORD*)(b + pe + 24 + 56);
    return true;
}
static void unload_dll() { if (g_dll) FreeLibrary(g_dll); g_dll = nullptr; }
static void* ts_of(HMODULE m, const char* name) {   // the test-only stub export of the currently loaded DLL
    TestStub_t f = m ? (TestStub_t)GetProcAddress(m, "TODGameplay_TestStub") : nullptr;
    return f ? f(name) : nullptr;
}
static bool in_dll(DWORD a) { return a >= g_dll_lo && a < g_dll_hi; }
static volatile DWORD g_fix_freed = 0;
static void __cdecl HostFixFree(DWORD p, DWORD, DWORD) { g_fix_freed = p; }   // stands in for 0x4778D0
// stand-ins for the fire shell's continuations after FIX_WPN_OWNER (0x8418DA: ecx = owner / 0x8418F6: skipped); the
// test calls the stub, so a ret goes back to the test
static volatile DWORD g_wo_hit = 0, g_wo_ecx = 0;
__declspec(naked) static void HostWoCont() { __asm mov g_wo_ecx, ecx __asm mov g_wo_hit, 1 __asm ret }
__declspec(naked) static void HostWoSkip() { __asm mov g_wo_hit, 2 __asm ret }

// [physics] stand-ins for the three wrapped originals (test 14): record the arguments, return a marker
static volatile DWORD g_ph_a[4];
// [ragdoll] stand-ins (test 23): Character::Update (thiscall, no arguments) and SendCommand (thiscall, 2 args, ret 8)
static volatile DWORD g_rd_upd = 0, g_rd_cmd[3];
static void __fastcall HostRdUpd(DWORD ch, void*) { g_rd_upd = ch; }
static float* g_rd_hitp; static void* g_rd_wh; static DWORD g_rd_r[7];   // esp before / after, eax, ebx, esi, edi, ebp
// RD_CORPSE: the stub pushes 0 and the seconds, then jumps to the continuation (here: record and drop them)
static DWORD g_rd_cp[4]; static void* g_rd_wc; static DWORD g_rd_fake_thread[0x20], g_rd_fake_frame[4];
__declspec(naked) static void HostRdCorpseCont() {
    __asm {
        pop eax
        mov g_rd_cp[0], eax
        pop eax
        mov g_rd_cp[4], eax
        mov g_rd_cp[8], edi
        mov g_rd_cp[12], esi
        ret
    }
}
__declspec(naked) static void rd_corpse_call() {   // edi = the script thread, as at 0x606D41
    __asm {
        push edi
        push esi
        lea edi, g_rd_fake_thread
        mov esi, 0x55555555
        call g_rd_wc
        pop esi
        pop edi
        ret
    }
}
__declspec(naked) static void rd_hit_call() {   // the RD_HIT call site shape: esi = hit, ebp = root, ecx = AI, 2 pushes, ret 8
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov ebx, 0x11111111
        mov esi, g_rd_hitp
        mov edi, 0x33333333
        mov ebp, 0x44444444
        mov g_rd_r[0], esp
        push 0
        push 0x534
        mov ecx, 0x0BADC0DE
        call g_rd_wh
        mov g_rd_r[4], esp
        mov g_rd_r[8], eax
        mov g_rd_r[12], ebx
        mov g_rd_r[16], esi
        mov g_rd_r[20], edi
        mov g_rd_r[24], ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
static DWORD __fastcall HostRdCmd(DWORD node, void*, int id, void* args) { g_rd_cmd[0] = node; g_rd_cmd[1] = (DWORD)id; g_rd_cmd[2] = (DWORD)(uintptr_t)args; return 0x0000C0DE; }
static unsigned __fastcall HostPhLine(DWORD probe, void*, int a, float* from, float* to) {
    g_ph_a[0] = probe; g_ph_a[1] = (DWORD)a; g_ph_a[2] = (DWORD)(uintptr_t)from; g_ph_a[3] = (DWORD)(uintptr_t)to; return 0xABCD0001;
}
static DWORD __cdecl HostPhScript(DWORD thread, DWORD arg) { g_ph_a[0] = thread; g_ph_a[1] = arg; return 0x77; }
// [mayhem] stand-in for Node::SendCommand thiscall(node, id, args) ret 8 (test 18)
static char __fastcall HostMhCmd(DWORD node, void*, int id, void* args) { g_ph_a[0] = node; g_ph_a[1] = (DWORD)id; g_ph_a[2] = (DWORD)(uintptr_t)args; return 0x5C; }
static void __fastcall HostMhBlur(DWORD ren, void*) { g_ph_a[3] = ren; }

// [mayhem_heat] stand-ins (test 18): the touch (thiscall, 1 argument), SendMessage (thiscall, 2), the destroy continuation
static volatile DWORD g_ht_ecx = 0, g_ht_a = 0, g_ht_b = 0, g_ht_hit = 0;
static void __fastcall HostHtTouch(DWORD self, void*, DWORD slot) { g_ht_ecx = self; g_ht_a = slot; g_ht_hit = 1; }
static void __fastcall HostHtSend(DWORD self, void*, DWORD id, DWORD args) { g_ht_ecx = self; g_ht_a = id; g_ht_b = args; g_ht_hit = 2; }
__declspec(naked) static void HostHtBack() { __asm mov g_ht_ecx, ecx __asm mov g_ht_hit, 3 __asm ret }
// the add_to_overall_score body (cdecl: thread, pointer to the amount)
static volatile int g_ht_score = 0; static volatile DWORD g_ht_thr = 0;
static void __cdecl HostHtScore(DWORD thread, int* amount) { g_ht_thr = thread; g_ht_score = *amount; }
// [mayhem_title] stand-ins (test 17): the wrapped originals (thiscall, no arguments)
static volatile DWORD g_mt_self = 0;
static DWORD __fastcall HostMtOrig(DWORD self, void*) { g_mt_self = self; return 0x5A5A0017; }

// [minimap] stand-ins (test 20): the layer renderer (thiscall, 3 arguments, ret 0Ch), GetWorldPosition
static volatile DWORD g_ov_a[4];
static DWORD __fastcall HostOvFrame(DWORD self, void*, DWORD a, DWORD b, DWORD c) { g_ov_a[0] = self; g_ov_a[1] = a; g_ov_a[2] = b; g_ov_a[3] = c; return 0x5A5A0018; }
static volatile DWORD g_pos_node = 0;
static void __fastcall HostGetPos(DWORD node, void*, float* out) { g_pos_node = node; out[0] = 10.0f; out[1] = 1.0f; out[2] = -20.0f; }

static const char* INI_ALL =
    "[general]\nlog = 1\n"
    "[bullettime]\nenabled = 1\nkey = MOUSE3\npad_button = PAD_RS\n"
    "[slowmo]\ndive_auto_slowmo = 0\nwire_auto_slowmo = 0\n"
    "[timing]\nclock_fix = 1\npickup_step_fix = 1\nfps_limit = 120\n"
    "[controls]\nmouse_turn_fix = 1\nmouse_sensitivity_x = 1.2\nmouse_sensitivity_y = 0.8\ninvert_y = 1\ndiagonal_normalize = 1\n"
    "[keys]\njump = SPACE\nshootdodge = LSHIFT\n"
    "[camera]\nfov = 75\ncull_fix = 1\n"
    "[restart]\nenabled = 1\nkey = F10\n"
    "[vehicles]\nsteering_return_rate = 0.25\ns_brake_parity = 1\n"
    "[suave]\nenabled = 1\ncheat_key = F7\n"
    "[smart_ai]\nenabled = 1\n"
    "[gang_wars]\nenabled = 1\n"
    "[seamless]\nenabled = 1\n"
    "[display]\nenabled = 1\nres_table = auto\nstartup_size = config\nwide_hud = 1\nhud_scale = 1\nborder = 0.03\nmenus = 1\ncull_fix = 1\nborderless = 1\n"
    "[pad]\nenabled = 1\nplaystation_hid = 0\ntell_scripts = 1\n"
    "[physics]\nenabled = 1\n"
    "[difficulty]\nenabled = 1\n"
    "[mayhem_title]\nenabled = 1\n"
    "[psycho]\nenemy_loco_moves = 1\nmoves = tornado, toro\ncooldown = 20-40\ntelegraph_seconds = 1.0\nelite_only = 1\n"
    "[minimap]\nenabled = 1\nrotate = 0\nzoom = 1.5\n"
    "[mayhem_heat]\nenabled = 1\n"
    "[mayhem]\nchain_reactions = 1\nbreakable_cover = 1\n"
    "[car_physics]\nenabled = 1\n"
    "[ragdoll]\nenabled = 1\n"
    "[bigger_fights]\nenabled = 1\n";
// ---- [difficulty] test doubles (test 16)
static volatile DWORD g_mn_eax = 0, g_mn_type = 0, g_mn_val = 0, g_mn_hit = 0;
__declspec(naked) static void HostMnSetCont() {   // stands for 0x6BA428 (the setter after sub esp,28h / mov eax,[cache])
    __asm mov g_mn_eax, eax
    __asm mov eax, [esp + 0x28 + 8]
    __asm mov ecx, [eax]
    __asm mov g_mn_type, ecx
    __asm mov ecx, [eax + 4]
    __asm mov g_mn_val, ecx
    __asm mov g_mn_hit, 1
    __asm add esp, 0x28
    __asm ret
}
static DWORD g_fake_list[4], g_fake_buf[8], g_fake_ltype[16], g_fake_lvt[32], g_fake_entry[0x30], g_fake_node[0x10];
static void __fastcall HostListOp(DWORD self, void*, int op, DWORD* args) {   // the list type's vtable +44h: op 9 = append
    if (op == 9 && args) { DWORD* L = (DWORD*)(uintptr_t)args[1]; DWORD n = L[2]; if (n < 8) { g_fake_buf[n] = args[0]; L[2] = n + 1; } }
}
static void __cdecl HostGetter(void*, int) { g_fake_entry[0x4C / 4] = 2; }   // the game read Setting_difficulty = 2

static const char* INI_OFF = "[general]\nlog = 1\n[bullettime]\nenabled = 0\n";
static const char* INI_CLOCK = "[timing]\nclock_fix = 1\n";

// host stand-ins for the Suave gate's two exits (redirected through the test-only export)
static volatile DWORD g_generic_hit = 0;
static volatile DWORD g_touch_hit = 0, g_exit_hit = 0, g_touch_ecx = 0, g_touch_arg = 0, g_exit_esp = 0, g_saved_esp = 0;
static jmp_buf g_jb;
__declspec(naked) void HostTouch() {   // stands for 0x86B4B0: thiscall touch(slot), ret 4
    __asm mov g_touch_ecx, ecx
    __asm mov eax, [esp + 4]
    __asm mov g_touch_arg, eax
    __asm mov g_touch_hit, 1
    __asm ret 4
}
static void __cdecl ExitC() { longjmp(g_jb, 1); }
__declspec(naked) void HostExit() {    // stands for 0x5C0FE9 (the use handler's common exit)
    __asm mov g_exit_esp, esp
    __asm mov g_exit_hit, 1
    __asm call ExitC
}
static void __cdecl GenericC() { longjmp(g_jb, 2); }
__declspec(naked) void HostGeneric() { // stands for 0x5C0E94 (the generic branch after its touch: slot-10 Suave)
    __asm mov g_exit_esp, esp
    __asm mov g_generic_hit, 1
    __asm call GenericC
}
// one call of the gate stub as command_use_power_up makes it: push 4; mov ecx,edi; call. Returns 1 = the touch
// returned to the caller (game path), 0 = refused at the common exit, 2 = continued in the generic branch.
static int gate_call(void* gate, DWORD pd, DWORD* regs_ok) {
    g_touch_hit = g_exit_hit = g_generic_hit = 0; g_touch_ecx = g_touch_arg = 0;
    DWORD esp_before = 0, esp_after = 0, esi_after = 0, edi_after = 0, ebx_after = 0;
    int r = setjmp(g_jb);
    if (r == 1) return 0;
    if (r == 2) return 2;
    __asm {
        push ebx
        push esi
        push edi
        mov esi, pd
        mov edi, 0x0BADC0DE
        mov ebx, 0x7E7E7E7E
        mov esp_before, esp
        mov g_saved_esp, esp
        push 4
        mov ecx, edi
        call gate
        mov esp_after, esp
        mov esi_after, esi
        mov edi_after, edi
        mov ebx_after, ebx
        pop edi
        pop esi
        pop ebx
    }
    *regs_ok = esp_after == esp_before && esi_after == pd && edi_after == 0x0BADC0DE && ebx_after == 0x7E7E7E7E &&
               g_touch_ecx == 0x0BADC0DE && g_touch_arg == 4;
    return 1;
}
static void check_patched_site(const Site& s) {
    DWORD at = s.va + s.off; const BYTE* p = (const BYTE*)(uintptr_t)at;
    if (s.kind == CALL || s.kind == JMP) {
        CHECK(p[0] == (s.kind == CALL ? 0xE8 : 0xE9));
        DWORD to = at + 5 + *(int32_t*)(p + 1);
        bool ok = in_dll(to);
        if (!ok) printf("  site %s: target %08X not in the DLL\n", s.id, to);
        CHECK(ok);
        for (int i = 5; i < s.len; ++i) CHECK(p[i] == 0x90);
    } else if (!strcmp(s.id, "CAM_VIEW")) {
        CHECK(p[0] == 0xA1 && in_dll(*(DWORD*)(p + 1)) && p[5] == 0x90);
    } else if (!strcmp(s.id, "CAM_CULL")) {
        CHECK(p[0] == 0x05 && in_dll(*(DWORD*)(p + 1)));
        CHECK(*(const BYTE*)(uintptr_t)(s.va) == 0xF3 && *(const BYTE*)(uintptr_t)(s.va + 2) == 0x10);   // still movss xmm0, m32
    } else if (!strcmp(s.id, "CAM_ASPECT")) {
        CHECK(in_dll(*(DWORD*)p));
    } else if (!strcmp(s.id, "SBRAKE_A")) {
        static const BYTE w[6] = {0xE9, 0x4B, 0x01, 0x00, 0x00, 0x90}; CHECK(!memcmp(p, w, 6));
        CHECK(at + 5 + *(int32_t*)(p + 1) == 0x4F6ED5);
    } else if (!strcmp(s.id, "SBRAKE_B")) {
        CHECK(p[0] == 0xEB && p[1] == 0x1E);
    } else if (!strcmp(s.id, "PD_CTLSTART")) {   // mov eax, imm32: the handler pointer now points into the DLL
        CHECK(*(const BYTE*)(uintptr_t)(s.va + 5) == 0xB8 && in_dll(*(DWORD*)p));
    } else if (!strcmp(s.id, "MT_SPRITE_VT") || !strcmp(s.id, "MT_TEXTBOX_VT")) {  // DATA: the vtable slot points into the DLL
        CHECK(s.kind == DATA && in_dll(*(DWORD*)p));
    } else if (!strcmp(s.id, "SL_PROGRESS")) {   // DATA: the vtable slot points into the DLL
        CHECK(s.kind == DATA && in_dll(*(DWORD*)p));
    } else if (!strncmp(s.id, "DS_RES_", 7)) {   // [display] table: a 16:9 size (auto: 1280x720 .. 2560x1440, index 4 = 1920x1080 here)
        int v = *(int*)p; bool w = s.id[7] == 'W'; int i = s.id[8] - '0';
        static const int want[5][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {1920, 1080}};
        CHECK(i >= 0 && i < 5 && v == want[i][w ? 0 : 1]);
        CHECK(*(const DWORD*)(uintptr_t)s.va == 0x042444C7 || *(const DWORD*)(uintptr_t)s.va == 0x082444C7);   // still mov [esp+N], imm32
    } else if (!strcmp(s.id, "DS_WH_H480")) {
        CHECK(*(int*)p == 480);
    } else if (!strcmp(s.id, "DS_WH_W853")) {
        CHECK(*(int*)p == 853);                      // 480 x 1920 / 1080 (no configpc next to the host: 1920x1080)
    } else if (!strncmp(s.id, "DS_WH_", 6)) {        // movss xmm0, [abs] operands -> DLL floats
        CHECK(in_dll(*(DWORD*)p));
        CHECK(*(const BYTE*)(uintptr_t)(s.va) == 0xF3 && *(const BYTE*)(uintptr_t)(s.va + 3) == 0x05);
    } else CHECK(!"unknown OPER site");
}

// ---- Smart AI test doubles (test 9): fake character_sentry_ai nodes laid out like the game's (node +4 type -> +28h
// class -> +60h handler, +18h data, +40h root; root data +14h Sub_Type, +18h FactionID; data +234h CombatCtrl -> data
// +8 target, +10h squared distance) and host stand-ins for the AI init, the member-write notify and GetWorldPosition.
struct FakeCls { DWORD pad[0x60 / 4]; DWORD handler; DWORD pad2[4]; };
struct FakeType { DWORD pad[0x28 / 4]; FakeCls* cls; DWORD pad2[4]; };
struct FakeEnemy {
    DWORD node[0x50 / 4]; DWORD data[0x280 / 4];
    DWORD rootnode[0x50 / 4]; DWORD rootdata[0x80 / 4]; DWORD combat[0x20 / 4]; DWORD cdata[0x20 / 4];
    FakeType type; FakeCls cls; FakeType rtype; FakeCls rcls; float pos[3];
    float f(DWORD off) const { return *(const float*)((const BYTE*)data + off); }
    void setf(DWORD off, float v) { *(float*)((BYTE*)data + off) = v; }
    void fight(int st, DWORD tgt, float d) { data[0x1D4 / 4] = (DWORD)st; cdata[2] = tgt; *(float*)&cdata[4] = d * d; }
};
static FakeEnemy g_fe[9];   // 0..6 spawned through the init, 7..8 "restored from a save" (found by the discovery)
static void fake_enemy(FakeEnemy& e, float x) {
    memset(&e, 0, sizeof e);
    e.node[1] = (DWORD)(uintptr_t)&e.type; e.type.cls = &e.cls; e.cls.handler = 0x5C90F0;
    e.node[0x18 / 4] = (DWORD)(uintptr_t)e.data; e.node[0x40 / 4] = (DWORD)(uintptr_t)e.rootnode;
    e.rootnode[0x18 / 4] = (DWORD)(uintptr_t)e.rootdata; e.rootdata[0x14 / 4] = 0; e.rootdata[0x18 / 4] = 9;
    e.rootnode[1] = (DWORD)(uintptr_t)&e.rtype; e.rtype.cls = &e.rcls; e.rcls.handler = 0x5C3060;   // character_root
    e.rootdata[0x58 / 4] = (DWORD)(uintptr_t)e.node;                                                 // AICtrl
    e.data[0x234 / 4] = (DWORD)(uintptr_t)e.combat; e.combat[0x18 / 4] = (DWORD)(uintptr_t)e.cdata;
    // RandomDefender row (TUNING.md 2.2), Health 100, Morale 1 (UseDefault), StopDistance 10, passive
    e.setf(0x2C, 100.0f); e.data[0xD8 / 4] = 1; e.setf(0xFC, 10.0f); e.setf(0x48, 3.0f);
    e.setf(0x4C, 0.3f); e.setf(0x50, 0.8f); e.setf(0x54, 1.0f); e.setf(0x58, 3.0f); e.setf(0x60, 1.0f); e.setf(0x64, 3.0f);
    e.setf(0x88, 30.0f); e.setf(0x8C, 30.0f); e.setf(0x90, 7.0f); e.setf(0xBC, 15.0f);
    e.data[0x1D4 / 4] = 4;
    e.pos[0] = x; e.pos[1] = 0.0f; e.pos[2] = 0.0f;
}
struct SmTestEnt { DWORD node; int used, dead, role, token, holding, sq_ok, mo_ok, react_set; float fear; };
static volatile int g_sm_init_calls = 0; static volatile DWORD g_sm_init_thread = 0;
static void __cdecl HostSmInit(void* thread, int* out) { ++g_sm_init_calls; g_sm_init_thread = (DWORD)(uintptr_t)thread; *out = 1; }
struct SmTouchRec { DWORD node; int slot; };
static SmTouchRec g_sm_touch[256]; static int g_sm_touch_n = 0;
static void __fastcall HostSmTouch(DWORD node, void*, int slot) { if (g_sm_touch_n < 256) { g_sm_touch[g_sm_touch_n].node = node; g_sm_touch[g_sm_touch_n].slot = slot; ++g_sm_touch_n; } }
// the spatial query: returns an engine-style entity list {+4 -> {+4 -> elements}, +8 count x stride} of roots
static DWORD g_sm_list_el[16]; static DWORD g_sm_list_a[2]; static DWORD g_sm_list[4]; static DWORD g_sm_list_type[16];
static int g_sm_query_n = 0, g_sm_free_n = 0; static float g_sm_query_r = 0.0f;
static void __fastcall HostSmQuery(DWORD qt, void*, DWORD* q) {
    (void)qt; ++g_sm_query_n; g_sm_query_r = *(float*)&q[4];
    int n = 0;
    for (FakeEnemy& e : g_fe) if (e.node[1]) g_sm_list_el[n++] = (DWORD)(uintptr_t)e.rootnode;
    g_sm_list_a[1] = (DWORD)(uintptr_t)g_sm_list_el; g_sm_list[1] = (DWORD)(uintptr_t)g_sm_list_a; g_sm_list[2] = (DWORD)n;   // stride 1
    q[0] = (DWORD)(uintptr_t)g_sm_list;
}
static void __fastcall HostSmFree(DWORD type, void*, DWORD* plist) { if (type == (DWORD)(uintptr_t)g_sm_list_type && *plist == (DWORD)(uintptr_t)g_sm_list) ++g_sm_free_n; *plist = 0; }
static DWORD g_sm_holder[0x20 / 4], g_sm_holder_data[0x90 / 4], g_sm_ram[0x20 / 4];
static void __fastcall HostSmPos(DWORD node, void*, float* v) {
    for (FakeEnemy& e : g_fe) if ((DWORD)(uintptr_t)e.rootnode == node) { v[0] = e.pos[0]; v[1] = e.pos[1]; v[2] = e.pos[2]; return; }
    v[0] = v[1] = v[2] = 0.0f;
}

// ---- seamless test doubles (test 11): a LoadCtrl node laid out like the game's (node +18h data, data +0Ch list,
// list {+4 -> {+4 -> elements}, +8 count}, element size from [0xA3B8F0]+24h; s_BlockLoad {+0 folder, +4 char*};
// folder +50h kind) and host stand-ins for every code address the seamless stubs call or jump to.
static DWORD g_sl_node[0x20 / 4], g_sl_data[0x20 / 4], g_sl_list[4], g_sl_arr[2], g_sl_el[8], g_sl_type[0x30 / 4];
static DWORD g_sl_entry[4][2], g_sl_folder[4][0x60 / 4], g_sl_action[0x30 / 4];
static volatile DWORD g_sl_cmd_id = 0, g_sl_cmd_ecx = 0, g_sl_copush = 0, g_sl_hit = 0, g_sl_swap_ecx = 0, g_sl_prog = 0, g_sl_msg = 0;
static int g_sl_next = 0;
static void __cdecl HostSlAppend() {   // what command_load_block does: append the next s_BlockLoad
    if (g_sl_next < 4) { g_sl_el[g_sl_list[2]] = (DWORD)(uintptr_t)g_sl_entry[g_sl_next]; ++g_sl_list[2]; ++g_sl_next; }
}
__declspec(naked) void HostSlSendCmd() {   // thiscall SendCommand(id, args) ret 8
    __asm mov g_sl_cmd_ecx, ecx
    __asm mov eax, [esp + 4]
    __asm mov g_sl_cmd_id, eax
    __asm pushad
    __asm call HostSlAppend
    __asm popad
    __asm mov eax, 1
    __asm ret 8
}
__declspec(naked) void HostSlCoPush() { __asm mov g_sl_copush, 1 __asm ret 16 }   // push a coroutine state (4 args)
__declspec(naked) void HostSlLsExit() { __asm mov g_sl_hit, 1 __asm ret }       // stands for 0x74A801
__declspec(naked) void HostSlLsText() { __asm mov g_sl_hit, 5 __asm ret }       // stands for 0x748B6E
__declspec(naked) void HostSlLs2Back() { __asm mov g_sl_hit, 6 __asm ret }      // stands for 0x748D17
__declspec(naked) void HostSlLsBack() { __asm mov g_sl_hit, 2 __asm ret }       // stands for 0x7489A6
__declspec(naked) void HostSlPostHud() { __asm mov g_sl_hit, 3 __asm ret }      // stands for 0x747DBE
__declspec(naked) void HostSlPostBack() { __asm mov g_sl_hit, 4 __asm ret }     // stands for 0x747CAB
__declspec(naked) void HostSlProg() { __asm mov g_sl_prog, 1 __asm ret 8 }      // stands for 0x87B830
__declspec(naked) void HostSlSendMsg() { __asm mov g_sl_msg, 1 __asm mov eax, 1 __asm ret 8 }   // stands for 0x869E60
static void __cdecl HostSlSleep() { Sleep(30); }
__declspec(naked) void HostSlSwap() {   // stands for UpdateLoadedBlocks_Impl: thiscall(int, entity) ret 8, takes 30 ms
    __asm mov g_sl_swap_ecx, ecx
    __asm pushad
    __asm call HostSlSleep
    __asm popad
    __asm ret 8
}
static void __fastcall HostSlGetProp(DWORD node, void*, int id, int* out) { (void)node; *out = id == 0xA9F ? 6 : -1; }   // Harbour 01
static void __fastcall HostSlGetPos(DWORD, void*, float* v) { v[0] = -306.0f; v[1] = 9.0f; v[2] = -470.0f; }
// call a stub the way the game calls / jumps to it; returns esp delta and whether ebx/esi/edi/ebp survived
struct SlRegs { DWORD esp_delta, eax, regs_ok; };
static void* g_slc_stub; static DWORD g_slc_e0, g_slc_e1, g_slc_rb, g_slc_rs, g_slc_rd, g_slc_rp, g_slc_ra;
static SlRegs sl_call(void* stub, DWORD ecx_v, DWORD ebx_v, DWORD esi_v, int nargs, DWORD a1, DWORD a2) {
    SlRegs r = {0, 0, 0};
    g_slc_stub = stub;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov g_slc_e0, esp
        mov eax, nargs
        cmp eax, 2
        jne no2
        push a2
        push a1
    no2:
        mov ecx, ecx_v
        mov ebx, ebx_v
        mov esi, esi_v
        mov edi, 0x0BADC0DE
        mov ebp, 0x7E7E7E7E
        call dword ptr [g_slc_stub]
        mov g_slc_ra, eax
        mov g_slc_rb, ebx
        mov g_slc_rs, esi
        mov g_slc_rd, edi
        mov g_slc_rp, ebp
        mov g_slc_e1, esp
        mov esp, g_slc_e0
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    r.esp_delta = g_slc_e0 - g_slc_e1; r.eax = g_slc_ra;
    r.regs_ok = g_slc_rb == ebx_v && g_slc_rs == esi_v && g_slc_rd == 0x0BADC0DE && g_slc_rp == 0x7E7E7E7E;
    return r;
}

// ---- [display] mode_menu test doubles (test 13b): the rest of SetTextIndex 0x60FA50 after its first three instructions
// (sub esp,0Ch; push esi; mov esi,ecx): record the ordinal and esi, undo the frame, ret 4
static volatile int g_dm_rest_ord = -1; static volatile DWORD g_dm_rest_esi = 0;
__declspec(naked) static void HostDmSetIdxRest() {
    __asm mov g_dm_rest_esi, esi
    __asm mov eax, [esp + 4 + 0x0C + 4]
    __asm mov g_dm_rest_ord, eax
    __asm pop esi
    __asm add esp, 0x0C
    __asm mov eax, 0x5A5A5A5A
    __asm ret 4
}
static void* __cdecl HostDmAlloc(int pool, int size) { (void)pool; return malloc((size_t)size); }
struct DmRegs { bool regs_ok, esp_ok; };
static DmRegs dm_settext_call(void* stub, DWORD tb, int ord) {
    DWORD e0 = 0, e1 = 0, rb = 0, rs = 0, rd = 0, rp = 0, p0 = 0;
    __asm {
        push ebx
        push esi
        push edi
        mov p0, ebp
        mov ebx, 0x11111111
        mov esi, 0x22222222
        mov edi, 0x33333333
        mov e0, esp
        push ord
        mov ecx, tb
        call stub
        mov e1, esp
        mov rb, ebx
        mov rs, esi
        mov rd, edi
        mov rp, ebp
        pop edi
        pop esi
        pop ebx
    }
    DmRegs r; r.regs_ok = rb == 0x11111111 && rs == 0x22222222 && rd == 0x33333333 && rp == p0; r.esp_ok = e0 == e1;
    return r;
}

// ---- [display] test doubles (test 13): the stubs' continuation addresses are redirected to these; a stub that the
// test CALLs (instead of jumping into it) comes back here with its return address on the stack.
static volatile int g_ds_path = 0;
__declspec(naked) static void HostDsFall() { __asm mov g_ds_path, 1 __asm ret }
__declspec(naked) static void HostDsJump() { __asm mov g_ds_path, 2 __asm ret }
static volatile DWORD g_ds_quad_this = 0, g_ds_quad_tex = 0; static float* g_ds_quad_p[4]; static volatile DWORD g_ds_quad_col = 0;
static void __fastcall HostDsQuad(DWORD thisp, DWORD, DWORD tex, float* p0, float* p1, float* p2, float* p3, DWORD, DWORD, DWORD, DWORD, DWORD col) {
    g_ds_quad_this = thisp; g_ds_quad_tex = tex; g_ds_quad_p[0] = p0; g_ds_quad_p[1] = p1; g_ds_quad_p[2] = p2; g_ds_quad_p[3] = p3; g_ds_quad_col = col;
}
static float g_ds_frame_vals[4];   // cx, cy, half w, half h for the sprite frame
static int g_ds_layout = 0;
static void __cdecl HostDsFillFrame(BYTE* F) {   // F = the caller frame the matrix stub sees (its esp + 10h)
    static const int offA[4] = {0x74, 0x78, 0x30, 0x14}, offB[4] = {0x60, 0x64, 0x2C, 0x10};
    for (int i = 0; i < 4; ++i) *(float*)(F + (g_ds_layout ? offB[i] : offA[i])) = g_ds_frame_vals[i];
}
// one call of a matrix stub the way the Sprite render calls 0x4671A0: frame, push 1.0 / sy / sx, ecx = matrix
static bool ds_matrix_call(void* stub, int layout, float sx, float sy, float* m) {
    g_ds_layout = layout;
    DWORD sxb, syb; memcpy(&sxb, &sx, 4); memcpy(&syb, &sy, 4);
    DWORD esp0 = 0, esp1 = 0, rb = 0, rs = 0, rd = 0, ra = 0;
    __asm {
        push ebx
        push esi
        push edi
        sub esp, 0x100
        mov eax, esp
        push eax
        call HostDsFillFrame
        add esp, 4
        mov ebx, 0x0B0B0B0B
        mov esi, 0x05050505
        mov edi, 0x0D0D0D0D
        mov esp0, esp
        push 0x3F800000
        push syb
        push sxb
        mov ecx, m
        call stub
        mov esp1, esp
        mov rb, ebx
        mov rs, esi
        mov rd, edi
        mov ra, eax
        add esp, 0x100
        pop edi
        pop esi
        pop ebx
    }
    return esp0 == esp1 && rb == 0x0B0B0B0B && rs == 0x05050505 && rd == 0x0D0D0D0D && ra == (DWORD)(uintptr_t)m;
}
// one call of a text stub: esi = font, frame = the stub's esp; corners at F + offsets; returns the path taken
static float g_ds_corner[16]; static int g_ds_corner_off[8]; static int g_ds_ncorner = 0;
static void __cdecl HostDsFillCorners(BYTE* F) { for (int i = 0; i < g_ds_ncorner * 2; ++i) *(float*)(F + g_ds_corner_off[i]) = g_ds_corner[i]; }
static void __cdecl HostDsReadCorners(BYTE* F) { for (int i = 0; i < g_ds_ncorner * 2; ++i) g_ds_corner[i] = *(float*)(F + g_ds_corner_off[i]); }
static int ds_text_call(void* stub, BYTE* font, bool* regs_ok) {
    g_ds_path = 0;
    DWORD esp0 = 0, esp1 = 0, rs = 0, ra = 0, rb = 0;
    __asm {
        push ebx
        push esi
        sub esp, 0x80
        lea eax, [esp - 4]
        push eax
        call HostDsFillCorners
        add esp, 4
        mov esi, font
        mov eax, 0x0A0A0A0A
        mov ebx, 0x0B0B0B0B
        mov esp0, esp
        call stub
        mov esp1, esp
        mov rs, esi
        mov ra, eax
        mov rb, ebx
        lea eax, [esp - 4]
        push eax
        call HostDsReadCorners
        add esp, 4
        add esp, 0x80
        pop esi
        pop ebx
    }
    *regs_ok = esp0 == esp1 && rs == (DWORD)(uintptr_t)font && ra == 0x0A0A0A0A && rb == 0x0B0B0B0B;
    return g_ds_path;
}

static int run_child(char** argv) {
    // start this exe again suspended, reserve the image range in it before its heap exists, let it run the tests
    if (!load_file(argv[1])) { printf("cannot read %s\n", argv[1]); return 2; }
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    char cmd[4 * MAX_PATH]; sprintf_s(cmd, "\"%s\" \"%s\" \"%s\" --child", self, argv[1], argv[2]);
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fflush(stdout);
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) { printf("CreateProcess failed\n"); return 2; }
    void* r = VirtualAllocEx(pi.hProcess, (void*)0x400000, g_imgsize, MEM_RESERVE, PAGE_READWRITE);
    if (r != (void*)0x400000) printf("parent: could not reserve 0x400000 in the child (%p)\n", r);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not swallow the progress lines
    if (argc < 3) { printf("usage: host.exe <TOD.exe> <dll dir>\n"); return 2; }
    if (argc < 4 || strcmp(argv[3], "--child")) return run_child(argv);
    strcpy_s(g_dir, argv[2]);
    if (!load_file(argv[1])) { printf("cannot read %s\n", argv[1]); return 2; }
    if (!map_image()) { printf("cannot map the simulated image at 0x400000 (address in use): SKIPPED\n"); return 3; }
    int dva = 0;
    CHECK(diff_count(&dva) == 0);

    // ---- 1. DllMain in a non-game process refuses
    {   // the DLL reads TODGameplay.ini from the HOST exe's folder: write an all-on ini there
        char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH); char* sl = strrchr(exe, '\\'); *sl = 0;
        char ini[MAX_PATH + 32]; sprintf_s(ini, "%s\\TODGameplay.ini", exe);
        FILE* f = nullptr; fopen_s(&f, ini, "w"); if (f) { fputs(INI_ALL, f); fclose(f); }
    }
    CHECK(load_dll());
    Status_t st = (Status_t)GetProcAddress(g_dll, "TODGameplay_Status");
    TestInstall_t ti = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
    TestStub_t ts = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
    CHECK(st && ti && ts && GetProcAddress(g_dll, "TODGameplay_Init"));
    DWORD s0 = st();
    CHECK((s0 & TODGP_F_REFUSED) != 0 && (s0 & TODGP_F_HOOKS_OK) == 0);
    CHECK(diff_count(&dva) == 0);
    {
        HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, TODGP_SHARED_NAME);
        CHECK(m != nullptr);
        if (m) {
            const TodgpShared* sh = (const TodgpShared*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(TodgpShared));
            CHECK(sh && sh->magic == TODGP_SHARED_MAGIC && sh->version == 2 && sh->size == 128 && (sh->flags & TODGP_F_REFUSED));
            CHECK(sh && sh->meter == 1.0f);
            if (sh) UnmapViewOfFile(sh);
            CloseHandle(m);
        }
    }
    printf("1. refusal in a non-game process: status %04X, image unchanged\n", s0);

    // ---- 2. every feature off
    CHECK(ti("wrong token", INI_OFF) == 0xFFFFFFFF);
    DWORD n = ti("simulated-image-not-the-game", INI_OFF);
    CHECK(n == 0);
    CHECK(diff_count(&dva) == 0);
    printf("2. all features off: %u patches, image unchanged\n", n);

    // ---- 3. every feature on
    n = ti("simulated-image-not-the-game", INI_ALL);
    printf("3. all features on: %u patches\n", n);
    CHECK(n == (DWORD)NS);
    int expected_bytes = 0;
    for (int i = 0; i < NS; ++i) { check_patched_site(g_sites[i]); expected_bytes += g_sites[i].len; }
    int d = diff_count(&dva);
    printf("   %d bytes differ from the file (patch bytes at most %d)\n", d, expected_bytes);
    CHECK(d > 0 && d <= expected_bytes);
    // every differing byte lies inside a patch range
    int outside = 0;
    for (DWORD va = 0x400000; va < 0x400000 + g_imgsize; ++va) {
        if (g_img[va - 0x400000] == file_byte_at(va)) continue;
        bool in = false;
        for (int i = 0; i < NS; ++i) if (va >= g_sites[i].va + g_sites[i].off && va < g_sites[i].va + g_sites[i].off + (DWORD)g_sites[i].len) in = true;
        if (!in) { ++outside; if (outside < 5) printf("   byte outside any site changed at %08X\n", va); }
    }
    CHECK(outside == 0);
    {   // protection restored to what it was (read only here)
        MEMORY_BASIC_INFORMATION mbi; VirtualQuery((void*)(uintptr_t)0x43C516, &mbi, sizeof mbi);
        CHECK(mbi.Protect == PAGE_READONLY);
    }

    // ---- 6. execute the self-contained stubs
    {
        typedef void (*V)();
        V ftol = (V)ts("ftol_game");
        CHECK(ftol != nullptr);
        unsigned int cw = 0; _controlfp_s(&cw, _PC_24, _MCW_PC);   // what CreateDevice leaves
        static const float k1000 = 1000.0f;
        float gdts[] = {0.016f, 0.0083333f * 0.3f, 0.0027778f * 0.3f, 0.0027778f * 0.3f, 0.0027778f * 0.3f, 0.001f, -0.0166f};
        int sum = 0; double exact = 0; int regs_ok = 1, depth_ok = 1;
        for (float g : gdts) {
            float gv = g; int r = 0; unsigned short sw0 = 0, sw1 = 0;
            DWORD b_ebx = 0, b_esi = 0, b_edi = 0, b_ebp = 0, b_ecx = 0;
            __asm {
                push ebx
                push esi
                push edi
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, 0x33333333
                mov ecx, 0x55555555
                fnstsw sw0
                fld dword ptr [gv]
                fmul dword ptr [k1000]
                call ftol
                fnstsw sw1
                mov r, eax
                mov b_ebx, ebx
                mov b_esi, esi
                mov b_edi, edi
                mov b_ecx, ecx
                pop edi
                pop esi
                pop ebx
            }
            if (b_ebx != 0x11111111 || b_esi != 0x22222222 || b_edi != 0x33333333 || b_ecx != 0x55555555) regs_ok = 0;
            if (((sw0 >> 11) & 7) != ((sw1 >> 11) & 7)) depth_ok = 0;   // fld + stub pop: TOP back where it was
            sum += r; exact += (double)(float)(g * 1000.0f);
        }
        _controlfp_s(&cw, _PC_53, _MCW_PC);
        CHECK(regs_ok); CHECK(depth_ok);
        printf("6. ftol stub: credited %d ms for %.3f exact ms (regs %s, x87 stack %s)\n", sum, exact, regs_ok ? "kept" : "CLOBBERED", depth_ok ? "balanced" : "UNBALANCED");
        CHECK(sum >= (int)floor(exact) - 1 && sum <= (int)floor(exact) + 1);
        V arm = (V)ts("arm_dive"); volatile BYTE* sup = (volatile BYTE*)ts("suppress_dive");
        CHECK(arm && sup);
        if (arm && sup) {
            BYTE old = *sup; *sup = 1;
            DWORD esp0 = 0, esp1 = 0, rv = 1;
            __asm {
                mov esp0, esp
                push 0
                push -12
                mov ecx, 0x12345678
                call arm
                mov rv, eax
                mov esp1, esp
            }
            *sup = old;
            CHECK(rv == 0 && esp0 == esp1);
            printf("   arm stub (suppressed): eax %u, stack %s\n", rv, esp0 == esp1 ? "balanced (ret 8)" : "UNBALANCED");
        }
    }

    // ---- 8. the Suave use-power-up gate stub (slot 11), its exits redirected to host functions
    typedef void(__cdecl* SvRedir_t)(DWORD, DWORD, DWORD); typedef void(__cdecl* SvForce_t)(int); typedef int(__cdecl* SvInt_t)();
    {
        void* gate = ts("use_gate"); SvRedir_t redir = (SvRedir_t)ts("use_gate_redirect");
        SvForce_t force = (SvForce_t)ts("suave_force_running"), fgate = (SvForce_t)ts("use_gate_force");
        SvInt_t inst = (SvInt_t)ts("suave_installed"), slot = (SvInt_t)ts("suave_slot");
        CHECK(gate && redir && force && fgate && inst && slot);
        CHECK(inst && inst() == 1 && slot && slot() == 11);   // installed by the all-on ini, default slot 11
        if (gate && redir && force && fgate) {
            redir((DWORD)(uintptr_t)&HostTouch, (DWORD)(uintptr_t)&HostExit, (DWORD)(uintptr_t)&HostGeneric);
            static DWORD pdata[16]; memset(pdata, 0, sizeof pdata);
            DWORD pd = (DWORD)(uintptr_t)pdata, regs = 0;
            pdata[0x1C / 4] = 9;  CHECK(gate_call(gate, pd, &regs) == 1 && regs);    // El Mariachi: noted, game path
            pdata[0x1C / 4] = 11; CHECK(gate_call(gate, pd, &regs) == 1 && regs);    // no charge: the game exits itself
            pdata[0x1C / 4] = 10; CHECK(gate_call(gate, pd, &regs) == 1 && regs);    // Pinata untouched with slot 11
            force(1); pdata[0x1C / 4] = 3; CHECK(gate_call(gate, pd, &regs) == 0 && g_exit_esp == g_saved_esp); force(0);
            fgate(2); CHECK(gate_call(gate, pd, &regs) == 2 && g_touch_hit == 1 && g_touch_arg == 4 && g_exit_esp == g_saved_esp); fgate(-1);
            printf("8. Suave gate (slot 11): slots 9/10/11 -> game path (registers kept), any slot while Suave runs -> common exit, approved slot-10 path -> touch then the generic branch (stacks %s)\n",
                   g_exit_esp == g_saved_esp ? "balanced" : "UNBALANCED");
        }
    }
    // ---- 9. Smart AI: the init wrapper and the tick on fake enemies (the original init, touch and position calls
    //         redirected to host functions; the script receiver globals made writable for the test)
    {
        typedef void(__cdecl* Wrap_t)(void*, int*); typedef void(__cdecl* Redir3_t)(DWORD, DWORD, DWORD);
        typedef void(__cdecl* Env_t)(int, uint32_t, int, int); typedef void(__cdecl* Tick_t)(double); typedef int(__cdecl* Inst_t)();
        typedef int(__cdecl* Entry_t)(DWORD, SmTestEnt*);
        Wrap_t wrap = (Wrap_t)ts("smart_init_wrap"); Redir3_t redir = (Redir3_t)ts("smart_redirect"); Env_t env = (Env_t)ts("smart_env");
        Tick_t tick = (Tick_t)ts("smart_tick"); Inst_t inst = (Inst_t)ts("smart_installed"); Entry_t entry = (Entry_t)ts("smart_entry");
        CHECK(wrap && redir && env && tick && inst && entry);
        CHECK(inst && inst() == 1);
        if (wrap && redir && env && tick && inst && entry) {
            DWORD op; VirtualProtect((void*)(uintptr_t)0xA3B000, 0x1000, PAGE_READWRITE, &op);
            redir((DWORD)(uintptr_t)&HostSmInit, (DWORD)(uintptr_t)&HostSmTouch, (DWORD)(uintptr_t)&HostSmPos);
            env(1, 1000, 5, 1);
            FakeEnemy* fe = g_fe;
            const float dist[6] = {10, 11, 12, 13, 14, 12.5f};
            const float posx[6] = {0, 4, 8, 12, 100, 6};
            for (int k = 0; k < 7; ++k) fake_enemy(fe[k], k < 6 ? posx[k] : 0.0f);
            memset(&fe[7], 0, sizeof fe[7]); memset(&fe[8], 0, sizeof fe[8]);   // not in the world yet
            fe[5].data[0xB8 / 4] = 1;                        // a buddy: not managed
            fe[6].cls.handler = 0x5C3060;                    // not a sentry (character_root class): ignored
            int changed_any = 0, buddy_writes = 0, roles[4] = {0, 0, 0, 0};
            for (int k = 0; k < 7; ++k) {
                g_sm_touch_n = 0; g_sm_init_calls = 0;
                *(volatile DWORD*)(uintptr_t)0xA3B75C = (DWORD)(uintptr_t)&fe[k].node;
                int out = 0; DWORD esp0 = 0, esp1 = 0;
                __asm mov esp0, esp
                wrap((void*)0x7777, &out);
                __asm mov esp1, esp
                CHECK(esp0 == esp1 && out == 1 && g_sm_init_calls == 1 && g_sm_init_thread == 0x7777);
                SmTestEnt te; memset(&te, 0, sizeof te);
                int reg = entry((DWORD)(uintptr_t)&fe[k].node, &te);
                if (k < 5) {
                    CHECK(reg == 1 && te.role >= 1 && te.role <= 3 && te.sq_ok && te.mo_ok && te.react_set);
                    roles[te.role]++;
                    float stop = fe[k].f(0xFC), retreat = fe[k].f(0x48), react = fe[k].f(0xCC);
                    CHECK(stop >= 7.0f - 1e-4f && stop <= 13.5f + 1e-4f);
                    CHECK(retreat <= stop * 0.6f + 1e-4f && react >= 0.05f && react <= 1.0f);
                    CHECK(fe[k].f(0x68) == 0.0f && fe[k].f(0x6C) == 0.0f);   // CS untouched at spawn
                    // every write was preceded by the game's touch with the matching slot, on this node
                    int bad_touch = 0;
                    for (int t = 0; t < g_sm_touch_n; ++t) if (g_sm_touch[t].node != (DWORD)(uintptr_t)&fe[k].node || g_sm_touch[t].slot > 0x36) ++bad_touch;
                    CHECK(bad_touch == 0 && g_sm_touch_n >= 3);
                    changed_any += g_sm_touch_n;
                } else if (k == 5) { CHECK(reg == 1 && !te.sq_ok && !te.mo_ok && te.role == 0); buddy_writes = g_sm_touch_n; }
                else CHECK(reg == 0 && g_sm_touch_n == 0);
            }
            CHECK(buddy_writes == 0);
            // tick 1: all five fight one target at 10..14 m: two keep the token, three hold (DontHunt 1); the
            // reaction delays are cleared (state 20 is past state 14)
            for (int k = 0; k < 6; ++k) fe[k].fight(20, 0x1234, dist[k]);
            g_sm_touch_n = 0;
            tick(1.0);
            int holders = 0, dh_touch = 0;
            for (int k = 0; k < 5; ++k) { holders += fe[k].data[0xF0 / 4] == 1; CHECK(fe[k].f(0xCC) == 0.0f); }
            for (int t = 0; t < g_sm_touch_n; ++t) dh_touch += g_sm_touch[t].slot == 0x33;
            CHECK(holders == 3 && dh_touch == 3 && fe[5].data[0xF0 / 4] == 0);
            CHECK(fe[0].data[0xF0 / 4] == 0 && fe[1].data[0xF0 / 4] == 0);   // the two closest advance
            // tick 2 (too soon: 100 ms period) does nothing; paused puts the three holds back once, then nothing
            g_sm_touch_n = 0; tick(1.05); CHECK(g_sm_touch_n == 0);
            env(1, 1100, 5, 2); fe[0].data[0x1D4 / 4] = 28; tick(1.2);
            { int h = 0; for (int k = 0; k < 5; ++k) h += fe[k].data[0xF0 / 4] == 1; CHECK(g_sm_touch_n == 3 && h == 0); }
            g_sm_touch_n = 0; tick(1.3); CHECK(g_sm_touch_n == 0);
            // tick 3: enemy 0 died at x=0: enemies within 15 m (x 4, 8, 12) are afraid, the one at x 100 is not
            env(1, 1200, 5, 1); g_sm_touch_n = 0; tick(1.4);
            SmTestEnt te; memset(&te, 0, sizeof te);
            CHECK(entry((DWORD)(uintptr_t)&fe[0].node, &te) == 1 && te.dead);
            for (int k = 1; k <= 3; ++k) { CHECK(fe[k].f(0x68) >= 20.0f && fe[k].f(0x6C) >= 6.0f); }
            CHECK(fe[4].f(0x68) == 0.0f && fe[4].f(0x6C) == 0.0f && fe[5].f(0x68) == 0.0f);
            // a foreign change (the game set DontHunt on enemy 2 itself): the squad leaves that enemy alone
            int dh2 = fe[2].data[0xF0 / 4]; fe[2].data[0xF0 / 4] = dh2 ? 0 : 1;
            tick(1.6);
            memset(&te, 0, sizeof te); CHECK(entry((DWORD)(uintptr_t)&fe[2].node, &te) == 1 && !te.sq_ok);
            CHECK(fe[2].data[0xF0 / 4] == (dh2 ? 0 : 1));
            // fear fades with game time: 25 s later the CS values are the row's own again (exactly)
            for (int s = 1; s <= 30; ++s) { env(1, 1200 + s * 1000, 5, 1); tick(1.6 + s * 0.2); }   // (the tick takes at most 1 s of game time per step)
            env(1, 26200 + 5000, 5, 1);
            for (int k = 1; k <= 3; ++k) CHECK(fe[k].f(0x68) == 0.0f && fe[k].f(0x6C) == 0.0f);
            // the fight ends (passive): every DontHunt the squad wrote is put back
            for (int k = 1; k < 5; ++k) fe[k].data[0x1D4 / 4] = 4;
            tick(8.0);
            for (int k = 1; k < 5; ++k) if (k != 2) CHECK(fe[k].data[0xF0 / 4] == 0);
            // discovery: two enemies the game restored from a save (no init): one still passive, one already fighting
            {
                typedef void(__cdecl* RQ_t)(DWORD, DWORD); typedef DWORD(__cdecl* Cnt_t)(int);
                RQ_t rq = (RQ_t)ts("smart_redirect_query"); Cnt_t cnt = (Cnt_t)ts("smart_counters");
                CHECK(rq && cnt);
                DWORD keep[3] = {*(DWORD*)(uintptr_t)0xA3B578, *(DWORD*)(uintptr_t)0xA3B8A8, *(DWORD*)(uintptr_t)0xA3B590};
                g_sm_list_type[0x24 / 4] = 1;
                g_sm_holder[0x18 / 4] = (DWORD)(uintptr_t)g_sm_holder_data; g_sm_holder_data[0x84 / 4] = (DWORD)(uintptr_t)g_sm_ram;
                *(DWORD*)(uintptr_t)0xA3B578 = 0x5150; *(DWORD*)(uintptr_t)0xA3B8A8 = (DWORD)(uintptr_t)g_sm_list_type;
                *(DWORD*)(uintptr_t)0xA3B590 = (DWORD)(uintptr_t)g_sm_holder;
                if (rq && cnt) {
                    rq((DWORD)(uintptr_t)&HostSmQuery, (DWORD)(uintptr_t)&HostSmFree);
                    fake_enemy(fe[7], 3.0f); fake_enemy(fe[8], 5.0f);
                    fe[8].fight(13, 0x1234, 11.0f);
                    DWORD found0 = cnt(0);
                    env(1, 31250, 5, 1); tick(10.0);
                    SmTestEnt a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
                    CHECK(entry((DWORD)(uintptr_t)&fe[7].node, &a) == 1 && entry((DWORD)(uintptr_t)&fe[8].node, &b) == 1);
                    CHECK(a.react_set && !b.react_set && a.role >= 1 && b.role >= 1 && fe[8].f(0xCC) == 0.0f);
                    CHECK(cnt(0) - found0 == 2 && g_sm_query_n >= 1 && g_sm_free_n == g_sm_query_n && g_sm_query_r == 60.0f);
                    tick(10.3); CHECK(cnt(0) - found0 == 2);   // known ones are not adopted twice
                    // pause releases what the squad wrote (a save made in the menu holds the game's values), resume re-applies
                    fe[1].fight(20, 0x1234, 10); fe[3].fight(20, 0x1234, 12); fe[4].fight(20, 0x1234, 13); fe[7].fight(20, 0x1234, 14); fe[8].fight(20, 0x1234, 15);
                    env(1, 31350, 5, 1); tick(10.5);
                    int h1 = 0; for (int k : {1, 3, 4, 7, 8}) h1 += fe[k].data[0xF0 / 4] == 1;
                    env(1, 31350, 5, 2); tick(10.7);
                    int h2 = 0; for (int k : {1, 3, 4, 7, 8}) h2 += fe[k].data[0xF0 / 4] == 1;
                    env(1, 31450, 5, 1); tick(10.9);
                    int h3 = 0; for (int k : {1, 3, 4, 7, 8}) h3 += fe[k].data[0xF0 / 4] == 1;
                    CHECK(h1 == 3 && h2 == 0 && h3 == 3);
                    printf("   discovery: 2 save-restored enemies adopted (reaction delay only for the passive one); pause released %d holds, resume re-applied %d\n", h1, h3);
                    rq(0x486950, 0x8718D0);
                }
                *(DWORD*)(uintptr_t)0xA3B578 = keep[0]; *(DWORD*)(uintptr_t)0xA3B8A8 = keep[1]; *(DWORD*)(uintptr_t)0xA3B590 = keep[2];
            }
            // a new mission: the registry is forgotten without writes
            env(1, 31300, 6, 1); g_sm_touch_n = 0; tick(8.2);
            CHECK(entry((DWORD)(uintptr_t)&fe[1].node, &te) == 0 && g_sm_touch_n == 0);
            env(0, 0, 0, 1);
            VirtualProtect((void*)(uintptr_t)0xA3B000, 0x1000, op, &op);
            printf("9. Smart AI: 5 enemies got personalities (assault %d, rifleman %d, cautious %d; %d touched writes), buddy and non-sentry left alone; "
                   "squad 3 holding / 2 advancing; one death frightened 3 of 4; foreign change respected; fear faded to the row values; mission change forgot all\n",
                   roles[1], roles[2], roles[3], changed_any);
        }
    }

    // ---- 4. fresh image + reloaded DLL, only clock_fix
    unload_dll(); CHECK(map_image()); CHECK(diff_count(&dva) == 0);
    CHECK(load_dll());
    ti = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
    n = ti("simulated-image-not-the-game", INI_CLOCK);
    CHECK(n == 5);   // the 3 clock sites + [fixes] vector_realloc + weapon_owner (installed with any feature)
    d = diff_count(&dva);
    CHECK(d > 0 && d <= 30);   // 3 clock calls + 2 fix sites (5 + 6 bytes)
    printf("4. clock_fix only: %u patches, %d bytes differ\n", n, d);

    // ---- 5. a changed site is skipped, the rest installs
    unload_dll(); CHECK(map_image());
    {
        DWORD op; VirtualProtect((void*)(uintptr_t)0x87DE40, 1, PAGE_READWRITE, &op);
        *(BYTE*)(uintptr_t)0x87DE40 = 0xE9;   // "another mod" hooked the camera entry
        VirtualProtect((void*)(uintptr_t)0x87DE40, 1, op, &op);
    }
    CHECK(load_dll());
    ti = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
    n = ti("simulated-image-not-the-game", INI_ALL);
    printf("5. camera entry pre-hooked: %u patches (all but the 4 camera sites)\n", n);
    CHECK(n == (DWORD)NS - 4);
    CHECK(*(BYTE*)(uintptr_t)0x87DE5F == 0x8B);   // CAM_VIEW untouched
    unload_dll();

    // ---- 10. Smart AI switches: master off = nothing; personality alone = the init call site + the tick (it adopts
    //          enemies restored from a save)
    {
        static const char* INI_SM_OFF = "[smart_ai]\nenabled = 0\npersonality = 1\nsquad = 1\nmorale = 1\n";
        static const char* INI_SM_PERS = "[smart_ai]\nenabled = 1\nsquad = 0\nmorale = 0\nreaction_stagger = 0\n";
        static const char* INI_SM_ALL = "[smart_ai]\nenabled = 1\n";
        DWORD got[3]; const char* inis[3] = {INI_SM_OFF, INI_SM_PERS, INI_SM_ALL};
        for (int k = 0; k < 3; ++k) {
            CHECK(map_image()); CHECK(load_dll());
            ti = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
            got[k] = ti("simulated-image-not-the-game", inis[k]);
            if (k == 0) CHECK(diff_count(&dva) == 0);
            if (k == 1) CHECK(*(BYTE*)(uintptr_t)0x5E7C28 == 0xE8 && *(DWORD*)(uintptr_t)0x5E7C29 != 0xFFFEC563);   // retargeted
            unload_dll();
        }
        CHECK(got[0] == 0 && got[1] == 6 && got[2] == 6);   // + [fixes] vector_realloc, weapon_owner
        printf("10. Smart AI switches: master off %u patches, personality only %u, everything %u (the init call + post-input + 2 input-tick calls)\n", got[0], got[1], got[2]);
    }

    // ---- 11. seamless: switches, the patched sites, and one walk-border swap driven through every stub
    {
        static const char* INI_SL_NOPF = "[seamless]\nenabled = 1\nprefetch = 0\n";
        static const char* INI_SL = "[seamless]\nenabled = 1\n";
        static const char* INI_SL_SM = "[seamless]\nenabled = 1\n[smart_ai]\nenabled = 1\n";
        static const char* INI_SL_OFF = "[seamless]\nenabled = 0\nprefetch = 1\n";
        DWORD got[4]; const char* inis[4] = {INI_SL_OFF, INI_SL_NOPF, INI_SL, INI_SL_SM};
        for (int k = 0; k < 4; ++k) {
            CHECK(map_image()); CHECK(load_dll());
            ti = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
            got[k] = ti("simulated-image-not-the-game", inis[k]);
            if (k == 0) CHECK(diff_count(&dva) == 0);
            if (k == 1) {
                CHECK(*(BYTE*)(uintptr_t)0x7489A1 == 0xE9 && *(BYTE*)(uintptr_t)0x747CA5 == 0xE9 && *(BYTE*)(uintptr_t)0x747CAA == 0x90);
                CHECK(*(BYTE*)(uintptr_t)0x748D0E == 0xE9 && *(BYTE*)(uintptr_t)0x748D13 == 0x90 && *(BYTE*)(uintptr_t)0x748D16 == 0x90);
                CHECK(in_dll(*(DWORD*)(uintptr_t)0x9CC4F4));
                CHECK(*(BYTE*)(uintptr_t)0x43C516 == 0xE8 && *(DWORD*)(uintptr_t)0x43C517 == 0xFFFFDC75);   // POST_INPUT untouched
            }
            if (k < 3) unload_dll();
        }
        CHECK(got[0] == 0 && got[1] == 11 && got[2] == 14 && got[3] == 15);   // + [fixes] vector_realloc, weapon_owner
        printf("11. seamless switches: off %u patches, without prefetch %u (9 sites + the fix), with prefetch %u (+ post-input + 2 input-tick calls), "
               "with Smart AI %u (shared tick sites)\n", got[0], got[1], got[2], got[3]);
        // the DLL of the last install (seamless + Smart AI) stays loaded: drive one swap through the stubs
        typedef void(__cdecl* Redir_t)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
        typedef void(__cdecl* Redir2_t)(DWORD, DWORD); typedef int(__cdecl* State_t)(int);
        Redir_t redir = (Redir_t)ts_of(g_dll, "seam_redirect"); Redir2_t redir2 = (Redir2_t)ts_of(g_dll, "seam_redirect2");
        State_t state = (State_t)ts_of(g_dll, "seam_state");
        void* walk = ts_of(g_dll, "seam_walkreq"); void* lspush = ts_of(g_dll, "seam_lspush"); void* lsskip = ts_of(g_dll, "seam_lsskip");
        void* prog = ts_of(g_dll, "seam_progress"); void* swap = ts_of(g_dll, "seam_swap"); void* postfade = ts_of(g_dll, "seam_postfade");
        void* fadein = ts_of(g_dll, "seam_fadein"); void* lsskip2 = ts_of(g_dll, "seam_lsskip2");
        typedef void(__cdecl* Redir3_t)(DWORD, DWORD, DWORD); Redir3_t redir3 = (Redir3_t)ts_of(g_dll, "seam_redirect3");
        CHECK(redir && redir2 && redir3 && state && walk && lspush && lsskip && lsskip2 && prog && swap && postfade && fadein);
        if (redir && redir2 && redir3 && state && walk && lspush && lsskip && lsskip2 && prog && swap && postfade && fadein) {
            CHECK(state(0) == 1);   // installed
            redir((DWORD)(uintptr_t)&HostSlSendCmd, (DWORD)(uintptr_t)&HostSlCoPush, (DWORD)(uintptr_t)&HostSlLsExit,
                  (DWORD)(uintptr_t)&HostSlLsBack, (DWORD)(uintptr_t)&HostSlProg, (DWORD)(uintptr_t)&HostSlPostHud,
                  (DWORD)(uintptr_t)&HostSlPostBack, (DWORD)(uintptr_t)&HostSlSendMsg, (DWORD)(uintptr_t)&HostSlSwap);
            redir2((DWORD)(uintptr_t)&HostSlGetProp, (DWORD)(uintptr_t)&HostSlGetPos);
            redir3((DWORD)(uintptr_t)&HostSlLsText, (DWORD)(uintptr_t)&HostSlLs2Back, (DWORD)(uintptr_t)&HostTouch);
            DWORD op; VirtualProtect((void*)(uintptr_t)0xA3B000, 0x1000, PAGE_READWRITE, &op);
            DWORD keep = *(DWORD*)(uintptr_t)0xA3B8F0;
            *(DWORD*)(uintptr_t)0xA3B8F0 = (DWORD)(uintptr_t)g_sl_type; g_sl_type[0x24 / 4] = 1;
            g_sl_node[0x18 / 4] = (DWORD)(uintptr_t)g_sl_data; g_sl_data[0x0C / 4] = (DWORD)(uintptr_t)g_sl_list;
            g_sl_list[1] = (DWORD)(uintptr_t)g_sl_arr; g_sl_arr[1] = (DWORD)(uintptr_t)g_sl_el; g_sl_list[2] = 0;
            static const char empty[1] = "", tele[] = "Teleport_Bridge_Downtown";
            for (int i = 0; i < 4; ++i) {
                g_sl_entry[i][0] = (DWORD)(uintptr_t)g_sl_folder[i]; g_sl_entry[i][1] = (DWORD)(uintptr_t)(i == 3 ? tele : empty);
                g_sl_folder[i][0x50 / 4] = i == 0 ? 1u : 2u;              // map, submap, submap, submap
            }
            g_sl_action[0x24 / 4] = (DWORD)(uintptr_t)g_sl_folder[1];      // TargetEntity
            DWORD node = (DWORD)(uintptr_t)g_sl_node, act = (DWORD)(uintptr_t)g_sl_action;
            // a) two walk actions (MAINMAP + SUBMAP) ask LoadCtrl: the original call runs, two entries are tagged
            static DWORD args[1] = {0x1234};
            SlRegs r1 = sl_call(walk, node, 0x11, act, 2, 0x273, (DWORD)(uintptr_t)args);
            SlRegs r2 = sl_call(walk, node, 0x11, act, 2, 0x273, (DWORD)(uintptr_t)args);
            CHECK(r1.esp_delta == 0 && r1.regs_ok && r2.esp_delta == 0 && r2.regs_ok);
            CHECK(g_sl_cmd_id == 0x273 && g_sl_cmd_ecx == node && g_sl_list[2] == 2 && state(7) == 2 && state(8) == 1);
            // b) the processing state pushes the load-screen state: a seamless batch, then the original push
            g_sl_copush = 0;
            SlRegs r3 = sl_call(lspush, 0x99, node, 0x22, 0, 0, 0);
            CHECK(r3.esp_delta == (DWORD)-16 && g_sl_copush == 1 && state(1) == 1 && state(3) == 1 && state(7) == 0);
            // c) the load-screen state reaches its fade-out: jumps past it to the text / sound-shutdown messages, then past the
            //    load-screen choice to the exit; the progress bar draws nothing
            g_sl_hit = 0; SlRegs r4 = sl_call(lsskip, 0, 0x33, 0x44, 0, 0, 0);
            CHECK(r4.esp_delta == 0 && g_sl_hit == 5 && state(2) == 1 && state(1) == 0);
            g_sl_hit = 0; g_touch_hit = 0; SlRegs r4b = sl_call(lsskip2, 0, 0x33, 0x44, 0, 0, 0);
            CHECK(r4b.esp_delta == 0 && g_sl_hit == 1 && g_touch_hit == 0);
            g_sl_prog = 0; SlRegs r5 = sl_call(prog, 0x55, 0, 0, 2, 0, 1);
            CHECK(r5.esp_delta == 0 && g_sl_prog == 0);
            // d) the swap, timed (30 ms stand-in)
            g_sl_swap_ecx = 0; SlRegs r6 = sl_call(swap, 0x5CE7E, 0, 0, 2, 1, 0xE7);
            CHECK(r6.esp_delta == 0 && r6.regs_ok && g_sl_swap_ecx == 0x5CE7E && state(5) >= 25 && state(5) < 1000 && state(6) == 1);
            // e) post-load: the black fade node path is skipped, the fade-in message is not sent; the swap ends
            g_sl_hit = 0; SlRegs r7 = sl_call(postfade, 0, 0, 0, 0, 0, 0);
            CHECK(r7.esp_delta == 0 && g_sl_hit == 3);
            g_sl_msg = 0; SlRegs r8 = sl_call(fadein, 0x66, 0, 0, 2, (DWORD)-5, 0);
            CHECK(r8.esp_delta == 0 && r8.eax == 0 && g_sl_msg == 0 && state(4) == 1 && state(2) == 0 && state(3) == 0);
            // f) afterwards every stub takes the vanilla path
            g_sl_prog = 0; sl_call(prog, 0x55, 0, 0, 2, 0, 1); CHECK(g_sl_prog == 1);
            g_sl_hit = 0; sl_call(postfade, 0, 0, 0, 0, 0, 0); CHECK(g_sl_hit == 4);
            g_sl_msg = 0; SlRegs r9 = sl_call(fadein, 0x66, 0, 0, 2, (DWORD)-5, 0); CHECK(r9.esp_delta == 0 && g_sl_msg == 1);
            g_sl_hit = 0; sl_call(lsskip, 0, 0, 0, 0, 0, 0); CHECK(g_sl_hit == 2);
            g_sl_hit = 0; g_touch_hit = 0; g_touch_arg = 1; SlRegs r10 = sl_call(lsskip2, 0, 0, 0, 0, 0, 0);
            CHECK(r10.esp_delta == 0 && r10.regs_ok && g_sl_hit == 6 && g_touch_hit == 1 && g_touch_arg == 0 && g_touch_ecx == 0x7E7E7E7E);   // push 0; mov ecx,ebp; call touch
            // g) a batch with a teleport entry (a taxi / mission-style request nobody walk-tagged) is a normal load
            g_sl_el[g_sl_list[2]++] = (DWORD)(uintptr_t)g_sl_entry[3];
            sl_call(lspush, 0x99, node, 0x22, 0, 0, 0);
            CHECK(state(1) == 0 && state(3) == 0);
            g_sl_hit = 0; sl_call(lsskip, 0, 0, 0, 0, 0, 0); CHECK(g_sl_hit == 2);
            g_sl_prog = 0; sl_call(prog, 0x55, 0, 0, 2, 0, 1); CHECK(g_sl_prog == 1);
            sl_call(swap, 0x5CE7E, 0, 0, 2, 1, 0xE7); CHECK(state(6) == 2);
            *(DWORD*)(uintptr_t)0xA3B8F0 = keep;
            VirtualProtect((void*)(uintptr_t)0xA3B000, 0x1000, op, &op);
            printf("    one walk-border swap through every stub: 2 entries tagged, batch seamless, load screen skipped, progress held, "
                   "swap timed %d ms, fade node + fade-in skipped, then vanilla paths again; a teleport batch stays normal\n", state(5));
        }
        unload_dll();
    }

    // ---- 8b. Suave with [suave] slot = 10 (the Pinata) on a fresh image: only the Suave sites and the input tick
    unload_dll(); CHECK(map_image()); CHECK(diff_count(&dva) == 0);
    CHECK(load_dll());
    {
        TestInstall_t ti10 = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t ts10 = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        DWORD n10 = ti10("simulated-image-not-the-game", "[general]\nlog = 1\n[suave]\nenabled = 1\nslot = 10\ncheat_key = F7\nlog_verbose = 1\n");
        printf("8b. Suave slot 10: %u patches (input tick + 3 gates)\n", n10);
        CHECK(n10 == 8);   // + [fixes] vector_realloc, weapon_owner
        void* gate = ts10("use_gate"); SvRedir_t redir = (SvRedir_t)ts10("use_gate_redirect");
        SvForce_t force = (SvForce_t)ts10("suave_force_running"), fgate = (SvForce_t)ts10("use_gate_force");
        SvInt_t inst = (SvInt_t)ts10("suave_installed"), slot = (SvInt_t)ts10("suave_slot");
        CHECK(gate && redir && force && fgate && inst && slot && inst() == 1 && slot() == 10);
        if (gate && redir && force && fgate) {
            redir((DWORD)(uintptr_t)&HostTouch, (DWORD)(uintptr_t)&HostExit, (DWORD)(uintptr_t)&HostGeneric);
            static DWORD pdata[16]; memset(pdata, 0, sizeof pdata);
            DWORD pd = (DWORD)(uintptr_t)pdata, regs = 0;
            pdata[0x1C / 4] = 10; CHECK(gate_call(gate, pd, &regs) == 1 && regs);    // no charge: the game's own branch exits
            pdata[0x1C / 4] = 11; CHECK(gate_call(gate, pd, &regs) == 1 && regs);    // Tequila is vanilla with slot 10
            force(1); CHECK(gate_call(gate, pd, &regs) == 0 && g_exit_esp == g_saved_esp); force(0);
            fgate(2); pdata[0x1C / 4] = 10;
            CHECK(gate_call(gate, pd, &regs) == 2 && g_touch_hit == 1 && g_touch_ecx == 0x0BADC0DE && g_exit_esp == g_saved_esp); fgate(-1);
            printf("    slot 10 -> game path without a charge, slot 11 vanilla, refusal -> common exit, approval -> touch + generic branch (decrement, no Pinata)\n");
        }
    }
    unload_dll();
    // ---- 12. [fixes] vector_realloc: the array resize's second free of a CRT-reallocated buffer is skipped
    unload_dll(); CHECK(map_image()); CHECK(diff_count(&dva) == 0);
    CHECK(load_dll());
    {
        TestInstall_t tif = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t tsf = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        DWORD nf = tif("simulated-image-not-the-game", "[timing]\nclock_fix = 1\n[fixes]\nvector_realloc = 0\nweapon_owner = 0\n");
        CHECK(nf == 3 && *(DWORD*)(uintptr_t)0x40638D == 0x0007153F);   // switched off: the site keeps its call
        CHECK(*(WORD*)(uintptr_t)0x8418D4 == 0x8E8B && *(DWORD*)(uintptr_t)0x8418D6 == 0x94);   // ... and FIX_WPN_OWNER its mov
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tif = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall"); tsf = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        nf = tif("simulated-image-not-the-game", "[timing]\nclock_fix = 1\n");
        CHECK(nf == 5 && *(BYTE*)(uintptr_t)0x40638C == 0xE8 && in_dll(0x406391 + *(DWORD*)(uintptr_t)0x40638D));
        CHECK(*(BYTE*)(uintptr_t)0x8418D4 == 0xE9 && in_dll(0x8418D9 + *(DWORD*)(uintptr_t)0x8418D5) && *(BYTE*)(uintptr_t)0x8418D9 == 0x90);
        typedef void(__cdecl* FixRedir_t)(DWORD, DWORD);
        typedef LONG(__cdecl* FixCount_t)();
        void* stub = tsf("fix_vec_free"); FixRedir_t redir = (FixRedir_t)tsf("fix_redirect"); FixCount_t skipped = (FixCount_t)tsf("fix_skipped");
        CHECK(stub && redir && skipped);
        if (stub && redir && skipped) {
            static DWORD vt_crt[8], vt_pool[8], obj_def[4], obj_pool[4];
            vt_crt[6] = 0x13579BDF; vt_pool[6] = 0x2468ACE0;   // +18h realloc: the "CRT realloc" marker / a pool's
            obj_def[0] = (DWORD)(uintptr_t)vt_crt; obj_pool[0] = (DWORD)(uintptr_t)vt_pool;
            redir((DWORD)(uintptr_t)&HostFixFree, 0x13579BDF);
            DWORD op; VirtualProtect((void*)(uintptr_t)0xA3A000, 0x2000, PAGE_READWRITE, &op);
            BYTE keep[0x120]; memcpy(keep, (void*)(uintptr_t)0xA3AFA0, sizeof keep);
            InitializeCriticalSection((CRITICAL_SECTION*)(uintptr_t)0xA3AFA0);
            *(BYTE*)(uintptr_t)0xA3AFBC = 0;
            *(DWORD*)(uintptr_t)0xA3B098 = 2;                                   // two ranges
            *(DWORD*)(uintptr_t)0xA3AFE8 = 0;          *(DWORD*)(uintptr_t)0xA3AFEC = (DWORD)(uintptr_t)obj_def;    // [0, 5000_0000): DEFAULT
            *(DWORD*)(uintptr_t)0xA3AFF0 = 0x50000000; *(DWORD*)(uintptr_t)0xA3AFF4 = (DWORD)(uintptr_t)obj_pool;   // [5000_0000, ..): a pool
            struct Case { DWORD old_buf, new_buf; bool freed; } cases[] = {
                {0x0F488940, 0x0F48B138, false},   // the crash: CRT block moved by the realloc -> not freed again
                {0x0F488940, 0, true},             // realloc failed: the old block is still the array's -> freed
                {0x50001000, 0x50002000, true},    // a pool block: vanilla free
                {0x50001000, 0, true},
            };
            int ok = 0;
            for (const Case& c : cases) {
                g_fix_freed = 0; DWORD esi_after = 0, edi_after = 0, ebx_after = 0, esp_before = 0, esp_after = 0;
                DWORD ob = c.old_buf, nb = c.new_buf;
                __asm {
                    push esi
                    push edi
                    push ebx
                    mov esi, nb
                    mov edi, 0x11112222
                    mov ebx, 0x33334444
                    mov esp_before, esp
                    push 0
                    push 0
                    push ob
                    call stub
                    add esp, 0xC
                    mov esp_after, esp
                    mov esi_after, esi
                    mov edi_after, edi
                    mov ebx_after, ebx
                    pop ebx
                    pop edi
                    pop esi
                }
                bool freed = g_fix_freed == ob;
                if (freed == c.freed && esi_after == nb && edi_after == 0x11112222 && ebx_after == 0x33334444 && esp_before == esp_after) ++ok;
                else printf("    case old %08X new %08X: freed %d (want %d)\n", ob, nb, freed, c.freed);
            }
            CHECK(ok == 4 && skipped() == 1);
            DeleteCriticalSection((CRITICAL_SECTION*)(uintptr_t)0xA3AFA0);
            memcpy((void*)(uintptr_t)0xA3AFA0, keep, sizeof keep);
            VirtualProtect((void*)(uintptr_t)0xA3A000, 0x2000, op, &op);
            printf("12. fixes: vector_realloc + weapon_owner off = 3 patches (sites untouched), on = 5; stub: CRT-moved block not freed twice, failed realloc / pool blocks freed as before (%d/4, registers and stack kept)\n", ok);
        }
        // 12b. weapon_owner: the fire shell's data with pOwner 0 -> the skip target (no reload request), with an owner ->
        //      the original mov (ecx = owner) and the continuation; registers and stack kept
        {
            typedef void(__cdecl* WoRedir_t)(DWORD, DWORD); typedef LONG(__cdecl* WoCount_t)();
            void* wst = tsf("fix_weapon_owner"); WoRedir_t wredir = (WoRedir_t)tsf("fix_wo_redirect"); WoCount_t wskips = (WoCount_t)tsf("fix_wo_skips");
            CHECK(wst && wredir && wskips);
            if (wst && wredir && wskips) {
                wredir((DWORD)(uintptr_t)&HostWoCont, (DWORD)(uintptr_t)&HostWoSkip);
                static DWORD wdata[0x40]; memset(wdata, 0, sizeof wdata);
                int okw = 0;
                for (int k = 0; k < 2; ++k) {
                    wdata[0x94 / 4] = k ? 0x0BADF00D : 0;
                    g_wo_hit = 0; g_wo_ecx = 0;
                    DWORD wd = (DWORD)(uintptr_t)wdata, esi_a = 0, edi_a = 0, ebx_a = 0, sp0 = 0, sp1 = 0;
                    __asm {
                        push esi
                        push edi
                        push ebx
                        mov esi, wd
                        mov edi, 0x5555AAAA
                        mov ebx, 0x0E7E0E7E
                        mov ecx, 0xCCCCCCCC
                        mov sp0, esp
                        call wst
                        mov sp1, esp
                        mov esi_a, esi
                        mov edi_a, edi
                        mov ebx_a, ebx
                        pop ebx
                        pop edi
                        pop esi
                    }
                    bool regs = esi_a == wd && edi_a == 0x5555AAAA && ebx_a == 0x0E7E0E7E && sp0 == sp1;
                    if (k == 0 && g_wo_hit == 2 && regs) ++okw;                                  // no owner: skipped
                    if (k == 1 && g_wo_hit == 1 && g_wo_ecx == 0x0BADF00D && regs) ++okw;        // owner: the mov, then on
                    if (!regs) printf("    weapon_owner case %d: registers / stack changed\n", k);
                }
                CHECK(okw == 2 && wskips() == 1);
                printf("12b. fixes: weapon_owner stub: pOwner 0 -> skip target (logged), owner -> ecx = owner + continuation (%d/2, registers and stack kept)\n", okw);
            }
        }
    }
    // ---- 13. [display]: off = nothing; on = exactly its sites (+ the camera cull fix, + [fixes]); the stubs on fake frames
    unload_dll(); CHECK(map_image()); CHECK(diff_count(&dva) == 0);
    CHECK(load_dll());
    {
        TestInstall_t tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nd = tid("simulated-image-not-the-game", "[display]\nenabled = 0\nmenus = 1\nwide_hud = 1\n");
        CHECK(nd == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t tsd = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        nd = tid("simulated-image-not-the-game", "[display]\nenabled = 1\n");   // defaults: auto table, config size, wide HUD, menus, cull fix
        int nds = 0, ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            // border: off by default; DS_BL_* (device init / window sizing) and DS_MM_* belong to mode_menu (default on), which
            // also installs the shared menu hooks (MN_*: the SCREEN RESOLUTION row's value list)
            bool ds = !strncmp(s.id, "DS_", 3) && strcmp(s.id, "DS_WH_BORDER");
            bool mn = !strncmp(s.id, "MN_", 3);
            bool want = ds || mn || !strcmp(s.id, "CAM_ENTRY") || !strcmp(s.id, "CAM_ASPECT") || !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (!strcmp(s.id, "DS_WH_H480") || !strcmp(s.id, "DS_WH_W853")) changed = want;   // 480 / 853 are also the 16:9 values (written, same bytes)
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (ds) ++nds;
        }
        CHECK(ok_sites);
        printf("13. display defaults: %u patches (%d display sites incl. mode_menu + menu hooks 3 + cull fix 2 + fixes 2); border vanilla\n", nd, nds);
        CHECK(nd == (DWORD)(nds + 3 + 4));
        typedef void(__cdecl* SetA_t)(DWORD);
        typedef void(__cdecl* SetAB_t)(DWORD, DWORD);
        SetA_t set_rend = (SetA_t)tsd("ds_set_renderer"), set_quad = (SetA_t)tsd("ds_set_quad"), set_mouse = (SetA_t)tsd("ds_set_mouse_back");
        SetAB_t set_text = (SetAB_t)tsd("ds_set_text_back");
        CHECK(set_rend && set_quad && set_mouse && set_text);
        static BYTE rend[0x400]; static DWORD rend_ptr; *(int*)(rend + 0x328) = 1920; *(int*)(rend + 0x32C) = 1080;
        rend_ptr = (DWORD)(uintptr_t)rend;
        if (set_rend && set_quad && set_mouse && set_text) {
            set_rend((DWORD)(uintptr_t)&rend_ptr);
            int ok = 0;
            // a) sprite matrices: a fitted box, a full-width band, a full-screen sprite; both frame layouts
            float m[16];
            g_ds_frame_vals[0] = 400; g_ds_frame_vals[1] = 326.5f; g_ds_frame_vals[2] = 353.5f; g_ds_frame_vals[3] = 170.5f;
            if (ds_matrix_call(tsd("ds_spr_a"), 0, 2.4f, 1.8f, m) && fabs(m[0] - 1.8) < 1e-5 && fabs(m[5] - 1.8) < 1e-5 && fabs(m[12] - 240) < 1e-3 && fabs(m[13]) < 1e-3 && m[10] == 1.0f && m[15] == 1.0f) ++ok;
            else printf("    sprite box A: %f %f %f %f\n", m[0], m[5], m[12], m[13]);
            g_ds_frame_vals[0] = 400; g_ds_frame_vals[1] = 37.5f; g_ds_frame_vals[2] = 400; g_ds_frame_vals[3] = 37.5f;
            if (ds_matrix_call(tsd("ds_spr_b"), 1, 2.4f, 1.8f, m) && fabs(m[0] - 2.4) < 1e-5 && fabs(m[5] - 1.8) < 1e-5 && fabs(m[12]) < 1e-3) ++ok;
            else printf("    sprite band B: %f %f %f\n", m[0], m[5], m[12]);
            g_ds_frame_vals[0] = 400; g_ds_frame_vals[1] = 300; g_ds_frame_vals[2] = 400; g_ds_frame_vals[3] = 300;
            // full-screen sprite, default backgrounds = cover: uniform 2.4, 180 px cropped top and bottom
            if (ds_matrix_call(tsd("ds_spr_a"), 0, 2.4f, 1.8f, m) && fabs(m[0] - 2.4) < 1e-5 && fabs(m[5] - 2.4) < 1e-5 && fabs(m[12]) < 1e-3 && fabs(m[13] + 180) < 1e-2) ++ok;
            else printf("    sprite full A: %f %f %f %f\n", m[0], m[5], m[12], m[13]);
            // b) font scale: tagged s, offset kept
            typedef float*(__cdecl* FS_t)(float*);
            FS_t fs = (FS_t)tsd("ds_fontscale");
            float sc[2] = {0, 0}; float* r = fs ? fs(sc) : nullptr;
            DWORD tb; memcpy(&tb, &sc[0], 4);
            if (r == sc && fabs(sc[0] - 1.8) < 1e-5 && sc[0] == sc[1] && (tb & 1)) ++ok; else printf("    font scale %f %f\n", sc[0], sc[1]);
            // c) text stubs: tagged font -> + (240, 0) on every corner; untagged -> untouched; both continuations
            set_text((DWORD)(uintptr_t)&HostDsFall, (DWORD)(uintptr_t)&HostDsJump);
            static BYTE font[0x80]; memset(font, 0, sizeof font);
            struct TC { const char* name; int n; int off[8]; } tcs[] = {
                {"ds_txt_a_rot", 4, {0x14, 0x18, 0x28, 0x2C, 0x0C, 0x10, 0x1C, 0x20}}, {"ds_txt_a_ax", 2, {0x14, 0x18, 0x0C, 0x10}},
                {"ds_txt_b_rot", 4, {0x14, 0x18, 0x24, 0x28, 0x0C, 0x10, 0x1C, 0x20}}, {"ds_txt_b_ax", 2, {0x14, 0x18, 0x0C, 0x10}}};
            int tok = 0;
            for (const TC& tc : tcs) {
                void* st = tsd(tc.name);
                for (int variant = 0; variant < 3; ++variant) {   // 0 tagged + flag 10h set, 1 tagged + clear, 2 untagged (1.0)
                    g_ds_ncorner = tc.n;
                    for (int i = 0; i < tc.n * 2; ++i) { g_ds_corner_off[i] = tc.off[i]; g_ds_corner[i] = 100.0f + i; }
                    *(float*)(font + 0x5C) = variant == 2 ? 1.0f : sc[0]; *(float*)(font + 0x60) = variant == 2 ? 1.0f : sc[1];
                    font[0x38] = variant == 0 ? 0x10 : 0;
                    bool regs = false; int path = st ? ds_text_call(st, font, &regs) : -1;
                    bool good = regs && path == (variant == 0 ? 1 : 2);
                    for (int i = 0; i < tc.n * 2; ++i) {
                        float want = 100.0f + i + (variant == 2 ? 0.0f : (i % 2 == 0 ? 240.0f : 0.0f));
                        if (fabs(g_ds_corner[i] - want) > 1e-3) good = false;
                    }
                    if (good) ++tok; else printf("    text stub %s variant %d: path %d regs %d corner0 %f\n", tc.name, variant, path, regs, g_ds_corner[0]);
                }
            }
            if (tok == 12) ++ok;
            // d) mouse: cursor (960, 540) -> canvas (400, 300); flags kept
            set_mouse((DWORD)(uintptr_t)&HostDsFall);
            {
                void* ms = tsd("ds_mouse");
                float mx = 960, my = 540, u = 0, v = 0; DWORD fl0 = 0, fl1 = 0, rb = 0;
                g_ds_path = 0;
                __asm {
                    push ebx
                    sub esp, 0x40
                    movss xmm0, mx
                    movss dword ptr [esp + 0x8], xmm0
                    movss xmm0, my
                    movss dword ptr [esp + 0xC], xmm0
                    mov ebx, 0x0B0B0B0B
                    xor eax, eax
                    cmp eax, 1
                    pushfd
                    pop fl0
                    call ms
                    pushfd
                    pop fl1
                    mov rb, ebx
                    movss xmm0, dword ptr [esp + 0x10]
                    movss u, xmm0
                    movss xmm0, dword ptr [esp + 0x14]
                    movss v, xmm0
                    add esp, 0x40
                    pop ebx
                }
                if (g_ds_path == 1 && fabs(u - 400) < 1e-3 && fabs(v - 300) < 1e-3 && (fl0 & 0x8D5) == (fl1 & 0x8D5) && rb == 0x0B0B0B0B) ++ok;
                else printf("    mouse: path %d u %f v %f flags %08X/%08X\n", g_ds_path, u, v, fl0, fl1);
            }
            // e) load bar quad: the 4 positions moved onto the fitted canvas, the original called with everything else
            set_quad((DWORD)(uintptr_t)&HostDsQuad);
            {
                void* lb = tsd("ds_loadbar");
                static float p[4][2] = {{91 * 2.4f, 535 * 1.8f}, {91 * 2.4f, 549 * 1.8f}, {709 * 2.4f, 549 * 1.8f}, {709 * 2.4f, 535 * 1.8f}};
                static float uv[4][2]; static DWORD col = 0xFFFFFFFF;
                DWORD esp0 = 0, esp1 = 0;
                g_ds_quad_this = 0;
                __asm {
                    lea eax, col
                    push eax
                    lea eax, uv + 24
                    push eax
                    lea eax, uv + 16
                    push eax
                    lea eax, uv + 8
                    push eax
                    lea eax, uv
                    push eax
                    lea eax, p + 24
                    push eax
                    lea eax, p + 16
                    push eax
                    lea eax, p + 8
                    push eax
                    lea eax, p
                    push eax
                    push 0x7E7E7E7E
                    mov esp0, esp
                    mov ecx, 0x0C0C0C0C
                    call lb
                    mov esp1, esp
                }
                bool good = g_ds_quad_this == 0x0C0C0C0C && g_ds_quad_tex == 0x7E7E7E7E && g_ds_quad_p[0] == p[0] && g_ds_quad_p[3] == p[3] && esp1 == esp0 + 40;
                good = good && fabs(p[0][0] - (240 + 91 * 1.8)) < 1e-2 && fabs(p[2][0] - (240 + 709 * 1.8)) < 1e-2 && fabs(p[1][1] - 549 * 1.8) < 1e-2;
                if (good) ++ok; else printf("    load bar: this %08X x0 %f x2 %f esp %08X/%08X\n", g_ds_quad_this, p[0][0], p[2][0], esp0, esp1);
            }
            // f) is_wide_screen hook: true, canvas 853 x 480, the width immediate rewritten in place
            {
                typedef void(__stdcall* IW_t)(unsigned*);
                IW_t iw = (IW_t)tsd("ds_iswide");
                *(int*)(rend + 0x328) = 2560; *(int*)(rend + 0x32C) = 1080;
                unsigned out = 0; if (iw) iw(&out);
                float* vwf = (float*)tsd("ds_vwf"); float* dotx = (float*)tsd("ds_dotx");
                if (out == 1 && vwf && *vwf == 1138.0f && *(int*)(uintptr_t)0x72B060 == 1138 && dotx && fabs(*dotx - 579.0) < 1e-3) ++ok;
                else printf("    is_wide hook: out %u vw %f imm %d\n", out, vwf ? *vwf : -1.0f, *(int*)(uintptr_t)0x72B060);
                *(int*)(rend + 0x328) = 1920; *(int*)(rend + 0x32C) = 1080;
                if (iw) iw(&out);
                CHECK(*(int*)(uintptr_t)0x72B060 == 853);
            }
            set_rend(0xA39F14);
            CHECK(ok == 8);
            printf("13. display stubs: %d/8 (sprite box / band / full-screen matrices, tagged font scale, 12/12 text stub cases, mouse inverse, load bar quad, is_wide hook)\n", ok);
        }
        // mode_menu: the SetTextIndex stub (DS_MM_SETTEXT) on a fake text box: a row code gets the label as the box's own
        // text (allocated through the game allocator, here the host's), an ordinary ordinal passes through untouched
        {
            typedef void(__cdecl* SetA_t)(DWORD);
            typedef int(__cdecl* List_t)(int, int, int, int, int, int);
            SetA_t set_back = (SetA_t)tsd("dm_set_back"), set_alloc = (SetA_t)tsd("dm_set_alloc");
            List_t mk = (List_t)tsd("dm_list");
            void* st = tsd("dm_settext");
            CHECK(set_back && set_alloc && mk && st);
            if (set_back && set_alloc && mk && st) {
                set_back((DWORD)(uintptr_t)&HostDmSetIdxRest); set_alloc((DWORD)(uintptr_t)&HostDmAlloc);
                int n = mk(3024, 1964, 3024, 1892, 22, 67);   // a 3:2 screen: windowed sizes + borderless native
                CHECK(n >= 10);
                int okm = 0;
                for (int t = 0; t < 3; ++t) {
                    static BYTE tb[0x100]; memset(tb, 0, sizeof tb); *(float*)(tb + 0xC4) = 1.0f; *(float*)(tb + 0xC8) = 1.0f;
                    int ord = t == 0 ? 0x7E00 : t == 1 ? 0x7E00 + n - 1 : 205;   // first value, last value (borderless native), a text ordinal
                    g_dm_rest_ord = -1; g_dm_rest_esi = 0;
                    DmRegs r = dm_settext_call(st, (DWORD)(uintptr_t)tb, ord);
                    const wchar_t* w = *(const wchar_t**)(tb + 0xB8);
                    bool label = t < 2;
                    bool good = r.regs_ok && r.esp_ok && g_dm_rest_ord == ord && g_dm_rest_esi == (DWORD)(uintptr_t)tb;
                    if (label) {
                        good = good && w && (tb[0x14] & 8);
                        if (good && t == 0) good = !wcscmp(w, L"1024X768 WINDOW");
                        if (good && t == 1) good = !wcscmp(w, L"3024X1964 NATIVE") && *(float*)(tb + 0xC4) < 0.99f;   // 16 chars, a box of 0 width: 12 fit, drawn smaller
                    } else good = good && !w && *(float*)(tb + 0xC4) == 1.0f;
                    if (good) ++okm;
                    else printf("    settext case %d: regs %d esp %d ord %d esi %08X text %ls scale %.2f\n", t, r.regs_ok, r.esp_ok, g_dm_rest_ord, g_dm_rest_esi,
                                w ? w : L"(none)", *(float*)(tb + 0xC4));
                }
                CHECK(okm == 3);
                printf("13b. display modes: %d row values for 3024x1964 (work 3024x1892), SetTextIndex stub %d/3 (labels as the box's own text, other ordinals untouched, registers / stack kept)\n", n, okm);
            }
        }
    }
    // ---- 16. [difficulty]: off = nothing; on = the menu sites + the input tick + [fixes]; setter / getter stubs on fake data
    unload_dll(); CHECK(map_image()); CHECK(diff_count(&dva) == 0);
    CHECK(load_dll());
    {
        TestInstall_t tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nd = tid("simulated-image-not-the-game", "[difficulty]\nenabled = 0\n");
        CHECK(nd == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t tsd = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        typedef void(__cdecl* Path_t)(const char*);
        char side[MAX_PATH + 40]; sprintf_s(side, "%s\\TODGameplay_difficulty_host.txt", g_dir);
        DeleteFileA(side);
        nd = tid("simulated-image-not-the-game", "[difficulty]\nenabled = 1\n");
        Path_t setp = (Path_t)tsd("df_set_sidecar"); if (setp) setp(side);   // never the real savegames folder
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "MN_", 3) || !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (changed) check_patched_site(s);
        }
        CHECK(ok_sites && nd == 8);
        printf("16. difficulty on: %u patches (menu 3, input tick 3, fixes 2), every other site untouched\n", nd);
        typedef void(__cdecl* Redir_t)(DWORD, DWORD); typedef int(__cdecl* Flag_t)();
        void* set_stub = tsd("menu_set"); void* get_wrap = tsd("menu_get"); Redir_t redir = (Redir_t)tsd("menu_redirect"); Flag_t flag = (Flag_t)tsd("df_flag");
        CHECK(set_stub && get_wrap && redir && flag);
        if (set_stub && get_wrap && redir && flag) {
            redir((DWORD)(uintptr_t)&HostGetter, (DWORD)(uintptr_t)&HostMnSetCont);
            DWORD op; VirtualProtect((void*)(uintptr_t)0xA3A000, 0x2000, PAGE_READWRITE, &op);
            DWORD keep_cache = *(DWORD*)(uintptr_t)0xA3B58C, keep_cur = *(DWORD*)(uintptr_t)0xA3B75C, keep_lt = *(DWORD*)(uintptr_t)0xA3BB04;
            *(DWORD*)(uintptr_t)0xA3B58C = 0xCAC4EC0D;
            struct Case { int type, index, want_index, want_flag; } cases[] = {{3, 3, 2, 1}, {3, 1, 1, 0}, {3, 2, 2, 0}, {3, 3, 2, 1}, {0, 3, 3, 1}};
            int ok = 0;
            for (const Case& c : cases) {
                static int tv[2]; tv[0] = c.type; tv[1] = c.index; g_mn_hit = 0;
                DWORD esi_a = 0, edi_a = 0, ebx_a = 0, ebp_a = 0, sp0 = 0, sp1 = 0, ptv = (DWORD)(uintptr_t)tv;
                __asm {
                    push esi
                    push edi
                    push ebx
                    mov esi, 0x51515151
                    mov edi, 0x0D1D1D1D
                    mov ebx, 0x0B0B0B0B
                    mov sp0, esp
                    push ptv
                    push 0x7E7E7E7E
                    call set_stub
                    add esp, 8
                    mov sp1, esp
                    mov esi_a, esi
                    mov edi_a, edi
                    mov ebx_a, ebx
                    mov ebp_a, ebp
                    pop ebx
                    pop edi
                    pop esi
                }
                bool regs = esi_a == 0x51515151 && edi_a == 0x0D1D1D1D && ebx_a == 0x0B0B0B0B && ebp_a != 0 && sp0 == sp1;   // ebp: the frame pointer (locals), kept by construction
                if (g_mn_hit == 1 && g_mn_eax == 0xCAC4EC0D && (int)g_mn_type == c.type && (int)g_mn_val == c.want_index && flag() == c.want_flag && regs) ++ok;
                else printf("    menu set case {%d,%d}: hit %u eax %08X -> {%u,%u} flag %d regs %d\n", c.type, c.index, g_mn_hit, g_mn_eax, g_mn_type, g_mn_val, flag(), regs);
            }
            CHECK(ok == 5);
            FILE* f = nullptr; char t[256] = ""; if (!fopen_s(&f, side, "rb") && f) { t[fread(t, 1, sizeof t - 1, f)] = 0; fclose(f); }
            CHECK(strstr(t, "level = brutal") != nullptr);
            // getter wrap: a three-value difficulty entry with the data option's marker (OptionValueType4 = 77)
            memset(g_fake_entry, 0, sizeof g_fake_entry); memset(g_fake_buf, 0, sizeof g_fake_buf); memset(g_fake_list, 0, sizeof g_fake_list);
            g_fake_buf[0] = 160; g_fake_buf[1] = 161; g_fake_buf[2] = 162;
            static DWORD holder[2]; holder[1] = (DWORD)(uintptr_t)g_fake_buf;   // list +4 -> holder, holder +4 -> buffer
            g_fake_list[1] = (DWORD)(uintptr_t)holder; g_fake_list[2] = 3;   // stride 1 (fake type +24h)
            g_fake_lvt[0x44 / 4] = (DWORD)(uintptr_t)&HostListOp; g_fake_ltype[0] = (DWORD)(uintptr_t)g_fake_lvt; g_fake_ltype[0x24 / 4] = 1;
            g_fake_entry[0x20 / 4] = 3; g_fake_entry[0x1C / 4] = 160; g_fake_entry[0x24 / 4] = 161; g_fake_entry[0x2C / 4] = 162;
            g_fake_entry[0x38 / 4] = 77; g_fake_entry[0x58 / 4] = (DWORD)(uintptr_t)g_fake_list;
            g_fake_node[0x18 / 4] = (DWORD)(uintptr_t)g_fake_entry;
            *(DWORD*)(uintptr_t)0xA3BB04 = (DWORD)(uintptr_t)g_fake_ltype;
            *(DWORD*)(uintptr_t)0xA3B75C = (DWORD)(uintptr_t)g_fake_node;
            typedef void(__cdecl* Get_t)(void*, int);
            ((Get_t)get_wrap)((void*)0x7E7E7E7E, 0);
            bool got = g_fake_list[2] == 4 && g_fake_buf[3] == 77 && g_fake_entry[0x34 / 4] == 77 && g_fake_entry[0x4C / 4] == 3;
            if (!got) printf("    menu get: %u values, [3] %u, ValueString4 %u, shown %u\n", g_fake_list[2], g_fake_buf[3], g_fake_entry[0x34 / 4], g_fake_entry[0x4C / 4]);
            ((Get_t)get_wrap)((void*)0x7E7E7E7E, 0);   // a second page open: no second append
            bool once = g_fake_list[2] == 4 && g_fake_entry[0x4C / 4] == 3;
            CHECK(got && once);
            *(DWORD*)(uintptr_t)0xA3B58C = keep_cache; *(DWORD*)(uintptr_t)0xA3B75C = keep_cur; *(DWORD*)(uintptr_t)0xA3BB04 = keep_lt;
            VirtualProtect((void*)(uintptr_t)0xA3A000, 0x2000, op, &op);
            printf("16. menu setter stub %d/5 (BRUTAL -> 2 + flag, HARD / NORMAL clear it, other types untouched; registers, stack, eax), "
                   "sidecar %s, getter wrap: BRUTAL appended once, shown 3 (%s)\n", ok, strstr(t, "level = brutal") ? "written" : "MISSING", got && once ? "ok" : "BAD");
        }
        DeleteFileA(side);
    }
    unload_dll();
    // ---- 14. [pad]: off = nothing; on = exactly its sites (+ [fixes]); the "pad present" stub follows the device used
    //          last; the fake DirectInput device answers GetDeviceState with a centred DIJOYSTATE (no pad here);
    //          prompts = keyboard leaves the text sites alone
    CHECK(map_image()); CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tip = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t tsp = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        DWORD np = tip("simulated-image-not-the-game", "[pad]\nenabled = 0\nprompts = xbox\n");
        CHECK(np == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tip = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall"); tsp = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        np = tip("simulated-image-not-the-game", "[pad]\nenabled = 1\nplaystation_hid = 0\ntell_scripts = 1\n");
        int npd = 0, ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "PD_", 3) || !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (changed) check_patched_site(s);
            if (!strncmp(s.id, "PD_", 3)) ++npd;
        }
        CHECK(ok_sites);
        CHECK(np == (DWORD)(npd + 3));   // pad sites + the per-frame hook + 2 fixes
        printf("14. pad defaults: %u patches (%d pad sites + POST_INPUT + fixes 2)\n", np, npd);
        typedef void(__fastcall* Present_t)(void* ecx, void* edx, int* out);   // thiscall(int*), ret 4 (fastcall: arg on the stack)
        Present_t present = (Present_t)tsp("pad_present"); volatile LONG* mode = (volatile LONG*)tsp("pad_mode");
        CHECK(present && mode);
        if (present && mode) {
            int out = 7; *mode = 0; present(nullptr, nullptr, &out); CHECK(out == 0);
            *mode = 1; out = 7; present(nullptr, nullptr, &out); CHECK(out == 1);
            *mode = 0;
        }
        struct FakeDev { void** vtbl; };
        FakeDev* dev = (FakeDev*)tsp("pad_fake_device");
        CHECK(dev && dev->vtbl);
        if (dev && dev->vtbl) {
            typedef HRESULT(__stdcall* Poll_t)(void*);
            typedef HRESULT(__stdcall* State_t)(void*, DWORD, void*);
            typedef HRESULT(__stdcall* Fx_t)(void*, REFGUID, void*, void**, void*);
            BYTE js[80]; memset(js, 0xCC, sizeof js);
            CHECK(((Poll_t)dev->vtbl[25])(dev) == 0);   // Poll (+64h)
            CHECK(((State_t)dev->vtbl[9])(dev, 80, js) == 0);   // GetDeviceState (+24h)
            CHECK(*(int*)js == 32768 && *(int*)(js + 4) == 32768 && *(int*)(js + 8) == 32768 && *(int*)(js + 20) == 32768);
            int pressed = 0; for (int k = 48; k < 80; ++k) pressed += js[k] != 0;
            CHECK(pressed == 0);
            static const GUID g0 = {0}; void* fx = (void*)1; CHECK(((Fx_t)dev->vtbl[18])(dev, g0, nullptr, &fx, nullptr) != 0 && fx == nullptr);   // no force feedback
        }
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tip = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        np = tip("simulated-image-not-the-game", "[pad]\nenabled = 1\nplaystation_hid = 0\nprompts = keyboard\ntell_scripts = 0\nrumble_log = 0\nrumble_events = 0\n");
        CHECK(*(BYTE*)(uintptr_t)0x8FFE6F == 0xE8 && *(int*)(uintptr_t)0x8FFE70 == (int)(0x8FD870 - 0x8FFE74));   // text sites untouched
        CHECK(*(int*)(uintptr_t)0x73F7A8 == (int)(0x4853D0 - 0x73F7AC));   // the script command untouched
        CHECK(np == 6);   // PD_SEL_PROC, PD_SEL_LOOK, PD_CTLSTART, POST_INPUT, 2 fixes, no more
        printf("14. pad minimal (prompts keyboard, no script answer, no rumble hooks): %u patches; stubs: present 0/1, fake device centred, no effects\n", np);
    }
    unload_dll();
    // ---- 15. [physics]: off = nothing; on = its 3 call sites + the input tick (post-input, kb / pad procedures) + [fixes];
    //          the three wrappers keep the original's calling convention and result (originals redirected to stand-ins)
    unload_dll(); CHECK(map_image()); CHECK(diff_count(&dva) == 0);
    CHECK(load_dll());
    {
        TestInstall_t tip = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD np = tip("simulated-image-not-the-game", "[physics]\nenabled = 0\nbullet_impulse = 50\n");
        CHECK(np == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tip = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        np = tip("simulated-image-not-the-game", "[physics]\nenabled = 1\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "PH_", 3) || !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(s);
        }
        CHECK(np == 8); CHECK(ok_sites);
        DWORD* rl = (DWORD*)ts_of(g_dll, "ph_r_line"); DWORD* rs = (DWORD*)ts_of(g_dll, "ph_r_shell"); DWORD* rd = (DWORD*)ts_of(g_dll, "ph_r_destroy");
        void* wl = ts_of(g_dll, "ph_line"); void* ws = ts_of(g_dll, "ph_shell"); void* wd = ts_of(g_dll, "ph_destroy");
        CHECK(rl && rs && rd && wl && ws && wd);
        if (rl && rs && rd && wl && ws && wd) {
            CHECK(*rl == 0x8B7000 && *rs == 0x8460B0 && *rd == 0x668BA0);
            *rl = (DWORD)(uintptr_t)&HostPhLine; *rs = (DWORD)(uintptr_t)&HostPhScript; *rd = (DWORD)(uintptr_t)&HostPhScript;
            float from[3] = {1, 2, 3}, to[3] = {4, 5, 6};
            DWORD res = 0, esp_b = 0, esp_a = 0, ebx_a = 0, esi_a = 0, edi_a = 0;
            float* pf = from; float* pt = to;
            __asm {
                push ebx
                push esi
                push edi
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, 0x33333333
                mov esp_b, esp
                push pt
                push pf
                push -1
                mov ecx, 0x0BADC0DE
                call wl
                mov esp_a, esp
                mov res, eax
                mov ebx_a, ebx
                mov esi_a, esi
                mov edi_a, edi
                pop edi
                pop esi
                pop ebx
            }
            CHECK(esp_a == esp_b); CHECK(res == 0xABCD0001); CHECK(ebx_a == 0x11111111 && esi_a == 0x22222222 && edi_a == 0x33333333);
            CHECK(g_ph_a[0] == 0x0BADC0DE && g_ph_a[1] == 0xFFFFFFFF && g_ph_a[2] == (DWORD)(uintptr_t)from && g_ph_a[3] == (DWORD)(uintptr_t)to);
            typedef DWORD(__cdecl* Sc_t)(DWORD, DWORD);
            DWORD r1 = ((Sc_t)ws)(0x5150, 0x5151); bool a1 = g_ph_a[0] == 0x5150 && g_ph_a[1] == 0x5151;
            DWORD r2 = ((Sc_t)wd)(0x6160, 0); bool a2 = g_ph_a[0] == 0x6160 && g_ph_a[1] == 0;
            CHECK(r1 == 0x77 && a1 && r2 == 0x77 && a2);
            *rl = 0x8B7000; *rs = 0x8460B0; *rd = 0x668BA0;
            printf("15. physics: off 0 patches, on %u (3 call sites + input tick + fixes); line wrapper ret 0Ch, result and registers kept; shell / destroy wrappers cdecl, arguments passed\n", np);
        }
    }
    unload_dll(); CHECK(map_image());
    // ---- 17. [mayhem_title]
    CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tim = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nm = tim("simulated-image-not-the-game", "[mayhem_title]\nenabled = 0\n");
        CHECK(nm == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tim = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        nm = tim("simulated-image-not-the-game", "[mayhem_title]\nenabled = 1\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "MT_", 3) || !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(s);
        }
        CHECK(nm == 6); CHECK(ok_sites);
        DWORD* rs = (DWORD*)ts_of(g_dll, "mt_sprite_orig"); DWORD* re = (DWORD*)ts_of(g_dll, "mt_exec_orig");
        void* ws = ts_of(g_dll, "mt_sprite_wrap"); void* we = ts_of(g_dll, "mt_exec_wrap"); volatile DWORD* pend = (volatile DWORD*)ts_of(g_dll, "mt_pending");
        CHECK(rs && re && ws && we && pend);
        if (rs && re && ws && we && pend) {
            CHECK(*rs == 0x8F6CC0 && *re == 0x434290);
            *rs = (DWORD)(uintptr_t)&HostMtOrig; *re = (DWORD)(uintptr_t)&HostMtOrig;
            // a fake title sprite: name "StartScreen" (+4Ch), parent named "StartScreenfolder" (+40h), its list (+94h)
            // submitted (+14h bit 20h); its quad index (+9Ch) points at nothing valid, so nothing may be drawn
            static BYTE parent[0x100], sprite[0x100], list[0x80], other[0x100];
            static const char n1[] = "StartScreen", n2[] = "StartScreenfolder", n3[] = "StartScreenTB";
            *(DWORD*)(parent + 0x4C) = (DWORD)(uintptr_t)n2;
            *(DWORD*)(sprite + 0x4C) = (DWORD)(uintptr_t)n1; *(DWORD*)(sprite + 0x40) = (DWORD)(uintptr_t)parent;
            *(DWORD*)(sprite + 0x94) = (DWORD)(uintptr_t)list; *(DWORD*)(list + 0x14) = 0x20;
            *(DWORD*)(other + 0x4C) = (DWORD)(uintptr_t)n3; *(DWORD*)(other + 0x40) = (DWORD)(uintptr_t)parent; *(DWORD*)(other + 0x94) = (DWORD)(uintptr_t)list;
            DWORD res = 0, esp_b = 0, esp_a = 0, ebx_a = 0, esi_a = 0, edi_a = 0;
            DWORD spr = (DWORD)(uintptr_t)other;
            __asm {
                push ebx
                push esi
                push edi
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, 0x33333333
                mov ecx, spr
                mov eax, ws
                mov esp_b, esp
                call eax
                mov esp_a, esp
                mov res, eax
                mov ebx_a, ebx
                mov esi_a, esi
                mov edi_a, edi
                pop edi
                pop esi
                pop ebx
            }
            CHECK(esp_a == esp_b && res == 0x5A5A0017 && g_mt_self == spr);
            CHECK(ebx_a == 0x11111111 && esi_a == 0x22222222 && edi_a == 0x33333333);
            CHECK(*pend == 0);                                   // another sprite: not remembered
            typedef DWORD(__fastcall* W_t)(DWORD, void*);
            ((W_t)ws)((DWORD)(uintptr_t)sprite, nullptr);
            CHECK(g_mt_self == (DWORD)(uintptr_t)sprite && *pend == (DWORD)(uintptr_t)list);   // the title's submitted list
            DWORD r2 = ((W_t)we)((DWORD)(uintptr_t)list, nullptr);
            CHECK(r2 == 0x5A5A0017 && g_mt_self == (DWORD)(uintptr_t)list && *pend == 0);     // executed, consumed (no device: nothing drawn)
            *(DWORD*)(list + 0x14) = 0;                           // not submitted this frame: nothing remembered
            ((W_t)ws)((DWORD)(uintptr_t)sprite, nullptr); CHECK(*pend == 0);
            *rs = 0x8F6CC0; *re = 0x434290;
            printf("17. mayhem_title: off 0 patches, on %u (4 sites + fixes 2); sprite / list wrappers thiscall, registers kept, title list remembered once\n", nm);
        }
    }
    unload_dll(); CHECK(map_image());

    // ---- 18. [psycho]: no code site of its own; it rides the input tick / post-input every tick feature uses
    CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tps = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD np = tps("simulated-image-not-the-game", "[psycho]\nenemy_loco_moves = 0\n");
        CHECK(np == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tps = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t tsp = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        np = tps("simulated-image-not-the-game", "[psycho]\nenemy_loco_moves = 1\nmoves = toro\ncooldown = 25\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(s);
        }
        typedef int(__cdecl* I_t)();
        I_t inst = (I_t)tsp("psycho_installed"); I_t mv = (I_t)tsp("psycho_moves");
        CHECK(np == 5 && ok_sites && inst && mv);
        if (inst && mv) { CHECK(inst() == 1); CHECK(mv() == 4); }   // 1 << MOVE_TORO
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tps = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        tsp = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        tps("simulated-image-not-the-game", "[psycho]\nenemy_loco_moves = 1\nmoves = pinata\n");
        inst = (I_t)tsp("psycho_installed");
        CHECK(inst && inst() == 0);
        printf("18. psycho: off 0 patches, on %u (input tick 3 + fixes 2, no site of its own), installed; unknown moves refused\n", np);
    }
    unload_dll(); CHECK(map_image());

    // ---- 19. [gang_wars]: off = nothing; on = no site of its own (only the shared input tick + [fixes]), every GW_ REF
    //      matches the image, installed with the default gangs / pairs / spares; ini values read
    CHECK(load_dll());
    {
        TestInstall_t tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD ng = tid("simulated-image-not-the-game", "[gang_wars]\nenabled = 0\n");
        CHECK(ng == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        TestStub_t tsg = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        ng = tid("simulated-image-not-the-game", "[gang_wars]\nenabled = 1\nrange = 30\nprovoke = 0\ngroup_bikers = 26\nspare_factions = 16, 17, 9, 0, 25\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
        }
        CHECK(ok_sites && ng == 5);
        typedef int(__cdecl* I_t)(); typedef int(__cdecl* C_t)(int);
        I_t inst = (I_t)tsg("gw_installed"); C_t cfg = (C_t)tsg("gw_cfg");
        CHECK(inst && inst() == 1);
        CHECK(cfg && cfg(0) == 10 && cfg(1) == 2 && cfg(2) == 0 && cfg(3) == 300);   // 9 gangs + bikers; spares 16, 17 (9, 0, 25 refused)
        printf("19. gang_wars: off 0 patches, on %u (input tick 3 + fixes 2, no own site); every GW_ reference matches; installed, %d gangs, %d spares\n",
               ng, cfg ? cfg(0) : -1, cfg ? cfg(1) : -1);
    }
    unload_dll(); CHECK(map_image());

    // ---- 20. [minimap]; a leftover [weapon_wheel] section of an older ini changes nothing
    CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tim = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nh = tim("simulated-image-not-the-game", "[minimap]\nenabled = 0\n[weapon_wheel]\nenabled = 1\nhold_key = X\nslowmo = 0.3\n");
        CHECK(nh == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tim = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        nh = tim("simulated-image-not-the-game", "[minimap]\nenabled = 1\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = (!strncmp(s.id, "MT_", 3) && strcmp(s.id, "MT_TEXTBOX_VT")) || !strcmp(s.id, "OV_FRAME") || !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(s);
        }
        CHECK(nh == 7); CHECK(ok_sites);
        // the end-of-frame wrapper: thiscall(first, last, flag), ret 0Ch, around a stand-in for the layer renderer
        DWORD* rf = (DWORD*)ts_of(g_dll, "ov_frame_orig"); void* wf = ts_of(g_dll, "ov_frame_wrap");
        CHECK(rf && wf);
        if (rf && wf) {
            CHECK(*rf == 0x421530);
            *rf = (DWORD)(uintptr_t)&HostOvFrame;
            DWORD res = 0, esp_b = 0, esp_a = 0, ebx_a = 0, esi_a = 0, edi_a = 0, ebp_a = 0;
            __asm {
                push ebx
                push esi
                push edi
                push ebp
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, 0x33333333
                mov esp_b, esp
                push 1
                push 0x1B
                push 0x16
                mov ecx, 0x00ABCDEF
                mov eax, wf
                call eax
                mov esp_a, esp
                mov res, eax
                mov ebx_a, ebx
                mov esi_a, esi
                mov edi_a, edi
                mov ebp_a, ebp
                pop ebp
                pop edi
                pop esi
                pop ebx
            }
            CHECK(esp_a == esp_b && res == 0x5A5A0018);
            CHECK(ebx_a == 0x11111111 && esi_a == 0x22222222 && edi_a == 0x33333333);
            CHECK(g_ov_a[0] == 0x00ABCDEF && g_ov_a[1] == 0x16 && g_ov_a[2] == 0x1B && g_ov_a[3] == 1);
            *rf = 0x421530;
        }
        // the radar match: Radar_BG under a HUD_radar node (class handler 0x6F0C80) -> 2; under another class -> 0
        typedef int(__cdecl* Match_t)(DWORD);
        Match_t mt = (Match_t)ts_of(g_dll, "hr_match");
        CHECK(mt != nullptr);
        if (mt) {
            static BYTE cls[0x80], type[0x40], radar[0x100], bg[0x100], other[0x100], data[0x100], cls2[0x80], type2[0x40], radar2[0x100];
            static const char nr[] = "Radar", nb[] = "Radar_BG";
            *(DWORD*)(cls + 0x60) = 0x6F0C80; *(DWORD*)(type + 0x28) = (DWORD)(uintptr_t)cls;
            *(DWORD*)(radar + 4) = (DWORD)(uintptr_t)type; *(DWORD*)(radar + 0x4C) = (DWORD)(uintptr_t)nr; *(DWORD*)(radar + 0x18) = (DWORD)(uintptr_t)data;
            *(DWORD*)(cls2 + 0x60) = 0x6F0000; *(DWORD*)(type2 + 0x28) = (DWORD)(uintptr_t)cls2;
            *(DWORD*)(radar2 + 4) = (DWORD)(uintptr_t)type2; *(DWORD*)(radar2 + 0x4C) = (DWORD)(uintptr_t)nr;
            *(DWORD*)(other + 0x4C) = (DWORD)(uintptr_t)nb; *(DWORD*)(other + 0x40) = (DWORD)(uintptr_t)radar2;
            *(DWORD*)(bg + 0x4C) = (DWORD)(uintptr_t)nb; *(DWORD*)(bg + 0x40) = (DWORD)(uintptr_t)radar;
            CHECK(mt((DWORD)(uintptr_t)other) == 0);
            CHECK(mt((DWORD)(uintptr_t)bg) == 2);
            CHECK(mt((DWORD)(uintptr_t)bg) == 2);   // cached
            // blips: RadarBlips +34h through a fake list (type at [0xA3BB40], element size 1), targets' positions from the host
            static BYTE ltype[0x40], list[0x20], arr[0x10], e1[0x20], e2[0x20];
            static DWORD buf[4];
            *(DWORD*)(ltype + 0x24) = 1;
            *(DWORD*)(list + 4) = (DWORD)(uintptr_t)arr; *(DWORD*)(list + 8) = 3; *(DWORD*)(arr + 4) = (DWORD)(uintptr_t)buf;
            buf[0] = (DWORD)(uintptr_t)e1; buf[1] = (DWORD)(uintptr_t)e2; buf[2] = 0;
            *(DWORD*)(e1 + 8) = 0x51515151; *(DWORD*)(e1 + 0xC) = 2;     // a waypoint
            *(DWORD*)(e2 + 8) = 0; *(DWORD*)(e2 + 0xC) = 3;              // a free blip
            *(DWORD*)(data + 0x34) = (DWORD)(uintptr_t)list;
            *(float*)(data + 0x0C) = 50.0f;
            DWORD op; VirtualProtect((void*)(uintptr_t)0xA3BB40, 4, PAGE_READWRITE, &op);
            DWORD keep = *(DWORD*)(uintptr_t)0xA3BB40; *(DWORD*)(uintptr_t)0xA3BB40 = (DWORD)(uintptr_t)ltype;
            DWORD* gp = (DWORD*)ts_of(g_dll, "hr_getpos"); typedef void(__cdecl* Post_t)(double);
            Post_t post = (Post_t)ts_of(g_dll, "hr_post_input"); volatile int* nb2 = (volatile int*)ts_of(g_dll, "hr_nblips");
            CHECK(gp && post && nb2);
            if (gp && post && nb2) {
                *gp = (DWORD)(uintptr_t)&HostGetPos;
                g_pos_node = 0;
                post(1.0);
                CHECK(*nb2 == 1 && g_pos_node == 0x51515151);
                CHECK(*(float*)(data + 0x0C) == 50.0f);                  // zoom 1: the radar's range untouched
                *gp = 0x88CA80;
            }
            *(DWORD*)(uintptr_t)0xA3BB40 = keep; VirtualProtect((void*)(uintptr_t)0xA3BB40, 4, op, &op);
        }
        printf("20a. minimap: off 0 patches, on %u (overlay 4 + POST_INPUT + fixes 2); frame wrapper thiscall ret 0Ch, registers kept; radar found by class; 1 of 2 blips read\n", nh);
    }
    unload_dll(); CHECK(map_image());

    // ---- 21. [mayhem_heat]
    CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tih = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nh = tih("simulated-image-not-the-game", "[mayhem_heat]\nenabled = 0\n");
        CHECK(nh == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tih = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        nh = tih("simulated-image-not-the-game", "[mayhem_heat]\nenabled = 1\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "HT_", 3) || !strcmp(s.id, "MT_SPRITE_VT") || !strcmp(s.id, "MT_EXEC_A") || !strcmp(s.id, "MT_EXEC_B") ||
                        !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(s);
        }
        CHECK(nh == 13); CHECK(ok_sites);
        typedef void(__cdecl* Redir_t)(DWORD, DWORD, DWORD); typedef LONG(__cdecl* Q_t)(); typedef int(__cdecl* QE_t)(int);
        Redir_t redir = (Redir_t)ts_of(g_dll, "ht_redirect"); Q_t qn = (Q_t)ts_of(g_dll, "ht_queued"); QE_t qe = (QE_t)ts_of(g_dll, "ht_queued_ev");
        void* ke = ts_of(g_dll, "ht_kill_e"); void* kn = ts_of(g_dll, "ht_kill_n"); void* de = ts_of(g_dll, "ht_destroy"); void* ca = ts_of(g_dll, "ht_car");
        CHECK(redir && qn && qe && ke && kn && de && ca);
        if (redir && qn && qe && ke && kn && de && ca) {
            redir((DWORD)(uintptr_t)&HostHtTouch, (DWORD)(uintptr_t)&HostHtSend, (DWORD)(uintptr_t)&HostHtBack);
            // a fake victim: sentry node (+18h data: +178h KillOwner 0), its parent root (+40h) with data (+18h faction 9)
            static BYTE sentry[0x60], sdat[0x200], root[0x60], rdat[0x40], obj[0x60], odat[0x80], veh[0x60], vdat[0x40];
            *(DWORD*)(sentry + 0x18) = (DWORD)(uintptr_t)sdat; *(DWORD*)(sentry + 0x40) = (DWORD)(uintptr_t)root;
            *(DWORD*)(root + 0x18) = (DWORD)(uintptr_t)rdat; *(DWORD*)(rdat + 0x18) = 9;
            *(DWORD*)(odat + 0x7C) = 1;                                   // explodeable: a barrel
            DWORD sn = (DWORD)(uintptr_t)sentry, on = (DWORD)(uintptr_t)obj, od = (DWORD)(uintptr_t)odat, vn = (DWORD)(uintptr_t)veh, vd = (DWORD)(uintptr_t)vdat;
            DWORD q0 = (DWORD)qn();
            static DWORD esp_b, esp_a, ebx_a, esi_a, edi_a, ebp_a, t_sn, t_fn, t_ebp;   // static: EBP is clobbered below
            t_sn = sn; t_fn = (DWORD)(uintptr_t)ke;
            __asm {
                push ebx
                push esi
                push edi
                push ebp
                mov t_ebp, ebp
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, t_sn
                mov eax, t_fn
                mov ebp, 0x44444444
                mov esp_b, esp
                push 7
                mov ecx, 0x0BADC0DE
                call eax
                mov esp_a, esp
                mov ebx_a, ebx
                mov esi_a, esi
                mov edi_a, edi
                mov ebp_a, ebp
                pop ebp
                pop edi
                pop esi
                pop ebx
            }
            CHECK(esp_a == esp_b && ebx_a == 0x11111111 && esi_a == 0x22222222 && edi_a == sn && ebp_a == 0x44444444);
            CHECK(g_ht_hit == 1 && g_ht_ecx == 0x0BADC0DE && g_ht_a == 7);
            CHECK((DWORD)qn() == q0 + 1 && qe((int)q0) == 3);           // killer 0 = not Ram: "death nearby"
            g_ht_hit = 0;
            __asm {
                push edi
                mov edi, sn
                push 8
                mov ecx, 0x0BADC0DF
                mov eax, kn
                call eax
                pop edi
            }
            CHECK(g_ht_hit == 1 && g_ht_ecx == 0x0BADC0DF && g_ht_a == 8 && (DWORD)qn() == q0 + 2);
            g_ht_hit = 0;
            __asm {
                push esi
                push edi
                mov edi, on
                mov esi, od
                mov eax, de
                call eax
                pop edi
                pop esi
            }
            CHECK(g_ht_hit == 3 && g_ht_ecx == *(volatile DWORD*)(uintptr_t)0xA3B8C0 && (DWORD)qn() == q0 + 3 && qe((int)q0 + 2) == 6);
            g_ht_hit = 0;
            __asm {
                push ebx
                push ebp
                mov eax, ca
                mov ecx, vd
                mov ebx, vn
                mov ebp, ecx
                push 0x1234
                push 0xFFFFFFFA
                mov ecx, 0x0CA4CA4
                call eax
                pop ebp
                pop ebx
            }
            CHECK(g_ht_hit == 2 && g_ht_ecx == 0x0CA4CA4 && g_ht_a == 0xFFFFFFFA && g_ht_b == 0x1234 && (DWORD)qn() == q0 + 4 && qe((int)q0 + 3) == 7);
            redir(0x86B4B0, 0x869E60, 0x6715CA);
            typedef void(__cdecl* ScSet_t)(DWORD, int, float); typedef void(__cdecl* Sc_t)(DWORD, int*);
            ScSet_t scset = (ScSet_t)ts_of(g_dll, "ht_score_setup"); Sc_t sc = (Sc_t)ts_of(g_dll, "ht_score");
            CHECK(scset && sc);
            if (scset && sc) {
                int lv[3] = {1, 3, 5}, want[3] = {38, 57, 76}, okc = 1;
                for (int i = 0; i < 3; ++i) {
                    scset((DWORD)(uintptr_t)&HostHtScore, lv[i], 2.0f);
                    int amount = 38; g_ht_score = 0; g_ht_thr = 0;
                    sc(0x7E57, &amount);
                    if (g_ht_score != want[i] || amount != want[i] || g_ht_thr != 0x7E57) { okc = 0; printf("   score at level %d: %d (want %d)\n", lv[i], g_ht_score, want[i]); }
                }
                CHECK(okc);
                scset(0x64CEF0, 0, 2.0f);
            }
            printf("21. mayhem_heat: off 0 patches, on %u (5 event / score sites + 3 shared sprite hooks + input tick 3 + fixes 2); kill / destroy / car stubs: registers and stack kept, originals reached, 4 events queued; score 38 -> 38 / 57 / 76 at levels 1 / 3 / 5\n", nh);
        }
    }
    unload_dll(); CHECK(map_image());
    // ---- 22. [mayhem] props: without [physics] nothing; with it the 3 physics sites + MH_CAR_A / MH_CAR_B
    CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tmh = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nm = tmh("simulated-image-not-the-game", "[mayhem]\nchain_reactions = 1\nbreakable_cover = 1\n");
        CHECK(nm == 0 && diff_count(&dva) == 0);   // no [physics]: no input tick, no Box3D -> nothing
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tmh = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        nm = tmh("simulated-image-not-the-game", "[physics]\nenabled = 1\n[mayhem]\nchain_reactions = 1\nbreakable_cover = 1\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "PH_", 3) || !strncmp(s.id, "MH_", 3) || !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") ||
                        !strcmp(s.id, "PAD_PROC") || !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(s);
        }
        CHECK(nm == 13); CHECK(ok_sites);
        DWORD* rc = (DWORD*)ts_of(g_dll, "mh_r_car"); void* wc = ts_of(g_dll, "mh_car");
        DWORD* rh = (DWORD*)ts_of(g_dll, "mh_r_dohit"); void* wh = ts_of(g_dll, "mh_dohit");
        CHECK(rc && wc && rh && wh);
        if (rc && wc && rh && wh) {
            CHECK(*rc == 0x806860 && *rh == 0x86A340);
            *rc = (DWORD)(uintptr_t)&HostPhScript; *rh = (DWORD)(uintptr_t)&HostMhCmd;
            typedef DWORD(__cdecl* Sc_t)(DWORD, DWORD);
            DWORD r = ((Sc_t)wc)(0x7170, 0); bool a = g_ph_a[0] == 0x7170 && g_ph_a[1] == 0;
            CHECK(r == 0x77 && a);
            // do_explosion's take_a_hit on something that is no destroyable: passed through (thiscall, ret 8, registers kept)
            static float hit[11] = {100.0f};
            DWORD res = 0, esp_b = 0, esp_a = 0, ebx_a = 0, esi_a = 0, edi_a = 0; float* ph = hit;
            __asm {
                push ebx
                push esi
                push edi
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, 0x33333333
                mov esp_b, esp
                push ph
                push 0xD7
                mov ecx, 0x0BADC0DE
                call wh
                mov esp_a, esp
                movzx eax, al
                mov res, eax
                mov ebx_a, ebx
                mov esi_a, esi
                mov edi_a, edi
                pop edi
                pop esi
                pop ebx
            }
            CHECK(esp_a == esp_b && res == 0x5C && ebx_a == 0x11111111 && esi_a == 0x22222222 && edi_a == 0x33333333);
            CHECK(g_ph_a[0] == 0x0BADC0DE && g_ph_a[1] == 0xD7 && g_ph_a[2] == (DWORD)(uintptr_t)hit);
            // the world-pass draw wrapper: no debris -> straight to the original (thiscall, no arguments, registers kept)
            DWORD* rb = (DWORD*)ts_of(g_dll, "mh_r_blur"); void* wb = ts_of(g_dll, "mh_blur");
            CHECK(rb && wb && *rb == 0x45D5E0);
            if (rb && wb) {
                *rb = (DWORD)(uintptr_t)&HostMhBlur; g_ph_a[3] = 0;
                DWORD eb = 0, ea = 0, xb = 0, xs = 0, xd = 0;
                __asm {
                    push ebx
                    push esi
                    push edi
                    mov ebx, 0x11111111
                    mov esi, 0x22222222
                    mov edi, 0x33333333
                    mov eb, esp
                    mov ecx, 0x0DEB0DEB
                    call wb
                    mov ea, esp
                    mov xb, ebx
                    mov xs, esi
                    mov xd, edi
                    pop edi
                    pop esi
                    pop ebx
                }
                CHECK(eb == ea && g_ph_a[3] == 0x0DEB0DEB && xb == 0x11111111 && xs == 0x22222222 && xd == 0x33333333);
                *rb = 0x45D5E0;
            }
            *rc = 0x806860; *rh = 0x86A340;
            printf("22. mayhem props: without physics 0 patches, with it %u (3 physics + 5 mayhem sites + input tick + fixes); car wrapper cdecl, "
                   "do_explosion hit wrapper thiscall ret 8 (registers kept, a non-destroyable passed through); world-pass draw wrapper thiscall, no debris = the original\n", nm);
        }
    }
    unload_dll(); CHECK(map_image());
    // ---- 23. [ragdoll]: alone it brings the physics world (no props): 3 physics call sites + the input tick + its 2 sites
    CHECK(diff_count(&dva) == 0); CHECK(load_dll());
    {
        TestInstall_t tir = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nr = tir("simulated-image-not-the-game", "[ragdoll]\nenabled = 0\n");
        CHECK(nr == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tir = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        nr = tir("simulated-image-not-the-game", "[physics]\nenabled = 0\n[ragdoll]\nenabled = 1\nlog_hits = 0\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& st = g_sites[i];
            bool want = !strncmp(st.id, "PH_", 3) || !strncmp(st.id, "RD_", 3) || !strcmp(st.id, "POST_INPUT") || !strcmp(st.id, "KB_PROC") ||
                        !strcmp(st.id, "PAD_PROC") || !strcmp(st.id, "FIX_VEC_FREE") || !strcmp(st.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < st.len; ++k) orig[k] = file_byte_at(st.va + st.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(st.va + st.off), orig, st.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", st.id, changed ? "patched, not expected" : "NOT patched"); }
            if (want && changed) check_patched_site(st);
        }
        CHECK(nr == 11); CHECK(ok_sites);
        DWORD* ru = (DWORD*)ts_of(g_dll, "rd_r_charupd"); DWORD* rh = (DWORD*)ts_of(g_dll, "rd_r_hit");
        void* wu = ts_of(g_dll, "rd_charupd"); void* wh = ts_of(g_dll, "rd_hit"); volatile int* seen = (volatile int*)ts_of(g_dll, "rd_hits_seen");
        CHECK(ru && rh && wu && wh && seen);
        if (ru && rh && wu && wh && seen) {
            CHECK(*ru == 0x910CE0 && *rh == 0x86A340);
            *ru = (DWORD)(uintptr_t)&HostRdUpd; *rh = (DWORD)(uintptr_t)&HostRdCmd;
            static DWORD fake_char[0x260 / 4];   // a character the hook does not know: no parent root -> ignored, nothing written
            memset(fake_char, 0, sizeof fake_char);
            DWORD esp_b = 0, esp_a = 0, ebx_a = 0, esi_a = 0, edi_a = 0, ebp_a = 0, res = 0;
            DWORD* pc = fake_char;
            __asm {
                push ebx
                push esi
                push edi
                mov ebx, 0x11111111
                mov esi, 0x22222222
                mov edi, 0x33333333
                mov esp_b, esp
                mov ecx, pc
                call wu
                mov esp_a, esp
                mov ebx_a, ebx
                mov esi_a, esi
                mov edi_a, edi
                pop edi
                pop esi
                pop ebx
            }
            CHECK(esp_a == esp_b && ebx_a == 0x11111111 && esi_a == 0x22222222 && edi_a == 0x33333333);
            CHECK(g_rd_upd == (DWORD)(uintptr_t)fake_char);
            bool untouched = true; for (DWORD v : fake_char) if (v) untouched = false;
            CHECK(untouched);
            // the hit site: esi = the hit record, ebp = the root, ecx = the AI, push 0 / push 0x534, call -> ret 8
            static float hit[11] = {25.0f, 0, 0, 0, 1.0f, 0.0f, 2.0f, 10.0f, 1.5f, 20.0f, 0};
            int before = *seen;
            g_rd_hitp = hit; g_rd_wh = wh;
            rd_hit_call();   // ebp is changed inside: everything it touches is static
            CHECK(g_rd_r[0] == g_rd_r[1] && g_rd_r[2] == 0xC0DE);
            CHECK(g_rd_r[3] == 0x11111111 && g_rd_r[4] == (DWORD)(uintptr_t)hit && g_rd_r[5] == 0x33333333 && g_rd_r[6] == 0x44444444);
            CHECK(g_rd_cmd[0] == 0x0BADC0DE && g_rd_cmd[1] == 0x534 && g_rd_cmd[2] == 0);
            CHECK(*seen == before + 1);
            *ru = 0x910CE0; *rh = 0x86A340;
            DWORD* rc = (DWORD*)ts_of(g_dll, "rd_r_corpse"); g_rd_wc = ts_of(g_dll, "rd_corpse");
            CHECK(rc && g_rd_wc);
            if (rc && g_rd_wc) {
                CHECK(*rc == 0x606D4B);
                *rc = (DWORD)(uintptr_t)&HostRdCorpseCont;
                g_rd_fake_thread[0x40 / 4] = (DWORD)(uintptr_t)g_rd_fake_frame;   // frame +0 = no entity: not an enemy -> 0.5 s
                rd_corpse_call();
                CHECK(g_rd_cp[0] == 0x3F000000 && g_rd_cp[1] == 0); CHECK(g_rd_cp[2] == (DWORD)(uintptr_t)g_rd_fake_thread && g_rd_cp[3] == 0x55555555);
                *rc = 0x606D4B;
            }
            printf("23. ragdoll: off 0 patches, alone %u (3 ragdoll sites incl. the corpse delay + the physics world's 3 sites + input tick + fixes); character update wrapper thiscall, unknown character left alone; hit stub records the hit, ret 8, registers kept; corpse stub: no enemy -> the vanilla 0.5 s pushed, registers kept\n", nr);
        }
    }
    unload_dll(); CHECK(map_image());

    // ---- 24. [bigger_fights]: off = nothing; on = the 5 save slot sites + the shared input tick + [fixes], every reference
    //      matches; separate_saves = 0 leaves the slot sites alone
    CHECK(load_dll());
    {
        TestInstall_t tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nb = tid("simulated-image-not-the-game", "[bigger_fights]\nenabled = 0\n");
        CHECK(nb == 0 && diff_count(&dva) == 0);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        nb = tid("simulated-image-not-the-game", "[bigger_fights]\nenabled = 1\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strncmp(s.id, "BF_SLOT", 7) || !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
        }
        CHECK(ok_sites && nb == 10);
        TestStub_t tsb = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        bool* inst = (bool*)tsb("bf_installed");
        CHECK(inst && *inst);
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nk = tid("simulated-image-not-the-game", "[bigger_fights]\nenabled = 1\nseparate_saves = 0\n");
        CHECK(nk == 5);
        printf("24. bigger_fights: off 0 patches, on %u (5 save slot sites + input tick 3 + fixes 2), without separate saves %u; every reference matches\n", nb, nk);
    }
    unload_dll(); CHECK(map_image());

    // ---- 25. [drops]: no code site of its own; it rides the input tick / post-input every tick feature uses
    CHECK(load_dll());
    {
        TestInstall_t tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        DWORD nd = tid("simulated-image-not-the-game", "[drops]\nenabled = 0\n");
        CHECK(nd == 0 && diff_count(&dva) == 0);
        TestStub_t tsd = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        typedef int(__cdecl* I_t)(); typedef float(__cdecl* F_t)(float);
        F_t am = (F_t)tsd("drops_ammo");
        CHECK(am && fabsf(am(0.5f) - 0.5f) < 1e-6f);                   // off: the difficulty's own factor
        unload_dll(); CHECK(map_image()); CHECK(load_dll());
        tid = (TestInstall_t)GetProcAddress(g_dll, "TODGameplay_TestInstall");
        tsd = (TestStub_t)GetProcAddress(g_dll, "TODGameplay_TestStub");
        nd = tid("simulated-image-not-the-game", "[drops]\nenabled = 1\nammo_scale = 2\nmax_on_ground = 9\n");
        int ok_sites = 1;
        for (int i = 0; i < NS; ++i) {
            const Site& s = g_sites[i];
            bool want = !strcmp(s.id, "POST_INPUT") || !strcmp(s.id, "KB_PROC") || !strcmp(s.id, "PAD_PROC") ||
                        !strcmp(s.id, "FIX_VEC_FREE") || !strcmp(s.id, "FIX_WPN_OWNER");
            BYTE orig[16]; for (int k = 0; k < s.len; ++k) orig[k] = file_byte_at(s.va + s.off + k);
            bool changed = memcmp((const void*)(uintptr_t)(s.va + s.off), orig, s.len) != 0;
            if (changed != want) { ok_sites = 0; printf("   site %s: %s\n", s.id, changed ? "patched, not expected" : "NOT patched"); }
        }
        I_t inst = (I_t)tsd("drops_installed"); I_t mog = (I_t)tsd("drops_max_on_ground"); am = (F_t)tsd("drops_ammo");
        CHECK(nd == 5 && ok_sites && inst && mog && am);
        if (inst && mog && am) { CHECK(inst() == 1); CHECK(mog() == 5); CHECK(fabsf(am(0.5f) - 2.0f) < 1e-6f); }   // clamped to the pool's 5; x2 replaces x0.5
        printf("25. drops: off 0 patches, on %u (input tick 3 + fixes 2, no site of its own), installed, every reference matches; ammo x2 replaces x0.5\n", nd);
    }
    unload_dll(); CHECK(map_image());

    // ---- 7. the dsound loader
    {
        char p[MAX_PATH + 32]; sprintf_s(p, "%s\\dsound.dll", g_dir);
        HMODULE ds = LoadLibraryA(p);
        CHECK(ds != nullptr);
        if (ds) {
            int okn = 0;
            for (int o = 1; o <= 12; ++o) if (GetProcAddress(ds, MAKEINTRESOURCEA(o))) ++okn;
            CHECK(okn == 12);
            CHECK(GetProcAddress(ds, MAKEINTRESOURCEA(1)) == GetProcAddress(ds, "DirectSoundCreate"));
            CHECK(GetProcAddress(ds, MAKEINTRESOURCEA(11)) == GetProcAddress(ds, "DirectSoundCreate8"));
            typedef BOOL(CALLBACK* Cb)(LPGUID, LPCSTR, LPCSTR, LPVOID);
            typedef HRESULT(WINAPI* Enum_t)(Cb, LPVOID);
            Enum_t en = (Enum_t)GetProcAddress(ds, "DirectSoundEnumerateA");
            static int count = 0;
            struct L { static BOOL CALLBACK cb(LPGUID, LPCSTR, LPCSTR, LPVOID) { ++count; return TRUE; } };
            HRESULT hr = en ? en(&L::cb, nullptr) : E_FAIL;
            CHECK(hr == 0);
            HMODULE gp = GetModuleHandleA("TODGameplay.dll");
            CHECK(gp != nullptr);   // the loader loaded it
            Status_t st2 = gp ? (Status_t)GetProcAddress(gp, "TODGameplay_Status") : nullptr;
            CHECK(st2 && (st2() & TODGP_F_REFUSED));
            printf("7. dsound loader: 12/12 ordinals, DirectSoundEnumerateA hr %08lX (%d device(s)), TODGameplay loaded and refused\n", (unsigned long)hr, count);
        }
    }
    printf("host: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
