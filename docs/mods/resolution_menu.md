# In-game resolution (`[display] mode_menu`)

## What it does

SETTINGS > SCREEN RESOLUTION lists the real modes of the monitor the game is on: windowed sizes that fit
(`1920X1080 WINDOW`), a borderless window at the monitor's own size (`3024X1964 NATIVE`) and exclusive fullscreen sizes
(`1920X1080 FULL`). Enter applies the choice live, and the game's own countdown keeps it or reverts it. The choice is
kept for the next start, with no need to edit configpc.txt.

## How it is implemented

1. **Engine facts.** The game already has a live apply thread for this row: `command_apply_screenres` 0x6D5800. It looks
   up `table[PCScreenSize]` (0x4AB940), resets the device, and shows "To keep changes press [Enter] ... Changes will
   automatically revert in N seconds". On Esc or timeout it applies `table[OldPcScreenSize]`. Sprites and TextBoxes
   cache their quads in pixels. The HUD has commands to re-layout (MasterControl `command_get_screen_dimensions`, HUD
   `command_reposition_HUD`). A new menu row is impossible (save break), so the existing row carries mode and size.
2. **Row values.** Through the menu helper (MN_GET_INIT 0x7FF75C / MN_GET_RESET 0x7FE7C8, option type 20) the DLL
   replaces the row's value list with codes 0x7E00+i, one per mode, built from the window's monitor
   (MonitorFromWindow): standard 16:9 / 16:10 / 3:2 / 4:3 windowed sizes that fit the work area, the native borderless
   size, the adapter's standard or native fullscreen sizes (adapter 0's monitor only), and the size in use.
3. **Labels.** DS_MM_SETTEXT 0x60FA50 (JMP hook on TextBox::SetTextIndex) turns a code into the mode's label as the
   box's own text: no text asset, works in every language. Labels are short because the value box holds about 16
   characters.
4. **Page open.** DS_MM_OLDGET 0x6CAF7F returns the row index of the mode in use.
5. **Apply and revert.** DS_MM_APPLY 0x6D5CD3 and DS_MM_REVERT 0x6D5E95 replace the apply thread's two table calls with
   the chosen mode (device Reset, window style and size). A Reset the device refuses restores the previous mode at
   once. After a change every Sprite / TextBox is marked dirty and the HUD is laid out again with the game's commands.
6. **Keep.** DS_MM_STORE 0x6D5EE5 (end of the apply thread) writes
   `Documents\Total Overdose\savegames\TODGameplay_display.txt` (mode, width, height, monitor) and gives Slot11 only a
   vanilla index 0..4. With `write_configpc = 1` it also writes configpc.txt (`fullscreen = false`, a windowed size that
   fits the DPI-unaware game) and the registry XRes / YRes, so the game without the DLL still starts sanely.
7. **Next start.** DS_MM_WININIT 0x45EBF9 and DS_BL_INIT 0x45EBE2 create the device directly in the kept mode (no
   first-frame switch). Without the side file the mode comes from configpc.txt and `[display] borderless`.
8. **DPI.** With mode_menu the game is per-monitor DPI aware: `1920X1080 WINDOW` is 1920x1080 real pixels (under 150 %
   scaling it looks smaller and sharper than the old blurry window).

## Ini keys (`[display]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | needed for everything in `[display]` |
| `mode_menu` | 1 | the in-game mode row (0 = the old behaviour of [display_fix.md](display_fix.md)) |
| `write_configpc` | 1 | keep configpc.txt and the registry sane for the game without the DLL |
| `label_chars` | 0 | labels longer than this are drawn condensed (0 = what the box holds) |
| `borderless` | 0 | only the first start's default once a mode is kept in game |

## Testing and limits

- Unit tests (display, 248 checks), host test 13b (the SetTextIndex stub), `check_sites` all OK.
- Checked in game on a test copy of the game:
  - in one session from the pause menu: windowed 1920x1080 -> 3024X1964 NATIVE -> 1280X720 WINDOW -> 1920X1080 FULL,
    each through the countdown, with the HUD at the new edges each time;
  - a restart in the kept fullscreen and in the kept borderless mode; a fresh profile with no 800x600 shrink;
  - Esc in the countdown reverts; the game without the DLL starts windowed at configpc's size;
  - in the combined test, a live switch 1920x1080 -> 1920x1200 with the HD effects on.
- Not verified: a physical pad on the row, two monitors, a Reset the device refuses (code review only), exclusive
  fullscreen while the window is not in front. Alt+Enter is not built (the game reads Enter through DirectInput too).
- The renderer bridge hooks setscreenresolution 0x45BF90; mode changes here bypass it, so the bridge does not see them.
- The "To confirm Screen Resolution press [Enter]" hint still overlaps MOUSE SENSITIVITY with keyboard prompts.

## Files

`dll/src/gp_display_modes.cpp/.h`, `dll/src/gp_display_logic.cpp`, `dll/src/gp_menu.cpp`, `display/README.md`,
`dll/HOOKS.md` "Display modes".
