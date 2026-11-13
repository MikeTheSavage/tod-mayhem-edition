# How the mod is built

This page covers what every mod in TOD Gameplay+ shares: how the code gets into the game, how patches are checked,
the build, in-game testing and the engine rules that shape every feature. The per-mod guides are listed in
[README.md](README.md).

All addresses are for the GOG `TOD.exe` (6,860,800 bytes, md5 `d9beb351b3c78ee9f14a766a368f3ff9`, image base
0x400000). A hook address in these guides is the first patched byte. `dll/src/sites.h` starts its byte check a few
bytes earlier on some rows, so `dll/HOOKS.md` sometimes quotes that check address instead.

## How the mod loads

1. **dsound.dll proxy.** `TOD.exe` imports DirectSoundCreate (ordinal 1) from dsound.dll. dsound.dll is not a
   KnownDLL and nothing else in the process loads it, so a copy next to the exe wins. The proxy (`dll/loader/`)
   forwards all 12 dsound exports to the system DLL (or to `[loader] chain_dsound`, for example a renamed DSOAL build)
   and loads `<game folder>\TODGameplay.dll` by full path from its DllMain. d3d9.dll (TOD HD) and dinput8.dll (the
   renderer bridge) are left to other proxies.
2. **TODGameplay.dll + TODGameplay.ini.** The DLL reads the ini next to the exe. A missing file, section or key means
   off. With every feature off it patches nothing. It uses no engine classes and has no global constructors that touch
   the engine (/MT build).
3. **The exe gate (`g_patch_allowed`).** At start the DLL hashes `TOD.exe` on disk. Only the exact GOG size and md5
   (and an MZ header at 0x400000) set `g_patch_allowed`. With any other exe nothing is patched, and one log line says
   why.
4. **Sites and REFs (`dll/src/sites.h`).** Every patch is a `SITE` row: address, the shipped bytes, patch offset and
   length, and kind (CALL retarget, JMP hook, OPER operand change, DATA pointer). A site is patched only when all its
   bytes match in memory; a mismatch (another mod got there first) skips it and logs. `REF` rows are never patched:
   they are instructions that prove an address or member offset the DLL reads or writes. A feature checks its REF rows
   at run time and stays off if any differs. `tests/dll/check_sites.py` checks every row against the exe on disk and
   checks that patches cover whole instructions and do not overlap. Current count: 114 sites, 391 refs, all OK.
5. **Shared per-frame hooks.** Most features need no site of their own. They run from:
   - POST_INPUT (0x43C516, after the keyboard and pad poll each main-loop pass),
   - the input tick KB_PROC / PAD_PROC (0x740B70 / 0x740AAD: after the player's input procedure, in the player's
     script context, only while he is in control).
   Each is patched once; `gp_game.cpp` calls every feature from its own wrapper.
6. **TODPhysics.dll.** Box3D (Erin Catto, MIT, pinned commit `3362633`, `third_party/box3d` plus a two-line x86 patch)
   lives in its own DLL behind a C function table (`physics/code/phys_api.h`). TODGameplay.dll loads it only when a
   physics feature is on. It holds the props world, the car boxes and the ragdolls.
7. **The data zip.** Game data cannot be shipped, so data changes are built on the player's PC from their own
   `blocks.naz` (`data/build_gameplay_mod.py`) into `todgameplay_data.zip`, which configpc.txt's `filesystem` line
   lists first. See [data_zip.md](data_zip.md).
8. **The shared overlay layer (`dll/src/gp_overlay.cpp`).** One set of patches for every 2D overlay: the Sprite
   per-frame render (vtable slot 0x9D2F70), the two render-list executor calls in the layer renderer (0x421AB3,
   0x421AE2) and the end-of-frame layer call (OV_FRAME 0x421BBF). Features register clients (sprite match / post / draw,
   frame draw). Every client reads back and restores each D3D9 state it touches. A fault turns that client off for the
   session. Clients: MAYHEM title, minimap, the radar stage of mayhem heat.
9. **The shared menu helper (`dll/src/gp_menu.cpp`).** Three sites (MN_GET_INIT 0x7FF75C, MN_GET_RESET 0x7FE7C8,
   MN_SET 0x6BA420) let a feature own one SETTINGS row by its option type: append values, replace values, map the
   chosen index. Used by difficulty (type 3) and resolution (type 20).
10. **Logs.** `TODGameplay.log` next to the exe. Every feature logs its decisions, its skipped sites and its cost.

## The build

`dll\build.bat` (Visual Studio 18, toolset v145, x86). Output goes to `<repo>\build`, then `dist\`.

1. Box3D (C17, SSE2 path) into `box3d_x86.lib`, then `TODPhysics.dll` (phys_sim, phys_car_sim, rd_sim).
2. The MAYHEM shaders with fxc, the heat icon resource with rc.
3. `TODGameplay.dll` (every feature's glue and logic), then the `dsound.dll` loader.
4. Every unit test (logic, suave, smart AI, gang wars, seamless, debug, display, pad, glyphs, physics, mayhem props,
   cars, ragdoll, difficulty, MAYHEM, psycho, HUD, heat).
5. `check_sites.py`, `check_suave.py`, `check_smart.py` (they read the exe only), and the stub disassembly listing.
6. `host.exe`: a non-game process maps TOD.exe's sections at their addresses, confirms the DLL refuses there (hash),
   then installs onto the image through a test entry, checks every patched byte, and executes the self-contained stubs.
   The guides refer to its numbered cases as "host test N".
7. Copies the DLLs, the sample ini and the three MAYHEM `.dds` files to `dist\`.

Run it only when no TOD.exe is running: a running game owns the shared-memory block and fails one host check.

## How features are tested in game

Every feature was checked in game on a separate test copy of the game, never on a played install. The test scripts
that drove those runs were specific to the author's machine and are not part of the repo. Each run followed the same
pattern:

1. Refuse while any `TOD.exe` / `TotalOverdose.exe` is running, and check again right before launch.
2. Touch only the test copy. The GOG folder and the real saves are only hashed.
3. Point USERPROFILE at an isolated profile and seed its save, Slot11 and side files.
4. Merge the ini with per-run overrides and a `[debug] keys_file` key script (wait, tap, hold, `waitlog` on the game's
   own log, `mark shot NAME`).
5. Grab the game window with PrintWindow at each mark.
6. Close the game by its own PID, remove everything the run added, and compare listings and hashes of the test copy,
   the real saves and the GOG files (all byte-identical).
7. Export the registry key `HKCU\Software\Eidos\Total Overdose` before the run and import it after.

Test-only ini keys start with `debug_` (plus the `[debug]` section). The installer never writes them, and the guides
list them separately from the player settings.

A combined test of the full build (every feature on, HD effects included) measured about 0.25 ms per frame for all
modules together in a fight.

## Engine rules

1. **New records only in the last instanced fragment.** Saves address nodes by their runtime creation index. A create
   record in any fragment but the last one instanced in its block shifts every later node, and old saves replay onto
   the wrong nodes and crash (an early version of the Suave clips in AnimDict.fragment did this). Data options use set
   operations on existing records; new records go at the end of the block's last fragment (Suave's clips live in
   GameCamera.fragment).
2. **No new menu rows.** A new SETTINGS row is a create record in HUD.fragment, which is not the last fragment
   instanced, so saves break. New choices go into existing rows (values appended or replaced at run time by the DLL)
   or into ini keys.
3. **Slot11 only gets vanilla values.** Setting_difficulty stays 0..2 and Setting_ScreenSize 0..4. A larger value makes
   the vanilla menu index past its list. Extra state goes to side files next to Slot11
   (`Documents\Total Overdose\savegames\TODGameplay_difficulty.txt`, `TODGameplay_display.txt`).
4. **The enemy pool is 9 shells.** Enemies are character placeholders; CharacterResourceCtrl hands out pooled shells
   (9 AICharacter in DefaultEnemyResources). MaxAllocated is 6 or 8 (written at 0x763694 / 0x7636A6); the most the mod
   raises it to is 9. Reinforcements, corpses held by ragdolls, Suave partners, Smart AI and gang wars all share those
   9 (Bigger Fights raises the pool itself, see [bigger_fights.md](bigger_fights.md)).
5. **Nothing is spawned.** There are no spawners or waves in the code. More enemies means re-arming dead placements
   with the game's own setters, so a save holds a normal dormant placeholder.
6. **Choose the write path on purpose.** A member write after the game's notify (0x86B4B0) is recorded for rewind and
   saves. A write without it is runtime only. Tuning, tints and cap changes use no notify; re-arms and faction changes
   use it, as the game does.
7. **Save and restore the script globals.** Every game call from the DLL saves and restores the current-node globals
   0xA3B75C / 0xA3B760 / 0xA3B764.
8. **The TIMESTAMP clock node is not game milliseconds.** Cache +274h (SM_CLOCK) ran at a tenth of game time in the
   PSYCHO tests and lagged real time in the gang-wars tests. Use the engine's game-ms global 0xA3DCD4 or real seconds.
9. **The saved screen size is applied after the first frame** (0x769ABD), which throws wide setups back to a 4:3 size.
10. **Ram cannot be moved from outside.** SetWorldPosition 0x88E600 does not stick on Ram (his physics controller owns
    his position), and a quaternion write on his root sinks him through the map. Turn him through the game's own yaw
    (mouse-turn accumulator or PhysicCtrl.turn_speed).
11. **Pools everywhere.** Destroyables are a pool of about 11 handed to the nearest placeholders; far objects do not
    exist yet. Pooled weapon shells keep node flag 4 (hidden) from their last blast, and `command_explode` on a shell
    never aimed does nothing (aiming it with owner 0 crashes).
12. **Shared buffers.** The model-name getter 0x884350 returns one shared buffer: copy it at once.
13. **A vanilla double free on every block swap** (0x406350). Survivable most of the time, fatal sometimes. Fixed by
    `[fixes] vector_realloc` ([crash_fixes.md](crash_fixes.md)).
14. **The ini parser.** Room for 512 entries in a 256 KB buffer (the sample has 355 keys). Older builds silently dropped
    everything after entry 256.

## Reference files

| What | Path |
|---|---|
| Every hook site, its bytes and how it is verified | `dll/HOOKS.md`, `dll/src/sites.h` |
| DLL features, keys, install | `dll/README.md`, `dll/TODGameplay.ini` (sample) |
| In-game test checklist for the DLL | `dll/TEST_IN_GAME.md` |
| Box3D licence and the x86 patch | `third_party/README.md`, `third_party/box3d_x86.patch` |
