# TODGameplay.dll

Quality-of-life and gameplay options for **Total Overdose (2005), GOG release** (`TOD.exe` 6,860,800 bytes,
md5 `d9beb351b3c78ee9f14a766a368f3ff9`). This is the code half of "TOD Gameplay+"; the data options are in `..\data\`.

Every feature is statically verified: the shipped bytes of every site, a disassembly review, unit tests and a non-game
test host. The Status column below says which features were also checked in game. The in-game checklist is
[TEST_IN_GAME.md](TEST_IN_GAME.md); every hook is listed in [HOOKS.md](HOOKS.md).

## Features

Every feature is switched in `TODGameplay.ini` next to `TOD.exe`. A missing ini, a missing key or `0` means off; with
everything off the DLL writes nothing to the game's memory (the loader only forwards dsound.dll).

| Feature | ini | What it does | Status |
|---|---|---|---|
| Manual bullet time | `[bullettime]` | A key / button toggles (or holds) slow motion through the game's own time controller (`command_trigger_slowmotion`, then WorldTime latched to the scale so the game's own "back to normal" returns to bullet time). Slow-motion bullets and blur as in a dive. A meter drains in real time and refills from kills, combo points and slowly over time. Starts are refused in exclusive modes (Tornado, power-up menu) and cut-scenes; it ends cleanly on death, vehicles, cut-scenes and mission starts (the game resets WorldTime, the DLL notices). | built, verified |
| Meter for other programs | always with bullet time | Shared memory `Local\TODGameplay.Shared.v1` (64-byte struct, seqlock), see `src/todgameplay_shared.h`. | built, verified in the test host |
| Meter bar on screen | - | **Deferred**: the only safe draw points are the D3D device hooks that TOD HD's d3d9.dll and the renderer bridge already own, and the HUD sprite system is not researched for new widgets. Other programs can draw it from the shared memory. | deferred |
| Dive slow motion off | `[slowmo] dive_auto_slowmo = 0` (`wire_auto_slowmo` for the slide wire) | Skips TIME_CTRL `command_auto_slow_motion` at the three dive sites (and the wire). Dives still cost nothing; stamina is untouched. | built, verified |
| Slow-motion clock-stall fix | `[timing] clock_fix = 1` | The integer-ms game clock (AI, triggers, timers) keeps the fraction instead of truncating every frame: at time scale 1 identical to vanilla, in slow motion no lag and no stall at high frame rates (vanilla freezes from 360 fps at scale 0.3). Also the rewind credit and the real-ms clock. | built, verified (stub executed in the host) |
| Pickup step fix | `[timing] pickup_step_fix = 1` | Dropped weapons and goodies keep their step remainder (vanilla throws it away: they fall 1.6x faster at 144 fps than at 60). | built, verified |
| Frame limiter | `[timing] fps_limit = N` | High-resolution waitable timer plus a short spin, before the input poll. Off by default: TOD HD's d3d9.dll and the renderer bridge already cap at 60; use one limiter. | built, verified |
| Mouse turning independent of frame rate | `[controls] mouse_turn_fix = 1` | Every frame's mouse turn is accumulated and applied at the next physics step (vanilla drops 60-90 % of the motion and loses sensitivity as 1/fps). Same feel as vanilla at 60 fps; tune `mouse_turn_seconds_per_frame`. Gamepad unchanged. | built, verified |
| Raw mouse | - | The game already reads the mouse through DirectInput 8 relative data, which Windows does not accelerate; no change needed. | no change |
| Mouse sensitivity / invert Y | `[controls] mouse_sensitivity_x/y`, `invert_y` | Scales MouseDeltaX / Y where the game writes them; invert flips Y (on top of the game's own invert option). `mouse_y_fps_normalize` (experimental) makes the vertical look frame-rate independent. | built, verified |
| Keyboard diagonals | `[controls] diagonal_normalize = 1` | W+D moves at 5 m/s instead of 7.07 (stick copy clamped to length 1). | built, verified |
| Key rebinding | `[keys]` | `game key or action = physical key` (keyboard keys, MOUSE1..5); applied to the game's own keyboard arrays after each poll, so menus see it too. A moved key loses its old function unless rebound. | built, verified (unit tests) |
| Camera FOV | `[camera] fov` | The game's 64 becomes the value; zooms keep their tangent ratio (El Toro, rewind, triggers). The culling frustum follows. `cull_fix` uses the real aspect for culling (leave it off with the renderer bridge). Camera distance is data: `..\data` option `camera_distance`. | built, verified |
| Quick restart | `[restart]` (hold F10 1 s) | Sends the mission controller `Command_End_Mission(0)`: the game's own failed-mission path, whose result screen offers the retry (restart from the mission start). Refused in intermissions / free roam, the prologue (MM010/MM013 load their follow-up directly) and the tutorial. Checkpoints are the game's saves: load them from the menu. | built, verified (static), not yet confirmed in game |
| Skippable movies / cut-scenes | - | Already skippable in vanilla (Return / Escape / Space); the locked camera shots are data: `..\data` option `cutscenes = unlock_all`. Nothing in the DLL. | no change needed |
| Vehicle steering return | `[vehicles] steering_return_rate` | Experimental: wheels return to straight gradually instead of snapping. | built, verified |
| S brakes like V | `[vehicles] s_brake_parity = 1` | Experimental: S counts as a pedal while rolling forward (mass factor 1.0). | built, verified |
| Suave loco move | `[suave]` | Power-up slot 11 becomes Suave: a salsa with the nearest enemy, a clapping ring, the game's dynamite, a shoot-dodge escape and the explosion, with camera shots, faces and music. Code and docs in `..\suave` (README, TEST_IN_GAME). Screenshot: [suave_dance.jpg](../docs/media/suave_dance.jpg). | built, verified (static) |
| Smart AI | `[smart_ai]` | Smarter enemies through the enemy brain's own fields (code and docs in `..\smart_ai`): each enemy gets a role at spawn (assault / rifleman / cautious: fight distance, back-off, shoot-dodge, move mix, reaction delay); per target at most 2 close in while the rest hold and shoot from range, taking turns (the game's DontHunt); allies dying nearby can break an enemy's morale (the game's CS_HealthLeft / CS_Change_for, flee state). Off by default; one call site (the AI init) plus the input tick. | built, verified (static, host), not yet confirmed in game |
| Seamless map borders | `[seamless]` | A submap swap triggered by a walk-border loader (no teleport; not missions, taxis, ZAP, cutscenes, deaths, saves, menus) runs without the scripted fade-out, the 0.3 s wait, the load screen, the progress bar, the 0.2 s black and the fade-in: the last frame stays on screen during the synchronous swap (a short hitch). Every swap is timed in the log; near a loader the next blocks are read into the OS file cache on a background thread (`TODGameplay_seamless.txt`). Pair with the data option `seamless` (bridge and tunnel teleports become walk borders). Code and docs in `..\seamless`. | built, statically verified (9 sites, the test host drives one swap through every stub), not yet confirmed in game |
| Border-swap crash fix | `[fixes] vector_realloc = 1` (default; installed with any other feature) | The engine's asset loader freed an array buffer the CRT realloc had already released: a double free on every block swap (vanilla too) that crashed the game in ntdll after a few quick border crossings. One call site (0x40638C); details in `HOOKS.md` "Engine fixes". | built, verified (host + 40 seamless swaps on a test copy with heap verification) |
| Weapon reload crash fix | `[fixes] weapon_owner = 1` (default; installed with any other feature) | An empty enemy weapon whose owner was gone (dropped, character despawned) asked that null owner to reload: access violation at 0x869E89 (a vanilla engine bug, seen as a crash in the post-game Harbour). One jmp at 0x8418D4 skips the request when the owner is 0; details in `HOOKS.md` "Engine fixes". | built, host-tested |
| 16:9 display | `[display]` (off by default) | Total Overdose at 1920x1080 (any wide aspect) as it should look: the resolution list becomes 16:9 (index 4 = the configpc size) and the saved option no longer switches the game to 4:3 after the first frame; the in-game HUD uses the whole width (score / ammo / health / radar at the screen edges, aim dot centred; optional `hud_scale`, `border`); menus, subtitles, load screens and the front end keep their 4:3 layout with square pixels (pillarboxed, text no longer 33 % too wide), the mouse hits the items, the load bar fits its frame; full-width bars stay full width, full-screen art covers the screen undistorted; `cull_fix` (no popping at the 16:9 edges). **In-game display modes** (`mode_menu`, default on): SETTINGS > SCREEN RESOLUTION lists the monitor's real windowed sizes, borderless at the native size and exclusive fullscreen sizes, applies the chosen one live through the game's own keep / revert countdown, and keeps it in `TODGameplay_display.txt`. Docs `..\display\README.md`, sites `HOOKS.md` "Display" / "Display modes". | built, statically verified (30 sites, host test 13), checked on a test copy at 1920x1080 |
| Controller support, prompts, rumble | `[pad]` (off by default), `[pad_bind]`; data option `controller = menu` | Xbox-style pads (XInput) and DualShock 4 / DualSense (HID, USB and Bluetooth) play through the game's own console controls: the console "XBox" bindings of ControlDict are added to the PC ones (every action, vehicles, loco moves, menus, pause), Gamepad 0 reads the DLL's pad, and the game's own gamepad procedure (analog movement, right-stick turn / look) runs while the pad is the device used last; keyboard + mouse keep working and take over when touched. Prompts follow the device: key names in texts (`Press [%0]`, the menu help `[Enter]` / `[Esc]` / `[A]`, `[Space]`) become button pictures in Xbox or PlayStation style (drawn by the DLL into the font atlas), `prompts = auto / keyboard / xbox / playstation` (an ini key only, on purpose: a BUTTON PROMPTS menu row would be a new record in HUD.fragment, which is not the last fragment loaded, and saves would stop loading). Rumble: the game's own vibration requests (it already obeys its VIBRATION setting) plus shots, shoot-dodge landings and camera shakes, `rumble_strength`. Dead zones, look curve / sensitivity / invert and button layout are in the ini. The data option shows the console-only SETTINGS > VIBRATION row on PC and moves the "To confirm Screen Resolution" hint below the then longer list. Details `HOOKS.md` "Pad". | built, unit + host tested; checked on a test copy with a scripted pad, not yet with a physical pad |
| Box3D physics props (prototype) | `[physics]` (off by default) | Loose props (crates, bins, chairs, benches ...) become Box3D rigid bodies (TODPhysics.dll, `third_party/box3d`): they fall and settle on the level's own collision, bullets push them, grenade / rocket / dynamite and exploding destroyables throw them, and the game draws them where the simulation puts them. Cosmetic: their gameplay collision is still the game's own. Docs `../physics/README.md`, sites `HOOKS.md` "Physics". Screenshot: [physics_explosion.jpg](../docs/media/physics_explosion.jpg). | built, statically verified (3 sites, host test 14, 176 unit checks), checked on a test copy |
| MAYHEM title | `[mayhem_title]` (off in code, `enabled = 1` in the sample ini) | The title screen shows as vanilla for `delay` (0.6 s); then an ember ignites in the shotgun's muzzle and a ring of fire burns a hole through the picture (thin hot rim, paper curling back, scorch, char with ember specks) that ends hugging the word MAYHEM, whose lettering comes up dark, then glowing, then in colour. Afterwards: heat shimmer on the painted flames, flickering ember cracks, a looping real-fire flipbook over the flame tips, a few sparks. The main menu gets the same burn after "TOTAL OVERDOSE" in its header band the first time it appears in a session (`main_menu = 1`), burnt in afterwards. Drawn by the DLL (vs/ps 3.0 shaders through the game's D3D9 device, state read back and restored) only right after the title picture / the menu caption: never in game, loading or pause. Art files next to the exe: `TODGameplay_mayhem.dds`, `_fx.dds`, `_fire.dds` (built by `../mayhem_title/art/build_*.py`). Sites `HOOKS.md` "Mayhem title". Before / after: [title_before_after.jpg](../docs/media/title_before_after.jpg). | built, unit + host tested (test 17), checked on a test copy |
| Difficulty in the menu | `[difficulty]` (off by default) | SETTINGS > DIFFICULTY LEVEL offers EASY / NORMAL / HARD / BRUTAL / PSYCHO, chosen in game and saved like the vanilla setting (Slot11 holds the game's Hard for the two new levels; `TODGameplay_difficulty.txt` next to it says which). EASY / NORMAL = vanilla; HARD = vanilla Hard + the hard factors of the data option `difficulty`, applied at run time; BRUTAL = vanilla Hard + the brutal factors + 50 % more enemies (for every two enemy deaths one dead placement brings one more enemy from the pool); PSYCHO = BRUTAL + enemy loco moves when `[psycho] enemy_loco_moves = 1`. Needs the data option `difficulty_menu` and `difficulty = normal`. Shared menu helper `src/gp_menu.cpp`. Docs `../docs/mods/difficulty.md`, `HOOKS.md` "Menus" / "Difficulty". | built, host test 14, checked on a test copy (menu, save / reload, reinforcements) |
| PSYCHO: enemies use loco moves | `[psycho]` (off by default; only on PSYCHO) | Elite enemies (health >= 200) and bosses use the Tornado spin (Ram's ram_locomove, their gun firing blanks, hits while Ram is in range, in sight and not diving) and the El Toro charge (ram_rammove_run along the line to where Ram stood). 1 s telegraph (red power-up flash, orange glow, the game's loco-move sting); a hit in the wind-up cancels it; per-enemy cooldown 20-40 s; one enemy move at a time; damage only to Ram (take_a_hit, enemy as owner); no slow motion, camera, HUD or meter change. Keys `enemy_loco_moves`, `moves`, `cooldown`, `telegraph_seconds`, `elite_only`. Docs `..\psycho\README.md`, `HOOKS.md` "PSYCHO enemy loco moves". | built, unit tests, host test 18, checked on a test copy |
| Mayhem heat | `[mayhem_heat]` (off in code, `enabled = 1` in the sample ini) | The mayhem meter / wanted heat: kills (cops and civilians more), Ram's explosions, broken props, barrels, cars, loco moves and stylish combo kills build heat in five levels; the game's own radar shows it (warm orange tint while it builds, red at the top with the rim pulsing red / dark) with 1..5 chilli coins in the loco-badge style beside the disc; it cools one level per `decay_seconds` once Ram has caused nothing for `calm_seconds`. More heat = reinforcement waves of dead placements of the area (rooftops first at level 3+, second / third comebacks at 4-5) through the [difficulty] reinforcement engine and all of its mission-safety rules, on every difficulty, scaled by it. About 0.01 ms per frame. Sites `HOOKS.md` "Mayhem heat" / "Shared HUD stages", docs `../docs/mods/mayhem_heat.md`. | built, unit + host tested (test 18), checked on a test copy |
| Hang watchdog | `[debug] hang_watchdog` (on in code and in the sample ini) | For freezes (the picture stops, sound goes on, no crash). A thread watches a heartbeat that the game thread gives every frame (POST_INPUT). When no frame has come for `hang_seconds` (5; `hang_load_seconds` = 60 during a seamless swap) while the game window is up and in front (never while minimised, alt-tabbed or with a debugger), it logs once per freeze: what the mod was doing last, the game thread's EIP, registers and its return addresses as module+offset with the mod's function names (from `TODGameplay.map` / `TODPhysics.map` next to the DLL), and writes `TODGameplay_hang_<date>_<time>.dmp` next to the log (all threads, a few MB; at most `hang_max_dumps` = 3 per session). It logs "the game thread is back after X s" when frames come again. No new patch site (it rides on the POST_INPUT hook), nothing while the game runs normally but a counter. Off on a non-GOG exe (like every module); works without dbghelp.dll (log only). `tools\hang_report.py DUMP` prints every thread's stack of such a dump. Test copy only: `debug_stall_key` stops the game thread for `debug_stall_seconds` (8). | built, unit tests (test_hang), checked on a test copy |
| Debug / stress (test copy only) | `[debug]` | Key script, heap tracker on TOD.exe's heap imports, automatic walk-border swap stress, driven by an in-game test harness (not included). Not for a played game. | built, used for the crash fix |
| Turn-to-face movement | - | **Deferred** (optional): needs the camera yaw split from the body yaw (input 0x73F7E0, integrate 0x57C220, camera 0x4C9DE0); not researched far enough for a safe patch. | deferred |

## Keys and defaults (sample ini)

| Action | Default |
|---|---|
| Bullet time | **middle mouse button** (MOUSE3), pad: right stick click (XInput pad 1), toggle mode |
| Bullet time feel | scale 0.30, ramp 0.25 s in / 0.25 s out, blur on, meter 8 s, start 10 %, +12 % per kill, +0.05 % per combo point, +2 %/s after 2 s idle, starts full |
| Dives in bullet time | keep the bullet-time speed (`dive_slowmo_in_bullettime = 0`) |
| Quick restart | hold **F10** for 1 s |
| Everything else in the sample | clock fix, pickup fix, mouse turn fix, diagonal clamp on; FOV, limiter, invert, vehicles, rebinding off |

## Install (on a copy first)

Files: `dist\TODGameplay.dll`, `dist\dsound.dll` (the loader), `dist\TODGameplay.ini` (and `dist\TODPhysics.dll`, only
needed for `[physics]`). `dist\TODGameplay.map` and `dist\TODPhysics.map` (the linker maps of this build) go next to
them for the hang watchdog's function names. A map from another build is not detected and gives wrong names, so always
ship the maps with their DLLs.

* Test copy: `powershell -ExecutionPolicy Bypass -File dll\install_copy.ps1 -Target <repo>\work\<copy>`
  (refuses any folder outside `<repo>\work\`).
* Real game, by hand: copy the files next to `TOD.exe`. Nothing else is changed; the game folder's d3d9.dll (TOD HD)
  and dinput8.dll (renderer bridge) stay as they are.
* Without the dsound.dll loader: the renderer bridge can load the DLL with one line, as can any proxy that calls
  `LoadLibraryA("<game folder>\\TODGameplay.dll")`.
* Another dsound.dll already in use (for example DSOAL): rename it and set `[loader] chain_dsound = <its new name>` (no
  comment on that line).

## Uninstall

Delete `dsound.dll`, `TODGameplay.dll` and `TODGameplay.ini` next to `TOD.exe` (copies: `install_copy.ps1 -Uninstall`).
The DLL changes nothing on disk except its log `TODGameplay.log`.

## Build and tests

`dll\build.bat` (VS 18, v145, x86; output `<repo>\build`, copied to `dist\`) builds the DLL and the loader and runs the
checks below. Set `PY` (default `python`) and `TODEXE` (default `C:\GOG Games\Total Overdose\TOD.exe`) in the
environment to use another Python or exe path. The build runs:

* `tests\dll\test_logic.exe` (pure logic);
* `tests\dll\check_sites.py` (every site's shipped bytes and instruction boundaries in the exe, read only);
* `tests\dll\host.exe`, a non-game process: refusal when the exe hash differs, then it installs onto a copy of TOD.exe's
  sections mapped at their addresses and checks every patched byte, executes the self-contained stubs, executes the
  Suave gate stub and loads the dsound loader. A crashing host fails the build.

Per feature:

| Feature | Tests |
|---|---|
| Suave | `test_suave.exe`, `suave\code\tools\check_suave.py` |
| Smart AI | `test_smart.exe`, `smart_ai\code\tools\check_smart.py`; host tests 9 and 10 |
| Display | `test_display.exe`; host test 13 |
| Mayhem title | `test_mayhem.exe`; host test 17 |
| Mayhem heat | `test_heat.exe`; host test 18 |
| Pad | `test_pad.exe`, `test_glyphs.exe` (writes `tests\dll\review\pad\*.png`); host test 14 |
| Hang watchdog | `test_hang.exe` (the stall state machine, the call-site check, the map parser on the build's own `TODGameplay.map`) |

The build also writes review listings to `tests\dll\review\` (every site's original instructions and the disassembly
of every stub).

## Files

| Path | What |
|---|---|
| `src/gp_game.cpp` | game glue: identity, ini, patches, stubs, input tick, per-frame work, shared memory |
| `src/gp_logic.cpp/.h` | pure logic (meter, bullet-time state machine, mouse maths, carry, limiter, keys, ini, md5) |
| `src/sites.h` | every patch site and referenced game address with its shipped bytes |
| `src/todgameplay_shared.h` | shared-memory struct for the meter |
| `src/gp_hang.cpp`, `src/gp_hang_logic.cpp/.h` | [debug] hang_watchdog: the watchdog thread, stack capture, minidump / its pure logic (stall state machine, call check, map parser) |
| `tools/hang_report.py` | every thread's stack of a TODGameplay_hang_*.dmp (or any TOD.exe minidump), named from the .map files |
| `loader/dsound_proxy.cpp`, `loader/dsound.def` | the dsound.dll loader |
| `../smart_ai/code/smart_game.cpp`, `smart_logic.cpp/.h` | Smart AI glue and pure logic |
| `src/gp_display.cpp`, `src/gp_display_logic.cpp/.h` | [display] glue / stubs and its pure maths |
| `src/gp_display_modes.cpp/.h` | [display] mode_menu: the SCREEN RESOLUTION row, live mode changes, the side file |
| `src/gp_pad.cpp`, `src/gp_pad_logic.cpp/.h`, `src/gp_glyphs.cpp/.h`, `src/gp_pad_kb.inc` | [pad] glue (fake DirectInput pad, control merge, prompts, rumble, XInput / HID readers), its pure logic, the button pictures (GDI+), the Keyboard text ordinals |
| `src/gp_mayhem.cpp`, `src/gp_mayhem_logic.cpp/.h`, `src/gp_mayhem_ps.hlsl`, `src/gp_mayhem_vs.hlsl` | [mayhem_title] glue (hooks, art files, shader pass, state read-back), the placement / burn / timeline / spark logic, the burn shaders (compiled by build.bat with fxc) |
| `src/gp_hudstage.cpp`, `src/gp_heat.cpp`, `src/gp_heat_logic.cpp/.h`, `src/gp_heat.rc` | shared sprite hooks + RADAR stage (radar tint / draw clients), [mayhem_heat] glue (event stubs, polls, HUD, escalation drive), its pure logic, the icon art resource |
| `../mayhem_heat/` | icon art generator `art/make_heat_art.py`, its output `art/heat_icons.png` / `art/heat_glyphs.h`, chili art `art/chili/`, clip maker `tools/make_clip.py` |
| `../mayhem_title/` | the MAYHEM lettering and fire clip, texture builders (`art/build_mayhem_textures.py`, `art/build_fire_flipbook.py`, output `art/build/`), offline burn preview (`tools/burn_preview.py` + the shader's numpy twin `tools/burn_model.py`) |
| `../physics/code/phys_game.cpp`, `phys_logic.cpp/.h`, `phys_sim.cpp`, `phys_api.h` | [physics] glue (in this DLL), pure logic, TODPhysics.dll (Box3D) and its interface |
| `TODGameplay.ini` | sample ini (copied to dist) |
| `build.bat`, `install_copy.ps1` | build + tests; install on copies |
