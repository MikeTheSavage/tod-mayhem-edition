# Smart AI (`[smart_ai]` in TODGameplay.ini)

Smarter enemies for the GOG Total Overdose, built into `TODGameplay.dll`. Hooks and evidence are in
[../dll/HOOKS.md](../dll/HOOKS.md), section "Smart AI". In-game checklist: [TEST_IN_GAME.md](TEST_IN_GAME.md).

Off by default: with `enabled = 0` (or no section) nothing is patched.

Only values an enemy's row already has are scaled, specials and DEA are skipped, and an enemy found again keeps its
first personality. These rules were added after a crash in the post-game Harbour 02.

## What it does

The enemy brain of TOD (`character_sentry_ai`) already has per-enemy fields for fight distance, back-off, a move mix,
a reaction delay, a "hold position" flag and a morale test. In the shipped game they are the same for every enemy of a
type and never react to the battle. Smart AI only writes those fields (with the game's own member-write notify first)
and leaves the acting to the game's state machine.

| Switch | Effect in play | How |
|---|---|---|
| `personality` | Enemies of one type stop being clones: about 30 % **assault** (close in to 7 m, strafe and shoot-dodge more), 25 % **cautious** (stay at 13.5 m, crouch more, back off 25 % further), the rest **riflemen** (8.5-11.5 m). Every enemy's wait / strafe / crouch times and move weights differ by up to 25 %. Only what the row already has is scaled: a type without shoot-dodge, back-off or a move (melee, defenders, specials) never gets one. | at spawn, after the game's AI init (call site 0x5E7C28); enemies a loaded save restored are found by the tick within a second |
| `reaction_stagger` | A group no longer reacts in the same frame: each enemy stands 0.2-1.0 s (assault quicker) before it starts its attack, unless Ram is within 8 m. | the game's own Wait_until_Aggressive, set once, cleared after use |
| `squad` | With 3+ ranged enemies on Ram, at most 2 advance to their fight distance; the others hold where they are and shoot from range. The turn passes after 8 s. Enemies farther than 22 m, or holding without a shot for 5 s, close in anyway. | the game's own DontHunt per enemy |
| `morale` | An ally dying within 15 m frightens the enemies around it (more for the last one standing); a frightened, hurt enemy may break and run (the game's flee). The fear fades in about 8-20 s. Stand-fast enemies never break. | the game's own CS_HealthLeft / CS_Change_for |

Left alone: Ram's buddies, the factions of `skip_factions` (civilians 6, DEA 10, wrestlers 13), special / escort /
never-aggressive types, bosses and specials (health >= `boss_health`, 1000), enemies whose hold flag the mission
authored, and anything another script changes after the DLL wrote it.

## Install, switch, uninstall

As the rest of TODGameplay.dll ([../dll/README.md](../dll/README.md)): `dsound.dll`, `TODGameplay.dll`, `TODGameplay.ini` next to
`TOD.exe`; set `[smart_ai] enabled = 1`. Each switch can be turned off alone. Uninstall: delete the three files or set
`enabled = 0`.

**Saves.** The game saves enemy members. While the game is paused (the menu) the DLL puts the hold flags and morale
values back to the game's own, so a save from the menu holds vanilla values for those; the personality values
(distances, move mix) are in a save made while the feature runs, and after uninstalling, enemies restored from such a
save keep their personality until they respawn. A save made while it runs loads fine without it.

## Files

| Path | What |
|---|---|
| `code/smart_logic.h/.cpp` | pure decisions (personalities, approach tokens, fear), no game memory |
| `code/smart_game.cpp` | game glue: the init wrapper, the tick, discovery, all reads / writes (SEH-guarded) |
| `code/test_smart.cpp` | unit tests (built and run by `..\dll\build.bat`) |
| `code/tools/check_smart.py` | static exe checks (run by `..\dll\build.bat`) |
| `TEST_IN_GAME.md` | the in-game checklist (The Ambush) |
