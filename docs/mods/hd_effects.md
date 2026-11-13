# HD effects (TOD HD's d3d9.dll in Gameplay+)

## What it does

The graphics effects of the TOD HD remaster that need no new art, on the player's normal GOG game: anisotropic
filtering, ambient occlusion, a post pass (grade, sharpen, FXAA), sun shafts and mist, better particles, per-pixel
lighting with material classes, screen-space reflections, procedural water, volumetric light beams and real-time
shadows. Off unless picked in the installer.

## How it is implemented

1. **No fork.** The TOD HD `d3d9.dll` proxy is used exactly as released (2026-10-01, sha256
   `c34aab17...ee5f8f`, pinned in `hdfx/tools/hdfx_stage.py` and checked before every install). It hooks the D3D9 device
   and USER32 import slots; it shares no code site with TODGameplay.dll.
2. **Split the features.** (A) = no new art: shader-only, procedural, or driven by data recomputed from the player's own
   files. (B) = needs TOD HD's art (HDRIs, scans, models, sheets) or the lighting rebake. Only (A) ships. The full table
   is in `hdfx/README.md`.
3. **The ini.** `hdfx/tools/make_ini.py` writes a Gameplay+ `tod_hd_fx.ini` from the TOD HD release ini: every (B) key 0,
   the window / resolution keys off (`borderless`, `dpi_aware`, `resolution_lock` = 0, so it never touches
   SetResolution), and a lighter colour grade (TOD HD's was tuned on its HD textures and looked too orange on vanilla
   ones). Lighting is mode 1 (the game's bake kept, the level's sun added per pixel), not TOD HD's mode 2, because
   without per-level HDRI skies mode 2 leaves non-jungle levels without fill light.
4. **Build the data on the player's PC.** `hdfx/tools/hdfx_regen.py` reads the archives configpc.txt lists (after the
   data zip step) and writes: the material-class manifest (3,335 rows), the sprite manifest (161), the jungle sky-face
   manifest (5), and `sh.txt` (sky irradiance baked from the game's own jungle skybox, not an HDRI). These hold hashes
   and bytes of game textures, so they are never shipped. TOD HD's own copies describe its HD texture pack and would
   not match vanilla anyway.
5. **Shipped payload** (`hdfx/payload/`): TOD HD's own data only (material rows, name lists, measured cone shapes and
   triplanar scales) and two procedurally generated water textures.
6. **Logs.** The d3d9.dll logs to `TODHD_LOG_DIR`. The dsound.dll loader sets it to
   `%LOCALAPPDATA%\TODGameplay\hdfx_logs` (or `[loader] hdfx_log_dir`) in its DllMain, before d3d9.dll writes its first
   line, so nothing lands in the game folder or in a TOD HD install folder.
7. **Missing data is safe.** Every effect checks its file and stays off if it is missing (no `sh.txt` -> no lighting).

`sun_shadows` cannot come over: it needs MM010's offline Cycles rebake. `static_shadows` is the live replacement (off
by default).

## Installer sub-toggles (`tod_hd_fx.ini` keys)

| Sub-toggle | Keys (on / off) | Default |
|---|---|---|
| Texture filtering | `aniso_all = 16 / 1` | on |
| Ambient occlusion | `ssao = 1 / 0` (HBAO) | on |
| Post: sharpen, FXAA, grade | `post = 1 / 0` | on |
| Light shafts, mist, volumetric beams | `light_shafts`, `mist`, `volumetric_beams`, `volumetric_light` | on |
| Particles | `particle_mips`, `soft_particles`, `part_lit`, `heat_haze` | on |
| Lighting | `lighting_mode = 1 / 0` | on |
| Water (needs Lighting) | `water_mode = 1 / 0` | on |
| Real-time shadows (needs Lighting) | `dyn_shadows = 1 / 0` | on |
| Static world shadows (needs real-time shadows) | `static_shadows = 1 / 0` | off |
| Frame cap 60 | `fps_cap = 60 / 0`; on also sets `[timing] fps_limit = 0` | on |

A player's own edited values in `tod_hd_fx.ini` survive a reinstall. Another program's `d3d9.dll` is set aside as
`d3d9.dll.before_hdfx` and put back when the group is off. A TOD HD install with its own ini is refused.

## Testing and limits

- Installer tests `installer/tests/test_installer.py` (HdfxPlan / HdfxInstall: every (B) key 0, the DLL is the pinned
  release, the shipped ini has every key the installer sets).
- Checked in game on a test copy of the game (with an isolated LOCALAPPDATA): MM010 jungle and
  Downtown, as vanilla / Gameplay+ / Gameplay+ with HD; each effect on its own; AO and shadow debug views; a front-end
  resolution change 1920x1080 -> 2560x1440 with a clean device Reset.
- In the combined test: an in-game SETTINGS > SCREEN RESOLUTION switch 1920x1080 -> 1920x1200 with the effects on
  (Reset hr 0, targets released and remade at the new size).
- Cost: all effects together add about 1.1 to 1.8 ms to an 18 to 20 ms frame in Downtown (single effects drown in
  window noise). Memory: Downtown about 391 MB private with HD against 329 MB without.
- Not verified: exclusive fullscreen and borderless with the effects on; levels other than MM010 and Downtown; water
  on screen; a long session; the installer step on a real game folder.
- Re-syncing with a newer TOD HD release: see the re-syncing steps in `hdfx/README.md`. Never write into a TOD HD
  install folder.

## Files

`hdfx/release/` (the pinned d3d9.dll and its release ini), `hdfx/payload/`, `hdfx/tools/` (`make_ini.py`,
`hdfx_regen.py`, `hdfx_stage.py`), `hdfx/README.md`, `dll/loader/dsound_proxy.cpp`
(`set_hdfx_log_dir`), `installer/gpi/plan.py` (`HDFX_SUBS`, `HDFX_FILES`), `installer/gpi/engine.py` (step `hdfx`).
