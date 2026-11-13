# Mod guides

TOD Gameplay+ (TOD Mayhem Edition) is a set of gameplay mods for the GOG release of Total Overdose (2005). Each mod has
its own guide here: what it does for the player, how it works inside the engine (hook sites, what runs each frame,
data changes), its ini settings and defaults, how it was tested and its known limits. All addresses are for the GOG
`TOD.exe` (6,860,800 bytes, md5 `d9beb351b3c78ee9f14a766a368f3ff9`, image base 0x400000). For the parts every mod
shares (the loader, the exe check, hook sites, the build, in-game testing and the engine rules), read
[architecture.md](architecture.md) first.

## The mods

| Mod | What it does for the player | Guide |
|---|---|---|
| Core gameplay | Manual bullet time with a meter, dive slow motion off, frame-rate fixes for timers, pickups and mouse turning, FOV, key rebinding, quick restart, two driving tweaks | [core_gameplay.md](core_gameplay.md) |
| Crash fixes | Fixes two vanilla engine crashes: the block-swap double free and the empty-weapon reload | [crash_fixes.md](crash_fixes.md) |
| Data zip | Driving feel, camera distance, slow-motion meter, cut-scene skips and the data halves of other mods, built from the player's own game files | [data_zip.md](data_zip.md) |
| Display fix (16:9) | The game stays at the wide size you chose; HUD at the screen edges; menus and text no longer stretched | [display_fix.md](display_fix.md) |
| In-game resolution | SETTINGS > SCREEN RESOLUTION lists the monitor's real windowed, borderless and fullscreen modes and switches live | [resolution_menu.md](resolution_menu.md) |
| Controller | Xbox and PlayStation pads play through the game's own console controls, with button prompts and rumble | [controller.md](controller.md) |
| Physics (Box3D) | Loose props fall, tumble and fly from bullets and explosions, also while Ram drives | [physics.md](physics.md) |
| Car physics | Cars shove props, blasts throw cars, wrecks flip and settle | [car_physics.md](car_physics.md) |
| Ragdoll | Hit enemies stagger; dead ones go limp and are thrown by blasts | [ragdoll.md](ragdoll.md) |
| Chain reactions + breakable cover | Explosions set off explosives and cars nearby; cover props break into debris | [chain_reactions_cover.md](chain_reactions_cover.md) |
| Difficulty | EASY / NORMAL / HARD / BRUTAL / PSYCHO in the game's own SETTINGS menu; BRUTAL brings more enemies | [difficulty.md](difficulty.md) |
| PSYCHO loco moves | On PSYCHO, elite enemies use the Tornado spin and the El Toro charge against Ram, and leave that move as a pickup when they die | [psycho_loco_moves.md](psycho_loco_moves.md) |
| Drops | Dead enemies leave more ammo, sometimes health and the odd power-up | [drops.md](drops.md) |
| Mayhem heat | Chaos builds a 5-level wanted meter shown on the radar; more heat sends more enemies | [mayhem_heat.md](mayhem_heat.md) |
| Bigger Fights (experimental, breaks saves) | More enemies from the start of a fight on BRUTAL / PSYCHO; police cars with gunmen at heat 3+ | [bigger_fights.md](bigger_fights.md) |
| Gang wars | Rival gangs fight each other when they meet | [gang_wars.md](gang_wars.md) |
| Minimap, objective markers | A sharp tile map inside the radar on a paper disc, markers over objectives | [hud_minimap.md](hud_minimap.md) |
| MAYHEM title | MAYHEM burns through the title screen and into the main menu header | [mayhem_title.md](mayhem_title.md) |
| HD effects | TOD HD's shader effects (AO, lighting, shadows, post, particles, water) on the vanilla game | [hd_effects.md](hd_effects.md) |
| Seamless world | Walking across a map border swaps the area without a load screen | [seamless.md](seamless.md) |
| Suave | A new loco move: a salsa with an enemy, a clapping ring, dynamite and a dive | [suave.md](suave.md) |
| Smart AI | Enemies get roles, stagger their reactions, take turns closing in, and can break and run | [smart_ai.md](smart_ai.md) |
| Installer | A visual installer with backups, per-group toggles and clean restore | [installer.md](installer.md) |

## Where the source lives

Every feature compiles into `TODGameplay.dll` (plus `TODPhysics.dll` for the Box3D parts), but the code is split by
feature. The shared DLL code and the smaller features sit in `dll/src/` (`gp_*.cpp`, with every hook site in
`dll/src/sites.h` and `dll/HOOKS.md`). The larger features have their own top-level folder with `code/`, `tools/` and
usually a README: `physics/` (props and cars), `ragdoll/`, `mayhem/` (chain reactions and cover), `psycho/`, `drops/`,
`gang_wars/`, `smart_ai/`, `suave/`, `seamless/`, `hud/` (minimap bake), `mayhem_heat/` and `mayhem_title/` (art and
tools), `hdfx/` (HD effects), `display/` (display tools) and `bigger_fights/` (tools). Data changes are built by the
Python code in `data/`, and the installer is in `installer/`. The "Files" section at the end of each guide lists the
exact files for that mod.
