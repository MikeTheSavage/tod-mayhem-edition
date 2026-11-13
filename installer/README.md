# Gameplay+ Installer

A visual installer for TOD Gameplay+ (Total Overdose, 2005, GOG PC). It is the way to install, update and uninstall
the mod.

Launch: double-click **`Gameplay Plus Installer.cmd`** in the repo root. It starts a small local server
(127.0.0.1, a free port) and opens it in Microsoft Edge app mode (a plain window), or in the default browser. The
server quits about 45 s after the window closes, unless an install is still running.

```
python -I installer\app.py [--game DIR] [--port N] [--no-browser] [--stay]
                           [--sandbox DIR] [--no-registry] [--music-dir DIR] [--documents DIR]
```
`--sandbox` refuses every game folder and backup location outside DIR, and `--no-registry` never touches
`HKCU\Software\Eidos\Total Overdose`. Both are for tests on scratch copies. `--documents DIR` (or the environment
variable `GPI_DOCUMENTS`) names the Documents folder whose `Total Overdose\savegames\TODGameplay_display.txt` the
Display group may replace: tests pass a fake one. A `--sandbox` run without it never touches any Documents folder (and
a `--documents` outside the sandbox is ignored).

## What it does

1. **Checks.** It refuses to install or restore while `TOD.exe` / `TotalOverdose.exe` runs from that game folder
   (process list; a TOD process whose path cannot be read also blocks). It also checks:
   - `TOD.exe` against the GOG build (6,860,800 B, md5 `d9beb351b3c78ee9f14a766a368f3ff9`); with any other exe it
     warns that the DLL features stay inactive. TOD.exe is never modified.
   - for a foreign `dsound.dll`;
   - free space on every drive involved.
2. **Backup.** Either the whole game folder (default) or only the files the installer owns. A `manifest.json` holds
   the sha256 of every copy, and every copy is read back and verified before anything is installed. The default
   location is `Gameplay+ Backups` next to the game folder; any folder outside the game folder can be chosen.
3. **Data zip, built on the player's machine** from their own game files with `data\build_gameplay_mod.py`. Game data
   is never shipped. The static `difficulty` option is always `normal`: the DLL applies the levels at run time and the
   in-game menu picks them. The zip is installed with `data\install_gameplay_mod.py`, which handles the configpc.txt
   `filesystem` line.
4. **Seamless prefetch table** `TODGameplay_seamless.txt`, rebuilt for the new zip
   (`seamless\tools\make_prefetch_table.py`).
5. **DLLs** from `dist\`: `TODGameplay.dll` and the `dsound.dll` loader when any DLL group is on, and `TODPhysics.dll`
   only with Box3D physics. A DLL no group needs is removed. `TODGameplay.map` / `TODPhysics.map` (the linker maps of
   the same build: the hang watchdog names the stuck code from them) are copied and removed with their DLL.
6. **`TODGameplay.ini`**, merged with `seamless\tools\merge_ini.py`:
   - every line the player already has stays;
   - only the keys in the table below are set;
   - a group that is off is switched off (`enabled = 0`), never deleted;
   - a missing section is copied from `dist\TODGameplay.ini`.
7. **HD effects** (when on): stage, build the manifests from the player's own archives, copy (`hdfx\README.md`).
8. **Minimap** (Minimap group, map ticked): `python -I hud\tools\bake_minimap.py --game <game> --out
   <game>\TODGameplay_minimap` as a child process, which calls `dist\TODMinimapBake.exe` (the 9 maps the first time: 2.5
   min and 23 MB on the dev machine, 8 threads; 2-5 min is the text on the page). A second run skips unchanged maps
   through its `bake.json`; a new bake version re-bakes everything once, so an existing install gets the new maps. Its
   own progress step (map n of 9). A failed bake does not stop the install: the step shows a warning, a map that failed
   keeps the picture it had (the DLL still draws an older bake's PNG), the radar stays a radar without one, and the
   markers still work; install again to retry.
9. **Display** (16:9 group): configpc.txt `fullscreen` / `screensize`, and the registry `XRes` / `YRes` (exported to
   `eidos.reg` in the backup first). These are only the FIRST start's default: with `[display] mode_menu = 1` the
   game's SETTINGS > SCREEN RESOLUTION lists the real modes and keeps the pick in
   `<Documents>\Total Overdose\savegames\TODGameplay_display.txt`, which wins at every later start. When the player
   picks a start mode in the installer (clicking a radio ticks "Use it at the next start"; the box shows only when that
   file exists), the installer copies that one file into the backup (`side\TODGameplay_display.txt`, sha256 in the
   manifest), reads it back, and only then deletes it, so the pick applies. **This is the only file outside the game
   folder the installer ever writes or deletes; no other save file is touched.** The pick applies once: the saved
   selection never has it ticked. A restore puts the file back only when there is none (a mode kept in game since is
   never overwritten).
10. **`todgameplay_difficulty.txt = menu`** when the difficulty menu is on; removed when it is off.
11. **`Gameplay+ install.json`** in the game folder holds the selection, which is pre-loaded next time. Without it, the
   installer pre-loads what it detects in the ini and the data manifest.

Every file it writes goes to a temp file next to the target first and is then swapped in with `os.replace`. A file
that is hard-linked elsewhere is never written into. Each run's log is `install.log` in its backup folder
(`restore.log` for restores).

**Uninstall / Restore:** pick a backup. It is verified against its manifest, the files that differ are copied back,
and the installer-owned files the backup did not have are removed. Restoring the oldest backup (made before the
first install) removes Gameplay+ completely. Switching every group off and installing also turns everything off
cleanly.

## Groups: what each one sets

| Group (card) | Data options (zip) | TODGameplay.ini (on) | Files | Off writes |
|---|---|---|---|---|
| Essentials / Fixes (default on; "Game may crash without these." when off) | - | Crash fixes: `[fixes] vector_realloc, weapon_owner`; Stability fixes: `[timing] clock_fix, pickup_step_fix`, `[controls] mouse_turn_fix, diagonal_normalize` (each 1, or 0 when its box is unticked) | DLL + loader | fixes `= 0` (they default to on in the DLL), the others `= 0` if present |
| Display | - | `[display] enabled = 1, mode_menu = 1, borderless = 0/1` (Start in: windowed 1920x1080 / borderless native; note "change it in game any time: SETTINGS > SCREEN RESOLUTION") | configpc `fullscreen`, `screensize` (1920,1080 or the monitor size), registry XRes/YRes; on a start-mode pick: `Documents\Total Overdose\savegames\TODGameplay_display.txt` backed up, then deleted | `[display] enabled = 0` |
| Controller | `controller = menu` | `[pad] enabled = 1, prompts = auto/keyboard/xbox/playstation`; Rumble 0..200 %: `rumble = 0/1`, `rumble_strength = %/100` | DLL | `[pad] enabled = 0` |
| Better Driving | `driving = better_feel`; Chase camera: `vehicle_camera = far` | optional `[vehicles] steering_return_rate = 0.25`, `s_brake_parity = 1` (experimental) | DLL only with those | `[vehicles]` keys `= 0` |
| Seamless World **(EXPERIMENTAL)** | `seamless = on_nomarkers` (or `on`) | `[seamless] enabled = 1, prefetch = 0/1` | `TODGameplay_seamless.txt` | `[seamless] enabled = 0`, table removed |
| Suave / Piñata Loco **(EXPERIMENTAL)** | `suave_anims = on`, `suave = on_slot10` (or `on` = slot 11), `suave_music = off` when no song file is given | `[suave] enabled = 1, slot = 10/11, music = 1/0` (1 only with the player's song file) | DLL; the music segments cut from the player's own copy of the song go into the data zip (see "The Suave song") | `[suave] enabled = 0` |
| Smart AI | - | `[smart_ai] enabled = 1, squad = 0/1, morale = 0/1` | DLL | `[smart_ai] enabled = 0` |
| Bullet Time & Quick Restart | - | `[bullettime] enabled = 1, key = <key>`, `[restart] enabled = 1, key = <key>` (key fields: press a key or a middle / side mouse button; only names TODGameplay.dll knows are written) | DLL | both `enabled = 0` |
| Minimap (default off) | - | Map in the radar: `[minimap] enabled = 1, map = 1, rotate = 1/0` (turns with the view / north up); Objective markers: `objective_marker`, `world_marker` = 1/0 (`[minimap] enabled = 1` with the map or the markers). An older ini's `[weapon_wheel] enabled` goes to 0 with this card on or off (`retired` key: the section is not in the shipped ini, the DLL ignores it anyway) | `TODGameplay_minimap\` baked by `hud\tools\bake_minimap.py` from the player's files (only with the map); the installer owns exactly the files that tool makes (`minimap.txt`, `bake.json`, each map's `.mmp` and `.occ` or an older bake's `.png`, their `.tmp` leftovers) | `enabled = 0`; the bake's files removed (a file the player put into that folder stays), the folder if empty |
| HD HUD icons (default on) | `hud_textures = on` | - | none of its own: 57 HUD textures (pickup coins, ammo icons + bullet tick, the 28 weapon cards, the combo skull) inside `todgameplay_data.zip` (about 28 MB bigger), from `hud_textures\art\` | data option `off`: the zip without it |
| HD Effects (default off) | - | `[timing] fps_limit = 0` when its frame cap is on; the effects' own keys go to `tod_hd_fx.ini`: texture filtering `aniso_all = 16/1`, AO `ssao`, post `post`, shafts + mist `light_shafts mist volumetric_beams volumetric_light`, particles `particle_mips soft_particles part_lit heat_haze`, lighting `lighting_mode = 1/0`, water `water_mode`, real-time shadows `dyn_shadows`, static shadows `static_shadows` (off), frame cap `fps_cap = 60/0` (`hdfx\README.md`) | `d3d9.dll` (TOD HD's, unchanged), `tod_hd_fx.ini` (the player's own values kept), `tod_hd_light.txt`, `tod_hd_fx\` (manifests and `sh.txt` built from the player's archives); another `d3d9.dll` is kept as `d3d9.dll.before_hdfx`; needs the DLL loader (log folder) | those files removed, `d3d9.dll.before_hdfx` put back |
| MAYHEM: Physics | - | `[physics] enabled = 1` (`shell_impulse = 450`, `blast_lift = 4` left as they are) | `TODPhysics.dll` + `TODPhysics.map` | `[physics] enabled = 0`, DLL and map removed |
| MAYHEM: Ragdoll **(EXPERIMENTAL, needs Physics)** | - | `[ragdoll] enabled = 1, impulse_scale = 0.5..3` ("Drama", default 1.0) | `TODPhysics.dll` (with the ragdoll API) | `[ragdoll] enabled = 0` |
| MAYHEM: Car Physics **(EXPERIMENTAL, needs Physics)** | - | `[car_physics] enabled = 1, impulse_scale = 0.5..3` (default 1.0) | `TODPhysics.dll` | `[car_physics] enabled = 0` |
| MAYHEM: Chain Reactions **(EXPERIMENTAL, needs Physics)** | - | `[mayhem] chain_reactions = 1, chain_cars = 1/0` | `TODPhysics.dll` | `[mayhem] chain_reactions = 0` |
| MAYHEM: Breakable Cover **(EXPERIMENTAL, needs Physics)** | - | `[mayhem] breakable_cover = 1` | `TODPhysics.dll` | `[mayhem] breakable_cover = 0` |
| MAYHEM: Mayhem Heat | - | `[mayhem_heat] enabled = 1, escalation = 1/0, escalation_scale = 0.25..2` (only with escalation), `decay_seconds = 5..30`, `tint = 1.0/0.0`, `icons = 1/0`, `score_multiplier = 1/0`, `score_label = 0` (always: the label is gone) | DLL | `[mayhem_heat] enabled = 0`, `score_label = 0` (if present) |
| MAYHEM: Difficulty (HARD / BRUTAL / PSYCHO) | `difficulty_menu = on` (or `on_all_languages`), `difficulty = normal` | `[difficulty] enabled = 1`; PSYCHO loco moves (sub-option, default on): `[psycho] enemy_loco_moves = 1` (unticked or the group off: `= 0` when present) | `todgameplay_difficulty.txt = menu` | `[difficulty] enabled = 0`, marker removed |
| MAYHEM: More Enemies (needs Difficulty) | - | `[difficulty] max_live_enemies = 6..9` (default 9) | - | `max_live_enemies = 0` |
| MAYHEM: Bigger Fights **(EXPERIMENTAL, needs Difficulty, old saves do not load)** | `bigger_fights = on` | `[bigger_fights] enabled = 1` | DLL, a data zip about 80 MB bigger | `[bigger_fights] enabled = 0` (`if_present`), data option back to off: the zip without it, `Slot00..` come back |
| MAYHEM: Gang Wars | - | `[gang_wars] enabled = 1, provoke = 1/0, spark = 0/1` | DLL | `[gang_wars] enabled = 0` |
| MAYHEM title (automatic) | - | `[mayhem_title] enabled = 1` while any Mayhem group is on; only if the shipped ini has that key | `TODGameplay_mayhem.dds`, `TODGameplay_mayhem_fx.dds`, `TODGameplay_mayhem_fire.dds` from `dist\` | `enabled = 0`, the three files removed |

The "coming soon" chip row hides itself while `catalog.COMING_SOON` is empty (it is empty now: every group is a real
toggle). No `debug_*` key is ever written (a test checks every set key against
`dist\TODGameplay.ini` and the `debug` prefix). Of the new groups only Bigger Fights has a data option: for the others the data zip is the same as
without them.

- `requires` (a card): Ragdoll, Car Physics, Chain Reactions and Breakable Cover need Physics (`[physics] enabled = 1`
  + `TODPhysics.dll`), More Enemies needs Difficulty. The switch is locked, the card dashed and dim with "Needs PHYSICS
  on.", and the plan turns the group off too. (Ragdoll could run without it: `physics\code\phys_game.cpp`
  `g_world_only` builds only the level world when `[physics] enabled = 0` and `[ragdoll] enabled = 1`; the installer
  still models it as needing Physics.)
- `requires` (a sub-option, a string): HD water / real-time shadows need Lighting, static shadows need real-time
  shadows, map rotation needs the minimap, Reinforcements needs "More
  enemies with more heat". Greyed out and locked on the page; the plan applies the same rule.
- EXPERIMENTAL: Seamless World, Suave / Pinata Loco, Ragdoll, Car Physics, Chain Reactions and Breakable Cover. The
  last four are built on the [physics] PROTOTYPE world and have unverified parts (enemy stand-up behind broken cover,
  cars as chain links tested only on the host, the corpse delay and checkpoints). "Static world shadows" has a small
  tag. Mayhem Heat, Gang Wars, PSYCHO loco moves and the HUD group were checked in game and are not tagged.
- Fire height: the design draws three (`.heat-1..3`); with nine Mayhem cards it is min(3, toggles on). The music is
  unchanged: one stem per toggle, stem 4 with the 4th toggle (or all on).

An earlier installer version had a camera / slow-motion tuning card; its vehicle camera is now Better Driving > Chase
camera. A selection saved by that version still loads (rumble on/off + strength becomes the percentage).

## The look

The page uses `web\installer.css`, the stylesheet of the installer design, as drawn, and `web\app.css` for what the design does not draw (status
badges, the EXPERIMENTAL hazard tag and note, the "what it changes" list, the backup kind, footer messages, the
credit line, the log, the restore confirm). The markup is plain HTML built by `web\app.js` (no component
runtime).

- Fire height = Mayhem toggles on (0-3) as `.heat-N` on the section, `.win-heat-N` on the window.
- `prefers-reduced-motion` swaps the flames for static embers; `?rm=1` forces it (`.rm`).
- Focus ring 3px #33E3D4, 3px offset; a card lights up when anything inside has focus.
- Sizes: 1280x800 and 1024x700 (container queries in installer.css).
- Fonts: Barlow Condensed, Barlow Semi Condensed, Rokkitt (SIL OFL 1.1), latin woff2 in `web\fonts\` (offline;
  `SOURCES.txt` has the URLs, `OFL.txt` the licence).
- View parameters (screenshots / tests only; they never start a job): `?open=a,b` opens card details,
  `?mayhem=N` sets N Mayhem toggles on screen, `?on=a,b` / `?off=a,b` switch cards on / off on screen, `?scroll=mayhem`, `?screen=restore|done|progress|error`, `?rm=1`,
  `?shot=1` (one job poll, for headless screenshots).

What the design drew that is not wired, and why:
- Seamless "Free roam between missions" / "Skip mission menu", Smart AI "Flanking" / "Takes cover": no such features
  exist; the cards show the real options instead (border markers, prefetch; squad tactics, morale).
- Suave "Suave moves" / "Piñata loco move" checkboxes: there is one move, and the choice is its slot (10 replaces the
  Piñata, 11 is Tequila), shown as two radio buttons.
- Better Driving "Tighter steering": the real experimental options are shown instead (gradual steering return, S
  brakes like V); "Chase camera" is wired (vehicle camera 1.5 m back).
- The progress screen's Cancel: a running install cannot be stopped safely half way, so there is none.

## Files

| Path | What |
|---|---|
| `app.py` | entry point: arguments, server, Edge app window |
| `gpi/catalog.py` | the groups, sub-options, dependencies (`requires` of cards and of sub-options), coming-soon list (empty) |
| `gpi/plan.py` | selection -> plan (data options, ini keys with set / if_present, files, display) |
| `gpi/engine.py` | the install and restore jobs (steps, log, checks, ini merge, configpc display lines) |
| `gpi/backup.py` | backups, manifests, verify, restore (hard-link safe writes) |
| `gpi/gamestate.py` | game folder detection (GOG registry + default path), exe check, installed state, space |
| `gpi/procs.py` | running-game check (psapi via ctypes) |
| `gpi/server.py` | the local HTTP server and JSON API |
| `gpi/repo.py` | where the repo's tools, `dist\` and the web files are (repo or PyInstaller bundle) |
| `web/index.html`, `web/app.js` | the page (markup of the design, built from the catalogue) |
| `web/installer.css` | the design's stylesheet and tokens, as drawn |
| `web/app.css` | additions for what the design does not draw |
| `web/fonts/` | Barlow Condensed, Barlow Semi Condensed, Rokkitt (OFL), `SOURCES.txt`, `OFL.txt` |
| `web/mayhem_music.js` | the Mayhem music player (self-contained; see below) |
| `web/music/` | the licensed track's stems and `music.json`. Produced separately: this installer only reads them |
| `tests/test_installer.py` | unit tests (plan mapping, ini merge, backup / restore round trips, running-game refusal, install job) |
| `tests/run_music_tests.py`, `tests/music_stems_test.html` | stem player tests in headless Edge with generated test tones (never shipped) |

Tests: `python -I installer\tests\test_installer.py` and `python -I installer\tests\run_music_tests.py`.

## Mayhem music

`web/mayhem_music.js` plays synchronised looping stems from `web/music/`: `stem1_guitar.ogg`, `stem2_bass_perc.ogg`,
`stem3_trumpet.ogg`, `stem4_strings_choir.ogg`, plus `music.json` (stem order, `bpm`, `time_signature` (3/4),
`loop_length_samples` + `sample_rate`, `credit` or `title` / `artist` / `license` / `url`).

- **Layers:** each Mayhem toggle that is on adds one stem: 1 = stem 1, 2 = stems 1-2, 3 = stems 1-3. Stem 4 joins
  when every Mayhem toggle is on, or with a 4th toggle.
- **Sync:** all stems start at the same AudioContext time with `loop = true` and keep running while any toggle is on,
  so they stay sample-locked. A stem fades in or out over 1.2 s starting at the next bar line.
- **Bus fades:** the whole bus fades in over 0.8 s with the first toggle and out over 1.5 s after the last one; then
  the sources stop.
- **Missing files:** missing stems are silent, with no errors.
- **Controls:** mute and volume are remembered in `%LOCALAPPDATA%\Gameplay Plus Installer\prefs.json`. Nothing plays
  before the first click.

**Credit:** the footer shows the credit line from `music.json`. For a CC-BY track, copy it here as well:

> Music: La Llorona (traditional), arranged for Gameplay+. Traditional Mexican song, public domain. Arrangement:
> Gameplay+. Instruments rendered with the GeneralUser GS SoundFont (S. Christian Collins). (From `web/music/music.json`.)

## Packing into one .exe (PyInstaller, for publishing)

`installer\pack_exe.cmd` builds `TOD Mayhem Installer.exe` (`--onefile --noconsole`). Run it from the repo root, or
better from a clean export (`git archive HEAD | tar -x -C <dir>`, so no untracked file can slip in), with a venv outside
the repo that has `pip install soundfile numpy pyinstaller` (`set PACK_PY=<venv>\Scripts\python.exe`, `set PACK_OUT=<dir>`).
The repo layout is kept inside the bundle (`gpi/repo.py` uses `sys._MEIPASS` when frozen):

```
set PACK_PY=<venv>\Scripts\python.exe
set PACK_OUT=<output folder>
installer\pack_exe.cmd
```

What is in the exe: the installer page and code, `data\` (builder and codecs), `dist\` (DLLs, maps, ini, the Mayhem
title images, the minimap bake exe), `suave\anim\clips\game`, `hud_textures\art`, `hdfx\release\d3d9.dll` (the TOD HD
effects DLL), `hdfx\payload`, `hdfx\tools` (stage / regen / gamedata only), `hud\tools` (bake + geo), `seamless\tools` (merge_ini, prefetch
table, the vendored `transitions.py`), `suave\audio\fingerprint.json`, plus Python, numpy and soundfile with its
libsndfile DLL. **Not** in it: `suave\audio\build` (the song segments), any mp3 / wav / flac, `hdfx\tests`, the hdfx dev tools,
`hdfx\release\tod_hd_fx.ini`, any game archive. List the bundle with
`pyi-archive_viewer` and check it for `.mp3` / `.wav` / `.flac` / `.naz` and `suave_mus` before publishing.

Third-party code in the exe: soundfile (BSD-3) with libsndfile (LGPL-2.1+; dynamically loaded, replaceable DLL in the
unpacked bundle), numpy (BSD-3), cffi (MIT), pycparser (BSD-3), Python (PSF), PyInstaller bootloader (GPL with the
bootloader exception). Their licence texts go next to the exe in `licenses\`.

Before publishing:
- The music stems under `web\music` (La Llorona, arranged for Gameplay+) are the project's own; the song the Suave move
  uses is NOT shipped: the player points the installer at their own copy (see "The Suave song" below).
- `data\lib` holds the fragment codec; it is the repo's own code. `seamless\tools\transitions.py` is the catalogue rule of
  the author's research tool, vendored into the repo.
- Test the packed exe on a scratch copy first: `--sandbox` and `--no-registry` work there too.

The packed exe has no Python for subprocesses. The installer imports the builder, `merge_ini`, the prefetch tool and
`install_gameplay_mod` in-process (packed, the minimap bake too; from the repo the bake runs as a child process); only the folder / file picker (PowerShell) and `reg export` / `reg import` run as
programs.

## The Suave song

The Suave move's music is four 17.4 s segments of a commercial recording, so they never ship. The Suave card has
**Your copy of Suavemente**: a file picker (mp3, wav, flac, ogg, opus, aiff, w64, caf: what libsndfile 1.2 reads; not
m4a / aac / wma). Picking a file runs `/api/song` (read + match only, shown under the picker); the install repeats it as
the step "Suave music from your copy of the song", **before the backup**, so a refused file changes nothing.

- `suave\audio\song_import.py` (numpy + soundfile, no ffmpeg) reads the file, mixes to mono (ffmpeg's 0.7071 * (L + R)),
  resamples to 44.1 kHz when needed, finds where the song starts in the file, cuts the 4 segments exactly as
  `make_segments.py` does (beat grid, fades, level), and writes Ogg Vorbis (libvorbis through libsndfile) +
  `manifest.json` into `%LOCALAPPDATA%\Gameplay Plus Installer\suave_music` (a per-user cache, never the game folder;
  `<sandbox>\Gameplay Plus Installer\suave_music` in a sandboxed run). The builder reads that manifest
  (`build(..., suave_manifest=...)`), checks the oggs with `ogginfo` and puts them into the data zip.
- `suave\audio\fingerprint.json` (made once from the reference mp3 by `make_fingerprint.py`) holds the beat grid, the
  segment list, the level target (`reference_rms_db`, the game's El Mariachi stream, so the player's game is not needed
  at import) and a 3.3 ms-hop, 1 dB-step loudness envelope of the first 60 s and of segments 3 and 4. It is not audio.
  The import cross-correlates the player's envelope with it (offset accurate to about 0.2 ms), requires the two late
  anchors to agree (within 60 ms), and refuses a wrong song, an edited or shortened version, or a file that is too
  short, with a plain message.
- No song file: the card says so, `suave_music = off` (no stream records, no oggs) and `[suave] music = 0` are written;
  the DLL never starts a segment (`Cfg::segment_for` returns 0 with `music = false`), so no DLL change is needed. The move
  plays on its timing without music.
- To rebuild the build folder by hand: `python suave\audio\song_import.py "<your file>" --out suave\audio\build`.
