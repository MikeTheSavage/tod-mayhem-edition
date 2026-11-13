# Testing the data options in game

Every option passes the static checks in `tests/data/`. This checklist covers what only the game can show. Use a copy
of the game, not the install you play on.

## Setup (once per build)

1. Build: `python data\build_gameplay_mod.py --game "<game folder>" --options <your>.ini --out work\<name>\todgameplay_data.zip`
   (`--set option=level` works too). The report folder next to the zip holds `diff.md` with every value changed.
2. Install: `python data\install_gameplay_mod.py install --config "<game folder>\configpc.txt" --zip work\<name>\todgameplay_data.zip`.
   Add `logdumpfile = tod_gp_log.txt` to `configpc.txt` for the log.
3. Start the game. The log must show `Opening zip <todgameplay_data.zip> into slot 0`, then the block reads, and none of
   `Could not read the asset block file`, `Unable to load script`, `is grown`.
4. Undo: `python data\install_gameplay_mod.py uninstall --config "<game folder>\configpc.txt"` (restores the file
   byte for byte and deletes the copied zip). Delete the log line by hand.

Test one option at a time first (one build per option), then one combined build.

Under TOD HD: build with `--game "<TOD HD game folder>"`; the zip must be first on the line, before `hd_override1.zip`.
Check that HD textures still show (the zip carries HD's own overdose.main). Rebuild after any TOD HD change.

## 1. driving = better_feel

Where: Map02 downtown, steal a Sedan (the most common car).

| # | Step | Pass | Fail |
|---|---|---|---|
| 1.1 | Tap A/D at about 50 km/h | Finer corrections than vanilla; full lock takes about 0.28 s (vanilla 0.20 s) | No difference |
| 1.2 | Hold D at about 85 km/h | The car still turns (vanilla: almost no steering left above 90 km/h) | No steering at 85 |
| 1.3 | Hold D at about 75 km/h until it slides, keep holding | Spins out after about 1.4 s (vanilla about 0.8 s) | Spins as fast as vanilla |
| 1.4 | Release the steering key | Wheels snap straight at once (unchanged: a gradual return is `[vehicles] steering_return_rate` in TODGameplay.dll) | |
| 1.5 | S while rolling forward | Still brakes weaker than V (unchanged: S-brake parity is `[vehicles] s_brake_parity` in TODGameplay.dll) | |
| 1.6 | Watch AI cars and police | They drive and chase normally, no cars spinning in traffic | AI cars spin or crash a lot more |

## 2. difficulty = easy / hard / brutal (normal = vanilla)

Where: any mission with gunfights, for example MM020 Contact Marco, or the Map02 police assault (mounted gun under the
freeway). Note the in-game difficulty setting; the preset scales on top of it. Compare against vanilla in the same spot.

| Value | Easy | Hard | Brutal | How to see it |
|---|---|---|---|---|
| Damage Ram takes (DamageEasy/Normal/Hard) | x0.6 | x1.4 | x2.0 | Health bar after a cop's Beretta hit (vanilla Normal: 20 x 1.1 = 22) |
| Damage of Ram's hits on NPCs (PlayerDamageMod) | x1.5 | x0.8 | x0.6 | Beretta at under 9 m into a VB_Small (100 HP): vanilla 4 hits, Easy 3, Hard 5, Brutal 6 |
| Enemy spread (SpreadEnemy_m, ten firearms) | +1.5 deg | x0.6 | x0.25 | Easy: tracers and impact puffs scatter around Ram at 15+ m |
| Enemy pause between bursts (Min/MaxPauseTime) | x1.5 | x0.75 | x0.5 | Brutal: SMG and rifle enemies fire again much sooner |
| AI reaction pauses (Wait_time_from/to) | x1.5 | x0.7 | x0.5 | Hard/Brutal: enemies change cover and strafe sooner |
| Ammo in dropped guns (BulletDrop bands) | x1.5 | x0.75 | x0.5 | Pick up the same enemy's gun: more / fewer rounds |

Pass: the differences are visible and the mission plays through. Fail: no change, or friendly NPCs (Marco, escorts)
die far faster than vanilla (they share the enemy spread and pause values; their health is not touched).

## 3. camera_distance = near / far / farther

Where: Slot00 or a new game; stand still, then run.

| # | Step | Pass | Fail |
|---|---|---|---|
| 3.1 | New game (or a save made with this option) | Ram smaller (far, farther) or larger (near) on screen, his height on screen the same | No change |
| 3.2 | Walk into the narrow street east of the Map02 plaza | The camera still pulls in at walls | Camera clips through walls or goes black |
| 3.3 | **Load a save made before installing** | Expected: vanilla distance (saves store the rig's end_pos / current_pos and CameraPos.pos; checked on 9 real saves). Note whether it switches to the new distance after a cutscene, a car ride or a mission start | |
| 3.4 | Uninstall, load a save made with the option | Expected: the modded distance stays in that save until a new game; note it | |

The field of view is code: `[camera] fov` in TODGameplay.dll.

## 4. vehicle_camera = far

1. Steal any car.
2. Pass: the camera sits about 1.5 m further back than vanilla, and collisions still pull it in.
3. Fail: unchanged, or the camera snaps between two distances (that would mean the rig values are not reset per car).

## 5. slowmo_length / slowmo_regen

Where: any fight with an enemy within 20 m; dive (SPACE + direction) to trigger the automatic bullet time.

| Option | Pass |
|---|---|
| slowmo_length = short / long / very_long | The stamina bar empties in about 4 / 16 / 32 s of dive slow motion (vanilla 8 s from full) |
| slowmo_regen = slow / fast | The bar refills half / twice as fast standing (vanilla 150 per s) and running (80 per s) |

Fail: no change, or sprinting is affected (it shares the stamina bar; the sprint cost is not touched).
Turning the automatic slow motion off is code: `[slowmo] dive_auto_slowmo` in TODGameplay.dll.

## 6. cutscenes = unlock_all (experimental)

Movies are already skippable with Return / Escape / Space. This option makes the 26 locked in-engine camera
sequences skippable too. Press Escape at the start of each; after a skip the camera must return to Ram and the
mission must go on.

| Block | Sequence | Watch for |
|---|---|---|
| mm010_ernestocruz | PlaneLanding_01 (plane landing), Escape_Plane_Dead, ONBoard_Airplane | Prologue goes on; slow motion ends |
| mm013_tommycruz | Truck_Blows_Up | |
| mm030 / mm066 / mm075 | Cesar intro, mission-start establishing shot, cave elevator | Elevator still arrives |
| mm080 | waypoint01 (slow-motion shots) | Time back to normal after the skip |
| mm090 | Kingroom battle, General cutscene | Boss fight starts |
| mm092 / mm100 / mm110 / mm120 | Plane approach, telescope, vault elevator, Johnson teleport | Teleport / elevator still happen |
| intermission_after_jungle | Mission start | |
| subm_05 (two runner jumps), subm_06/07/08/12/18/21/30/33 races, subm_15 | Race-start camera | The race starts and the countdown works |

Pass: every skip returns control and the mission completes. Fail (note the sequence): a softlock, the player stuck
in slow motion, or an elevator / teleport that never happens. A failing sequence can be kept locked by removing its
block from `UNSKIP_BLOCKS` in `data/gameplay/options.py`.

## 7. Audio

Nothing to install: the SOUND page of the in-game options already has separate music, speech, effects and ambience
sliders (saved in Slot11). Check that they work; anything beyond that would be DLL code.

## 8. Combined build

1. Install one zip with several options.
2. Play 10 minutes of free roam and one mission.
3. Pass: every option behaves as in its own test, the log is clean, and an existing save loads.
