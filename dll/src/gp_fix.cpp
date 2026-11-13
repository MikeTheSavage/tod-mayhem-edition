// gp_fix.cpp - engine bug fixes of TODGameplay.dll. [fixes] in TODGameplay.ini; installed
// whenever the DLL patches anything else (with every feature off nothing is patched, as before).
//
// vector_realloc (default 1): the border-swap crash (ntdll 0xc0000005 at +0x934D9):
//   The engine's growable array 0x406350 (used by the table copy of the asset loader, mode 3 of 0x85DEC0, on every
//   block swap) grows its buffer with the allocator realloc 0x477D10 and then frees the OLD buffer itself
//   (0x406384..0x40638C: push 0, push 0, push old; call 0x4778D0) whenever the pointer changed. That is right for the
//   fragment pools (their realloc grows in place or fails), but when the buffer lives in ALLOCATOR_DEFAULT (0xA3B0D0)
//   the realloc is the CRT realloc (0x47ACB0 -> 0x952F77), which already released the old block when it moved it:
//   the second free is a double free in the CRT heap. Measured on the test copy (heap tracker of [debug]): 24 double
//   frees of ~10 KB blocks per border swap, vanilla and seamless alike; both crash dumps fault in exactly this free
//   (old 0x0F488940 / 0x1D096F48, new = old + 0x27F8, size 0x27F8).
//   The fix retargets that one call: the old buffer is passed on to the free as before, except when the realloc
//   returned a new block (esi != 0) and the old one belongs to an allocator whose realloc is the CRT one (vtable +18h
//   = 0x47ACB0): then it was already freed and is left alone.
#include <windows.h>
#include <stdio.h>
#include "gp_glue.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))

static DWORD R_FIX_FREE = 0x4778D0;               // FIX_VEC_FREE original call target (allocator free by address, cdecl)
static const DWORD A_ALLOC_OFF = 0xA3AFBC;        // allocators shut down: free / realloc do nothing (REF FIX_ALLOC_FREE)
static const DWORD A_ALLOC_CS = 0xA3AFA0;         // the allocator table's critical section
static const DWORD A_RANGE_N = 0xA3B098;          // range count; range i: start [0xA3AFE0 + 8i], allocator [0xA3AFE4 + 8i]
static const DWORD A_RANGE = 0xA3AFE0;
static DWORD R_FIX_CRT_REALLOC = 0x47ACB0;        // ALLOCATOR_DEFAULT realloc method (REF FIX_DEF_REALLOC / FIX_DEF_RLC)

static bool g_vec = false, g_vec_on = false;
static volatile LONG g_skipped = 0;

// weapon_owner (default 1): the weapon reload crash (TOD.exe 0xc0000005 at +0x469E89, seen in Harbour 02):
//   weapon_effects_ctrl's fire shell 0x8412B0 sets state 10 when the clip is empty and its pOwner (+94h) is a character
//   (0x841CBB..0x841D0F) and resumes in the next step. Case 10/11 (0x84184E) first re-reads the weapon root's Owner into
//   pOwner (0x834EB0: 0 when the weapon lost its owner in between: dropped, its character despawned / returned to the
//   pool), and then, whenever that helper said "no", asks the owner for its weapon controller and sends it
//   command_reload: 0x8418D4 mov ecx,[esi+94h]; push &out; push 17h; call 0x869E80 (member get: mov eax,[ecx+4]) ->
//   with pOwner 0 a read of address 4 (the crash dump: ecx 0, exception address 4, return 0x8418E6). The shell checks
//   pOwner before it enters state 10 (0x841CBB) and 0x834EB0 answers "no" for a lost owner, but this path then uses
//   pOwner unchecked.
//   The fix (FIX_WPN_OWNER, a jmp at 0x8418D4): pOwner 0 -> no reload request, continue at 0x8418F6 (the step after the
//   two calls, which picks the next state); otherwise the original instruction and on to 0x8418DA.
static bool g_wo = false, g_wo_on = false;
static volatile LONG g_wo_skips = 0;
static DWORD R_WO_CONT = 0x8418DA, R_WO_SKIP = 0x8418F6;
static void __cdecl WoSkipLog(DWORD data, DWORD node) {
    LONG k = InterlockedIncrement(&g_wo_skips);
    if (k > 5 && k % 100) return;
    DWORD root = 0, parent = 0;
    __try { root = data ? RD32(data + 0x84) : 0; parent = node ? RD32(node + 0x40) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("fixes: weapon_owner: an empty weapon whose owner is gone asked for a reload (weapon ctrl %08X, data %08X, root %08X, "
           "parent %08X): skipped (%ld so far; vanilla crashes here)\n", node, data, root, parent, k);
}
// FIX_WPN_OWNER at 0x8418D4 (6 bytes: mov ecx,[esi+94h]); esi = the shell's data, ebx = its node (on this path),
// edi = the thread. Flags are dead at both continuations.
__declspec(naked) void WeaponOwnerStub() {
    __asm mov ecx, dword ptr [esi + 0x94]
    __asm test ecx, ecx
    __asm jz wo_skip
    __asm jmp dword ptr [R_WO_CONT]
wo_skip:
    __asm pushad
    __asm push ebx
    __asm push esi
    __asm call WoSkipLog
    __asm add esp, 8
    __asm popad
    __asm jmp dword ptr [R_WO_SKIP]
}

// the allocator 0x4778D0 would pick for p (same search: the highest range start <= p)
DWORD gp_fix_allocator_of(DWORD p, DWORD range_base, DWORD count) {
    for (DWORD i = count; i >= 1; --i) {
        DWORD start = RD32(range_base + 8 * i);
        if (p >= start) return RD32(range_base + 8 * i + 4);
    }
    return 0;
}

static BYTE __cdecl VecFreeSkipC(DWORD old_buf, DWORD new_buf) {
    if (!new_buf || !old_buf) return 0;                   // realloc failed: the old block is still ours to free
    BYTE skip = 0;
    __try {
        if (*(volatile BYTE*)(uintptr_t)A_ALLOC_OFF) return 0;
        DWORD a = 0;
        EnterCriticalSection((CRITICAL_SECTION*)(uintptr_t)A_ALLOC_CS);   // the same lock the dispatch takes
        __try { a = gp_fix_allocator_of(old_buf, A_RANGE, RD32(A_RANGE_N)); }
        __finally { LeaveCriticalSection((CRITICAL_SECTION*)(uintptr_t)A_ALLOC_CS); }
        DWORD vt = a ? RD32(a) : 0;
        skip = vt && RD32(vt + 0x18) == R_FIX_CRT_REALLOC;
    } __except (EXCEPTION_EXECUTE_HANDLER) { skip = 0; }
    if (skip) {
        LONG k = InterlockedIncrement(&g_skipped);
        if (k <= 3 || k % 500 == 0)
            gp_log("fixes: vector_realloc: the engine's second free of %08X (already released by the CRT realloc, now %08X) skipped (%ld so far)\n",
                   old_buf, new_buf, k);
    }
    return skip;
}

// FIX_VEC_FREE call site 0x40638C inside 0x406350: [esp+4] = old buffer (then 0, 0), esi = the realloc's result; the
// caller pops the 3 arguments. eax / ecx / edx are free (the original call clobbers them); ebx / esi / edi / ebp are
// preserved by the C half.
__declspec(naked) void VecFreeStub() {
    __asm push esi
    __asm push dword ptr [esp + 8]
    __asm call VecFreeSkipC
    __asm add esp, 8
    __asm test al, al
    __asm jz pass
    __asm ret
pass:
    __asm jmp dword ptr [R_FIX_FREE]
}

void gp_fix_load_config() {
    g_vec = gp_ini().geti("fixes", "vector_realloc", 1) != 0;
    g_wo = gp_ini().geti("fixes", "weapon_owner", 1) != 0;
}
bool gp_fix_wanted() { return g_vec || g_wo; }
int gp_fix_install() {
    int n = 0;
    if (g_wo) {
        if (!gp_ref_ok("FIX_WPN_SKIP") || !gp_ref_ok("FIX_WPN_OWNERRD")) gp_log("fixes: weapon_owner: reference bytes differ: OFF\n");
        else if (!gp_site_ok("FIX_WPN_OWNER")) gp_log("fixes: weapon_owner: site differs (another mod there?): OFF\n");
        else if (gp_patch_call("FIX_WPN_OWNER", (void*)&WeaponOwnerStub)) {
            g_wo_on = true; ++n;
            gp_log("fixes: weapon_owner on: an empty weapon whose owner is gone no longer asks a null owner to reload (crash at 0x869E89)\n");
        }
    }
    if (g_vec) {
        if (!gp_ref_ok("FIX_ALLOC_FREE") || !gp_ref_ok("FIX_DEF_REALLOC") || !gp_ref_ok("FIX_DEF_RLC"))
            gp_log("fixes: vector_realloc: reference bytes differ: OFF\n");
        else if (!gp_site_ok("FIX_VEC_FREE")) gp_log("fixes: vector_realloc: site differs (another mod there?): OFF\n");
        else if (gp_patch_call("FIX_VEC_FREE", (void*)&VecFreeStub)) {
            g_vec_on = true; ++n;
            gp_log("fixes: vector_realloc on: the asset loader's array no longer frees a buffer the CRT realloc already released (border-swap crash)\n");
        }
    }
    return n;
}

// ---- test host only
static void __cdecl TestRedirect(DWORD free_fn, DWORD crt_realloc) { R_FIX_FREE = free_fn; R_FIX_CRT_REALLOC = crt_realloc; }
static LONG __cdecl TestSkipped() { return g_skipped; }
static void __cdecl TestWoRedirect(DWORD cont, DWORD skip) { R_WO_CONT = cont; R_WO_SKIP = skip; }
static LONG __cdecl TestWoSkips() { return g_wo_skips; }
void* gp_fix_test_stub(const char* name) {
    if (!strcmp(name, "fix_weapon_owner")) return (void*)&WeaponOwnerStub;
    if (!strcmp(name, "fix_wo_redirect")) return (void*)&TestWoRedirect;
    if (!strcmp(name, "fix_wo_skips")) return (void*)&TestWoSkips;
    if (!strcmp(name, "fix_vec_free")) return (void*)&VecFreeStub;
    if (!strcmp(name, "fix_redirect")) return (void*)&TestRedirect;
    if (!strcmp(name, "fix_skipped")) return (void*)&TestSkipped;
    if (!strcmp(name, "fix_allocator_of")) return (void*)&gp_fix_allocator_of;
    return nullptr;
}
