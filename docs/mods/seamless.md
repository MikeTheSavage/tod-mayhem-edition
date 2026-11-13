# Seamless world (`[seamless]`, data option `seamless`)

## What it does

Walking across a map border (Downtown, Industrial, Harbour, Border) swaps the area without the fade, the wait and the
load screen. The last frame stays on screen during the swap, so a crossing is a short hitch. The bridge and the tunnel,
which were teleports, become walk borders too. Missions, taxis, ZAP stops, cut-scenes, deaths and menu loads stay
vanilla. Marked EXPERIMENTAL in the installer.

## How it is implemented

1. **Engine facts:**
   - The city is several blocks that swap when a loader volume is crossed. A walk loader's action (ActionType 21)
     appends `command_load_block` entries to LoadCtrl.
   - The processing state 0x74A8F0 pushes the load-screen state 0x748840, which fades out, waits 0.3 s, shows the load
     screen and progress bar, runs the synchronous swap (UpdateLoadedBlocks_Impl 0x8986E0), fades from black for 0.2 s
     and sends a fade-in.
   - The swap itself: submap read 146-472 ms on a cold disk, apply 27-100 ms, re-initialisation 50-70 ms.
2. **Classify the batch.** SL_WALKREQ 0x7E69EA tags the entries a walk loader appended. SL_LSPUSH 0x74A985 classifies
   the pending batch before the load-screen state is pushed: seamless only when every entry came from a walk loader,
   none has a teleport name, and all are map / submap folders.
3. **Skip the presentation** (only for a seamless batch):
   - SL_LSSKIP 0x7489A1: jump to the state's own no-fade path (no fade-out, no 0.3 s wait).
   - SL_LSSKIP2 0x748D0E: leave after the HUD / rumble / sound-shutdown messages (no load screen).
   - SL_PROGRESS 0x9CC4F4 (DATA: the progress bar's vtable slot): no draw, no present, so the last frame stays.
   - SL_POSTFADE 0x747CA5: no black fade node and no 0.2 s wait.
   - SL_FADEIN 0x747E3A: no fade-in message.
   Kept: the world pause, the ambient sound shutdown / resume pair, the unpause.
4. **Timing.** SL_SWAP 0x89A819 and SL_SWAP_RG 0x874ECA (the save-load reload pass) time every swap into the log.
5. **Prefetch.** `TODGameplay_seamless.txt` (172 loader volumes, 20 blocks, built for the installed zip by
   `seamless/tools/make_prefetch_table.py`). A 4 Hz check of Ram's position in the input tick; near a loader a
   low-priority background thread reads the next blocks once per `prefetch_repeat` s into the OS file cache (file
   reads only).
6. **Data option `seamless`** (`data/gameplay/seamless.py`, set operations only):
   - Bridge: Downtown 01's loader box moved 211 m south to z -539.99; `Teleport_to` emptied on both sides.
   - Tunnel: Downtown 02's box moved 152 m west through the tube; `Teleport_to` emptied in Downtown 02, Industrial 04
     and Industrial 02. Ground was checked in both block sets along both corridors (`seamless/tools/verify_ground.py`).
   - The farm pass stays a teleport: the two farm blocks never share ground there.
   - `on_nomarkers` also hides the 103 border markers (52 loader beams, 51 area signs) at zero size and opacity 0.

## Ini keys (`[seamless]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature |
| `prefetch` | 1 | read the next blocks into the file cache near a loader |
| `prefetch_distance` | 60 | m from a loader volume |
| `prefetch_repeat` | 180 | s before the same block is read again |
| `log` | 1 | one log line per load decision and per swap |

## Testing and limits

- Unit tests `seamless/code/test_seam.cpp` (45 checks); data tests `tests/data/test_seamless.py`; the host test drives
  one swap through every stub. A catalogue of every loader (`seamless/tools/catalog_rerun.py`) gives 168 walk,
  2 teleport, 2 one-way and 3 mission borders.
- Played in game over several sessions. Two crashes during quick Downtown 01 / 02 swaps led to the engine double-free
  fix ([crash_fixes.md](crash_fixes.md)); with the fix, 40 swaps 0.6 to 2.0 s apart ran clean on the test copy.
- Expected hitch: about 0.15 to 0.25 s warm, 0.3 to 0.7 s cold. A zero-hitch border would need async reads inside the
  loader or merged submaps.
- The prefetch table is built for the installed zip. If the data zip is swapped without rebuilding the table (the
  installer rebuilds it), prefetch only logs "skipped".
- The in-game checklist is `seamless/TEST_IN_GAME.md`.

## Files

`seamless/code/seam_game.cpp`, `seam_logic.cpp/.h`, `test_seam.cpp`, `data/gameplay/seamless.py`, `seamless/tools/`
(`ground_probe.py`, `verify_ground.py`, `catalog_rerun.py`, `make_prefetch_table.py`, `merge_ini.py`),
`seamless/README.md`, `seamless/TEST_IN_GAME.md`, `dll/HOOKS.md` "Seamless border swaps".
