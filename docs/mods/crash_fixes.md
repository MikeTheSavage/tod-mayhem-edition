# Crash fixes (`[fixes]`)

## What it does

Fixes two crashes that are bugs in the vanilla engine. The first could hit after a few quick map-border crossings or a
save load (a double free in the asset loader). The second hit when an empty enemy gun lost its owner and asked
nobody to reload. Both are on by default and install with any other feature.

## How it is implemented

### Block-swap double free (`vector_realloc`)

1. **The crash.** Two crashes in ntdll (0xc0000005 at +0x934D9) during Downtown 01 / 02 seamless swaps. Both dumps
   showed the same stack: UpdateLoadedBlocks 0x8986E0 -> Folder_::LoadAssetBlock 0x87E8D0 -> ... -> array resize
   0x406350 -> allocator free by address 0x4778D0 -> ALLOCATOR_DEFAULT free 0x47AC90 -> CRT free.
2. **Root cause.** 0x406350 grows its buffer with the allocator's realloc (0x477D10, dispatched by address) and then
   frees the old pointer itself when it moved (0x406384..0x40638C). That is right for the fragment pools (stack
   allocators) and wrong for ALLOCATOR_DEFAULT (0xA3B0D0), whose realloc 0x47ACB0 is the CRT realloc 0x952F77: it has
   already released the old block. Result: a double free, about 24 per border swap, on every block swap and save load,
   in vanilla too. It is fatal only when the heap has reused the block meanwhile, so more swaps per minute meant more
   crashes.
3. **Diagnosis.** A `[debug] heap_check` tracker on TOD.exe's HeapAlloc / HeapFree / HeapReAlloc imports logs every
   bad free; all of them come from 0x406391. With seamless only, Smart AI only, both, no prefetch, and everything off,
   the count was the same 342 double frees each time, so no mod feature causes it.
4. **The fix.** FIX_VEC_FREE 0x40638C (CALL retarget) -> VecFreeStub: free the old buffer as before, except when the
   realloc returned a block and the old buffer's allocator (looked up like 0x4778D0, under the allocator's critical
   section 0xA3AFA0) has the CRT realloc 0x47ACB0 at vtable +18h. 3 REF rows prove the addresses (FIX_ALLOC_FREE,
   FIX_DEF_RLC, FIX_DEF_REALLOC).

### Empty-weapon reload (`weapon_owner`)

1. **The crash.** TOD.exe +0x469E89 (eip 0x869E89, reading address 4) in the post-game Harbour.
2. **Root cause.** weapon_effects_ctrl's fire shell 0x8412B0, case 10 / 11 (empty clip): 0x834EB0 re-reads the
   weapon's Owner into pOwner (+94h) and gets 0 when the gun was dropped or its character left the pool. The code then
   calls the member get 0x869E80 and `command_reload` on that null owner without a check.
3. **The fix.** FIX_WPN_OWNER 0x8418D4 (6-byte JMP hook): pOwner 0 -> skip the reload request and continue at 0x8418F6,
   as after a request. Every skip is logged (`fixes: weapon_owner:`). REF rows FIX_WPN_SKIP, FIX_WPN_OWNERRD.

Neither fix has data or per-frame work.

## Ini keys

| Key | Default | Meaning |
|---|---|---|
| `[fixes] vector_realloc` | 1 | the block-swap double free fix (0 = vanilla) |
| `[fixes] weapon_owner` | 1 | the null-owner reload fix (0 = vanilla) |

Both are applied whenever any other feature is on. With every feature off nothing is patched, these included.

Test-only (`[debug]`, never in a played game): `keys_file`, `heap_check` (1 = log and skip bad frees, 2 = log only),
`stress_swaps`, `stress_a`, `stress_b`, `stress_min`, `stress_max`, `stress_quit`, `window_offscreen`.

## Testing and limits

- Double free: a stress run in game on a test copy of the game made 40 Downtown 01 <-> 02 seamless
  swaps 0.6 to 2.0 s apart, with heap verification before and after every swap: no crash, 0 bad frees, HeapValidate OK
  86 of 86, and more than 1000 engine double frees skipped by the fix. Host test 12 covers the stub.
- Weapon owner: host test 12b covers both paths. The race could not be reproduced on demand (three runs on a save
  where the crash had happened), so this fix rests on code review and the host test. Watch for the
  `fixes: weapon_owner:` log line in play.
- The visual installer's Essentials group sets both keys.

## Files

`dll/src/gp_fix.cpp`, `dll/src/gp_debug.cpp`, `dll/src/gp_debug_logic.cpp`, `tests/dll/test_debug.cpp`,
`dll/tools/dump_info.py`, `dll/tools/exe_dis.py`, `dll/HOOKS.md` "Engine fixes" and "Debug".
