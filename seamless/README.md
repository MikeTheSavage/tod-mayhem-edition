# Seamless map borders

The city of Total Overdose is several blocks (Downtown 01-04, Industrial 01-04, Harbour 01-02, Border 01) that swap
when you cross a border. This feature makes the crossings play without a load screen on the GOG game with the
original renderer.

Status: **experimental**. The static checks below pass; it has not been verified in game yet. The in-game checklist
is [TEST_IN_GAME.md](TEST_IN_GAME.md).

## Two halves

| Half | What | Where |
|---|---|---|
| Data option `seamless = on` / `on_nomarkers` | The corridor teleports become walk borders: the Downtown-Harbour bridge (T1) and the Downtown 02 - Industrial 04 / Industrial 02 tunnel. Teleport_to emptied on 5 loader actions, one box per corridor moved along the corridor to 23 m past the other block's return box. Set ops only (save-safe). The farm pass stays a teleport (no shared ground). `on_nomarkers` also hides the markers of every walk border: 52 loader beams (`MapTeleport.bmp` sprites, children of the loader boxes) and 51 area signs (`ExitMap_<area>.BMP` sprites; the tunnel's Industrial sign sits in `MAPLOADERS_Enabled` beside the box), set to zero size and opacity 0. Farm teleport, ZAP stops (`Trigger_Root_ZAP`, same textures, other entities) and mission loaders keep theirs. | `data/gameplay/seamless.py` (+ the row in `options.py`, the check in `builder.py`) |
| DLL `[seamless]` | A swap that walk loaders asked for skips the fade-out, the 0.3 s wait, the load screen, the progress bar, the 0.2 s black and the fade-in; the last frame stays on screen during the swap; every swap is timed in `TODGameplay.log`; near a loader the next blocks are read into the OS cache on a background thread. | `code/` (+ `dll/src/sites.h` section Seamless, `dll/HOOKS.md`) |

## Per border

| Border | Data change | DLL |
|---|---|---|
| Downtown 01 <-> Harbour 01 (bridge) | Downtown's box `TriggerLoad_SUBMAP01_Map04_LosToros_Train_Harbour` z -329.0 -> -539.99 (211 m south, x 3 cm, y kept); `Teleport_to` emptied on both sides. Box order wrong (gap -188 m) -> walk (23 m). | no load screen |
| Downtown 02 <-> Industrial 04, Industrial 02 -> Downtown 02 (tunnel) | Downtown's box `TriggerLoad_SUBMAP04_Map03_LosToros_Industrial` (-319.0, 482.4) -> (-464.1, 435.5) (152 m west through the tube); `Teleport_to` emptied in Downtown 02, Industrial 04 and Industrial 02. Wrong (-129 m) -> walk (23 m). | no load screen |
| 162 vanilla walk borders (city, jungle) | none | no load screen |
| Farm 01 <-> Farm 02 (pass) | none: Farm 01 and Farm 02 do not share the pass ground (different meshes, up to 20 m apart; neither has ground at the other's box) | vanilla (teleport batches are never seamless) |

## Testing (static checks, all PASS)

* Builder (every block): base round-trip, output re-parse, header / INFO identical, propCk / cmdCk from the tables,
  create records (node + type, order) identical in every fragment (save index rule), only planned (node, property)
  pairs changed, read-back; plus the seamless re-scan (teleports empty, moved centre = plan within 0.01 mm, rotation
  and size unchanged, other boxes untouched, box order walk with 23 m). GOG and TOD HD bases.
* `tools/verify_ground.py`: walkable ground in BOTH block sets (map + submap) along the corridor centre line +-3 m,
  from 15 m before the return box to the far side of the moved box, the same surface within 0.25 m (bridge 125
  points, tunnel 134 points, 0 missing).
* `tools/catalog_rerun.py`: the catalogue rule of the author's transition research, re-run: vanilla 175 volumes = 164 WALK / 7 TELEPORT / 1 ONEWAY /
  3 MISSION (reproduces the research); with the zip 168 / 2 (farm) / 2 / 3. Industrial 02's tunnel box counts as
  ONEWAY because Downtown returns to its overlapping twin Industrial 04 (as in vanilla).
* `tests/data/test_seamless.py` (6 tests), `code/test_seam.cpp` (45 checks), `dll/build.bat` (check_sites 38 sites /
  137 refs, the test host section 11).

## Tools

```
python -I tools\ground_probe.py  "C:\GOG Games\Total Overdose" bridge|tunnel|farm|jungle|all   ascii ground maps of both blocks
python -I tools\verify_ground.py "C:\GOG Games\Total Overdose"                                   the ground check above
python -I tools\catalog_rerun.py "C:\GOG Games\Total Overdose" <zip> --md out.md                  the catalogue re-run
python -I tools\make_prefetch_table.py "C:\GOG Games\Total Overdose" <zip> TODGameplay_seamless.txt
python -I tools\merge_ini.py <TODGameplay.ini> <template ini> section.key=value ...               used by the installer
```
`ground_probe` and `verify_ground` import collision readers from the author's research tools (not included; point
`TOD_COLLISION_TOOLS` at them). `catalog_rerun` needs only the vendored `transitions.py`.

## Build and install

The installer's Seamless World card does all of this. To build by hand:

```
cd data
python build_gameplay_mod.py --game "C:\GOG Games\Total Overdose" --set driving=better_feel --set difficulty=brutal --set seamless=on_nomarkers --out ..\work\seamless_install\todgameplay_data.zip
cd ..
dll\build.bat
python -I seamless\tools\make_prefetch_table.py "C:\GOG Games\Total Overdose" work\seamless_install\todgameplay_data.zip TODGameplay_seamless.txt
```

Then install the zip with `data\install_gameplay_mod.py`, copy the DLLs and `TODGameplay_seamless.txt` next to
TOD.exe and set `[seamless] enabled = 1`. Rebuild the table whenever the zip is rebuilt (the installer does this).
