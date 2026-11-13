# Display fix: 16:9 and wide screens (`[display]`)

## What it does

The game keeps the wide size you set instead of jumping back to a 4:3 size after the first frame. The in-game HUD
moves to the real screen edges. Menus, subtitles, load screens and the front end keep their 4:3 layout with square
pixels instead of being stretched 33 % too wide, and the mouse still hits the menu items. Exclusive fullscreen works
at wide sizes, and a borderless window is available.

## How it is implemented

1. **Engine facts:**
   - The resolution table has five fixed 4:3 sizes (640x480 .. 1600x1200), built at 0x4AB940.
   - After the first frame, 0x769ABD applies the saved SETTINGS index from Slot11 (`Setting_ScreenSize`) and overrides
     configpc.txt. A saved index 4 lands at 1600x1200; a fresh profile (index 1) at 800x600.
   - The HUD is a 640x480 canvas with square pixels: at 16:9 a 1440x1080 box in the middle.
   - Menus and text are an 800x600 canvas stretched to the screen (x2.4, y1.8 at 1920x1080).
   - Exclusive fullscreen only knows a fixed 4:3 mode list and falls back to 640x480 for any other size.
2. **Resolution table and start size.** DS_RES_W0..H4 (10 operand patches at 0x4AB977..0x4ABA0B) replace the five
   sizes (`res_table = auto`: 1280x720, 1600x900, 1920x1080, 2560x1440, index 4 = the configpc size). DS_RES_START
   0x769ABD logs the saved index instead of applying it (`startup_size = config`). No save is touched.
3. **Wide HUD.** DS_WH_DIM 0x72AFD6 and DS_WH_DOT 0x6DB6F4 make `is_wide_screen` (0x485640) answer true. Operand
   patches set the canvas height 480 / `hud_scale` (0x72AFE5), the width from the back buffer (0x72B060, 0x72B083,
   0x72B0A8), the aim dot (0x6DB714, 0x6DB72A) and the border (0x719108).
4. **Fitted menus.** DS_SPR_MTX 0x8F4D20 and DS_SPR2_MTX 0x8F6EED (both Sprite render paths) give the scale matrix a
   translation and pick, per sprite rectangle: fit (box), full-width band (`bands`), or full screen (`backgrounds`).
   Text: DS_TXT_SCALE_A/B (0x8FE7DF, 0x8FEFE2) return the font scale with its lowest mantissa bit set as a tag; four
   DrawString joins (0x41B5C8, 0x41B805, 0x41BD82, 0x41BFBF) add the offset only for tagged fonts. DS_MOUSE 0x8F4AD4
   applies the inverse fit to the cursor. DS_LOADBAR 0x87BA5E moves the load bar's raw-pixel quad onto the fit.
5. **Culling.** `cull_fix = 1` turns on `[camera] cull_fix` (CAM_ASPECT 0x87E054): no popping at the 16:9 edges.
6. **Fullscreen and borderless.** DS_FS_LOOKUP 0x45BE42 and DS_SR_LOOKUP 0x45BFCD add an entry for any size the adapter
   offers. DS_BL_INIT 0x45EBE2 sends fullscreen init to the windowed init, and DS_BL_WIN1/2 (0x45BF08, 0x45C02C) give
   the window a popup frame centred on (or covering) its monitor. `dpi_aware` makes the sizes physical pixels.
7. Everything is patched at start; nothing runs per frame except the menu and HUD stubs when the game calls them.

With `mode_menu = 1` (the default) the resolution is chosen in game instead: see [resolution_menu.md](resolution_menu.md).

## Ini keys

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature |
| `res_table` | auto | the five table sizes (auto, a list of five, or `off`) |
| `startup_size` | config | `config` keeps configpc's size; `saved` = vanilla |
| `wide_hud` / `hud_scale` / `border` | 1 / 1.0 / vanilla | HUD to the edges, its size, corner margin |
| `menus` / `bands` / `backgrounds` | 1 / stretch / cover | 4:3 fit for menus and text; full-width bars; full-screen art |
| `cull_fix` | 1 | culling with the real aspect |
| `fullscreen_any_mode` | 1 | exclusive fullscreen at any adapter size |
| `borderless` | 0 | configpc `fullscreen = true` as a borderless window |
| `dpi_aware` | on with borderless or mode_menu | physical pixels |
| `log` | 1 | log lines |

## Testing and limits

- Unit tests `tests/dll/test_display.cpp`, host test 13 (off = untouched; every stub run on fake frames).
- Checked in game on a test copy of the game, windowed 1920x1080: HUD at the edges, menus
  pillarboxed, mouse probes (outside the fitted entry no hover; on LOAD GAME hover, and Enter opens the list),
  load-screen art and the fitted bar, the pause menu. Borderless 3024x1964 and exclusive 3024x1964 were also run.
- The visual installer's Display group installs and configures it.
- Without `mode_menu` the SCREEN RESOLUTION menu still prints the old 4:3 names (text assets).
- `backgrounds = cover` crops menu and load-screen art about 12 % top and bottom at 16:9.
- Bands are found by geometry; a full-width sprite meant as a centred element stretches like vanilla.

## Files

`dll/src/gp_display.cpp`, `dll/src/gp_display_logic.cpp/.h`, `tests/dll/test_display.cpp`, `display/README.md`,
`display/tools/`, `dll/HOOKS.md` "Display" and "Fullscreen sizes and borderless".
