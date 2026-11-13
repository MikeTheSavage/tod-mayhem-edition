# The data zip (`todgameplay_data.zip`)

## What it does

Changes the game's own data: driving feel, camera distance, slow-motion meter length and refill, skippable cut-scene
shots, and the data halves of other mods (Suave clips and music, seamless borders, the VIBRATION row, the BRUTAL /
PSYCHO menu texts). It is built on the player's PC from his own game files, so no game data is ever shipped.

## How it is implemented

1. **Engine facts.** The game reads its blocks through the archives listed on configpc.txt's `filesystem =` line, in
   order: the first archive that has a file wins. A STORED zip listed first therefore overrides single blocks. Blocks
   are fragment files; each node is a create record plus property sets. Saves address nodes by runtime creation index.
2. **Read the player's blocks.** `data/gameplay/gamefs.py` looks each block up in the game's own `filesystem` order (so
   a TOD HD `hd_override` block wins where it exists) and never writes into a game folder.
3. **Apply the options.** `data/gameplay/options.py` holds every option and level with its factors. Each value is
   generated from the player's own block at build time (x factor, + delta, or moved along the camera's line of sight).
   Vanilla levels emit nothing. Changes are set operations on existing records; new records are only appended at the
   end of the last fragment instanced in a block (the save rule). The one exception is the experimental option
   `bigger_fights`, which creates mid-block on purpose and sends saves to their own files
   ([bigger_fights.md](bigger_fights.md)).
4. **Verify.** `builder.py` checks every edited block: the base block round-trips exactly, the output re-parses and
   re-serialises exactly, header / INFO / untouched payloads are byte-identical, the property / command checksums are
   recomputed from the proven tables, create records (node, type, order) are identical, only the planned (node,
   property) pairs differ, and every planned value reads back. Two options that change the same property are refused.
5. **Zip.** STORED, read back through a port of the engine's OpenZip / FindFile, checked for name-hash collisions with
   blocks.naz. A report of every changed value goes to `<out>.report/diff.md`.
6. **Install.** `data/install_gameplay_mod.py` copies the zip and its manifest and puts it first on the configpc.txt
   `filesystem` line (backup `configpc.txt.todgameplay.bak`, idempotent, exact undo, refuses while TOD.exe runs from
   that folder).
7. **Save check.** `savecheck.py` reads copies of the player's saves to see which edited values a save also stores
   (driving, difficulty and slow-motion values are not stored; the player camera is, so `camera_distance` only applies
   to a new game).

## Options

| Option | Levels | What changes |
|---|---|---|
| `driving` | vanilla, better_feel | sedan turn_rate 0.12, 25 VehicleDict rows: cornering x0.9, control_speed x0.5 |
| `difficulty` | normal, easy, hard, brutal | static damage / spread / reaction / ammo factors. Keep `normal` with the DLL: the DLL applies the levels at run time ([difficulty.md](difficulty.md)) |
| `camera_distance` | default, near, far, farther | player camera -0.5 / +0.75 / +1.5 m along its line of sight (new games only) |
| `vehicle_camera` | default, far | every vehicle camera 1.5 m back |
| `slowmo_length` | default, short, long, very_long | dive slow-motion meter 4 / 16 / 32 s |
| `slowmo_regen` | default, slow, fast | meter refill x0.5 / x2 |
| `cutscenes` | vanilla, unlock_all | Unskipable cleared on 70 camera triggers in 22 mission blocks (experimental) |
| `suave_anims` | off, on | the Suave clips, appended to GameCamera.fragment ([suave.md](suave.md)) |
| `suave` | off, on, on_start2, on_slot10 | Suave's HUD name, icon stand-in, music streams |
| `bigger_fights` | off, on | experimental, breaks saves: bigger pools, dormant extra placements, three dormant police cars per block ([bigger_fights.md](bigger_fights.md)) |
| `seamless` | off, on, on_nomarkers | corridor teleports become walk borders; markers hidden ([seamless.md](seamless.md)) |
| `controller` | off, menu | SETTINGS > VIBRATION shown on PC; the resolution hint moved down ([controller.md](controller.md)) |
| `difficulty_menu` | off, on, on_all_languages | BRUTAL / PSYCHO texts in unused Menu text slots #77 / #101 ([difficulty.md](difficulty.md)) |

`python build_gameplay_mod.py --list` prints the same table.

## Commands

```
cd <repo>\data
python -I build_gameplay_mod.py --game "C:\GOG Games\Total Overdose" --options my.ini --out ..\work\gog\todgameplay_data.zip
python -I install_gameplay_mod.py install   --config "<game folder>\configpc.txt" --zip ..\work\gog\todgameplay_data.zip
python -I install_gameplay_mod.py uninstall --config "<game folder>\configpc.txt"
```
Python 3.12, standard library only. The visual installer runs the same builder in-process ([installer.md](installer.md)).
Rebuild after a TOD HD update: the zip carries a copy of any block it edits and hides the other copy.

## Testing and limits

- Unit tests: `tests/data/` (`python -m unittest` there; `TODGP_SLOW=1` adds the cut-scene blocks and the TOD HD base).
  A full run is 43 tests with 3 skipped. One test, `test_install.test_on_copies_of_the_real_configs`, compares against
  the configpc.txt of a local game install and fails when that file differs from what it expects.
- A combined build equals the union of the single-option diffs byte for byte.
- `camera_distance` does not apply to saves made before installing (the camera position is saved).
- `cutscenes = unlock_all` leaves the Point_Challenge intros in 7 submaps locked.

## Files

`data/build_gameplay_mod.py`, `data/install_gameplay_mod.py`, `data/gameplay/` (gamefs, options, builder, savecheck,
seamless, suave_data, suave_anims, menus), `data/lib/` (fragment codec, save-editor reader), `data/options.ini`
(example), `data/TEST_IN_GAME.md`, `tests/data/`.
