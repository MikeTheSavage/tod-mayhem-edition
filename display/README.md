# 16:9 display (`[display]` in TODGameplay.dll)

Total Overdose at 1920x1080 (or any wide size) as it should look. Off by default in the DLL; the installer's Display
card switches it on (backup first; only the `[display]` section of `TODGameplay.ini` is set).

## What is wrong in vanilla at 1920x1080

| Problem | Cause |
|---|---|
| The game goes back to 4:3 | The saved option SCREEN RESOLUTION (Slot11 `Setting_ScreenSize` 0..4, all 4:3: 640x480 .. 1600x1200) is applied after the first frame and overrides configpc `screensize`. Index 4 = 1600x1200; a fresh profile has 1 = 800x600. |
| HUD in a box in the middle | The HUD canvas is 640x480 with square pixels: at 16:9 a 1440x1080 box, 240 px empty each side. |
| Huge, stretched fonts | Menus, subtitles, load screens and the front end are an 800x600 canvas stretched to the screen: x 2.4, y 1.8 at 1920x1080 = 33 % too wide. |

## What `[display]` does

| Key (default) | Effect |
|---|---|
| `res_table = auto` | The five sizes become 1280x720, 1600x900, 1920x1080, 2560x1440 and the configpc size (index 4). The menu still prints the old names (text assets). |
| `startup_size = config` | The saved option is no longer applied after the first frame: configpc `screensize` stays. Saves are not touched. |
| `wide_hud = 1`, `hud_scale = 1.0`, `border = vanilla` | HUD canvas 480 high and 480 x W/H wide (853 at 16:9): every anchored widget goes to the real edges, the aim dot stays in the middle. |
| `menus = 1` | The 800x600 canvas is fitted with square pixels (s = min(W/800, H/600) = 1.8, x offset 240 at 1920x1080): sprites (both render paths), TextBox text (font scale + glyph offset in both DrawString versions), mouse hit tests, the load bar. |
| `bands = stretch` | Full-width bars (menu frame, header, subtitle band, cut-scene shutters) still span the screen. |
| `backgrounds = cover` | Full-screen sprites (fades, menu backdrop, load-screen art) fill the screen undistorted (2.4 uniform, 180 px cropped top and bottom); `stretch` = vanilla, `fit` = pillarboxed. |
| `cull_fix = 1` | Culling frustum with the real aspect (1.7778): no objects popping at the 16:9 edges. Same as `[camera] cull_fix`. FOV untouched (the game's Hor+ gives 79.6 deg horizontal at 16:9). |

Sites, frames and REF rows: [`../dll/HOOKS.md`](../dll/HOOKS.md) section "Display"; code `../dll/src/gp_display.cpp`,
`gp_display_logic.cpp` (unit tests `../tests/dll/test_display.cpp`, host test 13).

## Testing

Checked on a test copy of the game, windowed at 1920x1080 with an isolated profile: title, main menu, load list, load
screen (CENTRO art + fitted load bar), gameplay HUD and pause menu, each compared before and after (before: stretched
menus and a 4:3 HUD box). The mouse hit tests were checked too: with the fitted layout a cursor at x 300 px is outside
the LOAD GAME entry (no highlight, while the stretched vanilla layout highlights it there), and at x 560 px the entry
is highlighted and ENTER opens the load list.

These checks ran in an in-game test harness on a copy of the game (not included): scripted key and mouse input,
screenshots, and afterwards the test copy, the real saves, the GOG files and the registry key
`HKCU\Software\Eidos\Total Overdose` restored and compared.

## Fullscreen and borderless

Vanilla exclusive fullscreen only knows a fixed 4:3 mode list: any other `screensize` starts at 640x480, and the saved
option cannot switch to a wide size either (seen with 21:11 and 21:16 sizes). `[display]` has:

| Key (default) | Effect |
|---|---|
| `fullscreen_any_mode = 1` | Exclusive fullscreen at any size the adapter offers (60 Hz if offered, else the highest <= 85 Hz, else the lowest: on the test monitor 1920x1200 exists only at 119/120 Hz). |
| `borderless = 0` | 1: `fullscreen = true` runs as a frameless window at the configpc size, centred on its monitor; at the monitor's size it covers it exactly. No display mode change. |
| `dpi_aware` (on with borderless) | Physical pixels: under 150 % scaling 3024x1964 is the whole screen, nothing blurred. |

With `mode_menu` (below) there is no need to edit configpc.txt, `[display] borderless` or the registry by hand: those
only set the first start's default.

## Choosing the mode in the game (`mode_menu = 1`, the default with `enabled = 1`)

SETTINGS > SCREEN RESOLUTION lists the real modes of the monitor the window is on, built at run time (any PC):

| Values (in this order) | Which |
|---|---|
| `1280X720 WINDOW` ... | standard 16:9 / 16:10 / 3:2 / 4:3 sizes (21:9 on ultrawide monitors) whose framed window fits the work area |
| `3024X1964 NATIVE` | a frameless window covering the monitor at its own (native) size: no display mode change |
| `1920X1080 FULL` ... | exclusive fullscreen: the adapter's sizes that are standard or native, not above the monitor; only on the monitor of the device's adapter |
| (the size in use) | always listed, even when it is no standard size |

Left / Right (keys, d-pad, mouse on the arrows) choose, Enter / A applies it **live** (device Reset, window style and
size, no restart), then the game's own question "Screen Resolution has changed. To keep changes press [Enter]. To
Revert changes press [Esc]. Changes will automatically revert in N seconds" keeps it or goes back. A mode the device
refuses goes back to the previous one at once. After a change the menus are rebuilt at the new size and the in-game HUD
is laid out again (the game's own command_get_screen_dimensions / command_reposition_HUD).

The kept mode is the mod's: `Documents\Total Overdose\savegames\TODGameplay_display.txt` (mode, width, height, the
monitor it was chosen on), applied when the device is created at the next start (no first-frame switch, no 4:3 trap).
For the game without the DLL it also writes configpc.txt (`fullscreen = false`, `screensize` = a windowed size that
fits the DPI-unaware game, e.g. 1920,1080) and the registry XRes / YRes the same; the saved option (Slot11) gets only
the vanilla indexes 0..4. Without the side file (first start) the mode comes from configpc.txt (`fullscreen`,
`screensize`) and `[display] borderless`.

| Key (default) | Effect |
|---|---|
| `mode_menu = 1` | the in-game mode row (0 = the old behaviour) |
| `write_configpc = 1` | keep configpc.txt / the registry sane for the game without the DLL |
| `label_chars = 0` | labels wider than the value box are drawn condensed (0 = measured from the box) |
| `dpi_aware` (on with mode_menu) | physical pixels: "1920X1080 WINDOW" is 1920x1080 real pixels (under 150 % scaling it is smaller on screen than the old DPI-unaware window, and sharp; 2880x1620 is not a value, `2560X1440 WINDOW` is the nearest) |

Sites and how the game's own apply thread is used: [`../dll/HOOKS.md`](../dll/HOOKS.md) "Display modes". The mode
menu was checked in game on a test copy.

## Known limits

* Without `mode_menu` the SCREEN RESOLUTION menu still shows the 4:3 names (the strings are text assets).
* The value box holds about 17 characters: longer labels are drawn condensed. The borderless value is called NATIVE
  (it always is the monitor's size), exclusive ones FULL; no value carries the word BORDERLESS.
* Load-screen / menu art is cropped 12.5 % top and bottom with `cover`; `backgrounds = stretch` gives the vanilla look.
* Bands are mapped by geometry (a sprite spanning the full canvas width); a full-width sprite meant as a centred
  element would be stretched like vanilla.
* Log lines that show it is active: `display: menus on`, `startup_size = config`, `cull_fix: culling aspect 1.7778`.
