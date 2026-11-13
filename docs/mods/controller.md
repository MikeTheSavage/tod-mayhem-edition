# Controller support (`[pad]`, data option `controller = menu`)

## What it does

Xbox-style pads (XInput) and DualShock 4 / DualSense pads play the game through its own console controls: analog
movement, right-stick camera, every action, vehicles, loco moves, menus and pause. Keyboard and mouse keep working;
whichever was used last is in charge. On-screen prompts switch to Xbox or PlayStation button pictures, and the pad
rumbles.

## How it is implemented

1. **Engine facts.** The PC build still has the whole console input layer:
   - ControlDict has `ControlSetup` nodes `PS2`, `XBox` and `PC`, each a table of actions with bindings of 5 types
     (key, none, mouse button, pad button, pad axis; evaluation 0x924B40). The cache picks `PC` (control_type = 2,
     hard-coded at 0x71C0E6), so no pad binding exists on PC.
   - A DirectInput `Gamepad` object (built at 0x93E086) reads a DIJOYSTATE into 32 button dwords and axes
     (ProcessController 0x439E00). Rumble: SetControllerVibration 0x439B10.
   - The play state 0x740560 runs the gamepad procedure 0x73DC70 (analog movement, cubic right-stick turn) only when
     `is_pad_present` (0x4853D0) says yes, and that stub answers 0 on PC.
   - The game already sends rumble requests and obeys its own VIBRATION setting (0x787200 -> 0x924F40 -> 0x439B10).
   - PC texts name keys through `[%N]` parameters filled from the Keyboard text slot, plus literal `[Enter]`, `[Esc]`,
     `[A]`, `[Space]`. Every TextBox draw goes through the text expander 0x8FD410.
2. **A fake pad device.** Gamepad 0 gets a fake IDirectInputDevice8A. Its GetDeviceState hands over the merged pad
   (the XInput slot used last, or a DualShock 4 / DualSense read over HID) as a DIJOYSTATE. The game makes its own
   edges, dead zone and curve; the DLL lifts the stick past the game's 30 % dead zone and pre-warps the look curve.
3. **Console bindings on PC.** PD_CTLSTART 0x925F17 (OPER: the ControlSetup start handler pointer) runs the original,
   then walks the `XBox` setup's folders (cheats excluded) with the game's own Start, so every action also gets its pad
   binding (68 -> 78 controls, 84 -> 144 bindings in game). `[pad_bind]` and `map_*` rewrite those nodes.
4. **Gamepad procedure on demand.** PD_SEL_PROC 0x740A93, PD_SEL_LOOK 0x742360 and PD_SEL_CMD 0x73F7A7 retarget the
   `is_pad_present` calls: 1 while the pad is the device used last. Any key, mouse button or mouse move switches back.
5. **Prompts.** PD_TXTPARAM 0x8FFE6F remembers which TextBox parameter is a key name. PD_EXPAND1..3 (0x8FDFB9,
   0x8FE22D, 0x8FEB8E: box size, string width, render) rewrite key names into button glyphs after the expander. The
   glyphs are 18 code points of the game's own font repointed at cells the DLL paints with GDI+ into rows 288..415 of
   the font atlas (all mip levels). Keyboard style restores the original glyph records.
   A key often drives several actions (W: run and Accelerate; S: run_backwards and Decelerate; F: use and
   use_vehicle), and on the pad those sit on different buttons. So the picture comes from the ACTION the text means:
   per-text hints (`gp_pad_prompts.cpp`, keyed by the box's TextSlot and ordinal, read via PD_TB_TEXTID 0x8FE981 /
   PD_TB_SLOTIDX 0x8FE9AA) taken from the game's own tutorial data (`Tutorial_pc_index` SignIndex = PC Specific_ID)
   and the console texts. The car tutorial shows CROSS / SQUARE / TRIANGLE, the exit stunt L2 / R2, and "You are
   dead! Press [Esc]" shows OPTIONS (menu_show, what that screen waits for). Literals follow their menu actions
   (`[Enter]` menu_select, `[Esc]` menu_back unless the text says menu_show, `[A]` menu_delete, `[Space]`
   tutorial_skip). Texts without a hint keep the key's first action. The HUD use prompt ("Press [F] to use") is one
   box for cars and everything else: PD_USE_VEH 0x60D81B / PD_USE_ON 0x60D646 note the HUD script's data when it
   shows the prompt, and the prompt shows TRIANGLE (use_vehicle) while its vehicle flag (+38h) is set, else CROSS.
6. **Rumble.** PD_VIBRATE 0x924F76 logs the game's own requests and passes them on. PD_SHOT 0x834760 (JMP hook, every
   round spent) counts Ram's own shots. The DLL adds dive landings (MoveState leaving 1B..20h / 24h) and camera shakes.
   Motors go out through XInputSetState, DS4 report 05 or DualSense report 02 (USB), times `rumble_strength`, and stay
   0 when VIBRATION is off.
7. **Data option `controller = menu`.** Shows SETTINGS > VIBRATION on PC (existing record, NotPcOption -> false) and
   moves the "To confirm Screen Resolution" hint (HUD.fragment node 0x00100285) from y 450 to 466 so it clears the
   longer list. Both are set operations.
8. **Per frame.** POST_INPUT reads the pads; the prompt style follows the device used last.

No BUTTON PROMPTS menu row: it would be a new record in HUD.fragment (save break). The style is the ini key `prompts`.

## Ini keys (`[pad]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature |
| `xinput` / `playstation_hid` | 1 / 1 | which pads are read |
| `prompts` / `glyphs` | auto / auto | prompt style (auto, keyboard, xbox, playstation) and picture kind |
| `deadzone_left` / `deadzone_right` / `trigger_threshold` | 0.20 / 0.15 / 0.25 | stick dead zones, trigger press point |
| `look_curve` / `look_sensitivity_x/y` / `invert_x/y` | 2.0 / 1.0 / 0 | right-stick feel |
| `compensate_game_deadzone` | 1 | lift the stick past the game's 30 % |
| `rumble` / `rumble_strength` / `rumble_events` | 1 / 1.0 / 1 | rumble, its strength (0..2), the extra events |
| `rumble_shot` / `rumble_landing` / `rumble_shake` | 0.25 / 0.55 / 1.0 | extra event strengths |
| `tell_scripts` | 0 | 1 tells the game's scripts "controller input" (console help with PS2 sprites the DLL does not redraw) |
| `map_<game button>` | console layout | which pad button feeds a console button |
| `[pad_bind] <action> = <button>` | none | per-action pad buttons |

## Testing and limits

- Unit tests `test_pad`, `test_pad_prompts` and `test_glyphs` (the glyph test also renders the button pictures to PNG
  for review); host test 14.
- A prompt audit, `dll/tools/pad_prompt_audit.py`, lists every PC prompt that names a key with its picture before and
  after and the button that actually works.
- Checked in game on a test copy of the game with the `[debug]` scripted pad, which feeds the same fake device: control merge, pad menus, movement, right-stick turn, RT shoots, LT shoot-dodges, VIBRATION on / off,
  and keyboard / Xbox / PlayStation prompts in menus, the save prompt and the pause list, plus rumble requests logged
  in a fight.
- **No physical pad was used in testing.** XInput reading and rumble, the DS4 / DualSense HID parsers and USB rumble
  reports are unit-tested against byte layouts only. PlayStation rumble over Bluetooth is not sent (USB only).
- Stick feel is tuned on paper. Camera-shake rumble never fired in the test fights; car-crash rumble is not built.
- A text already on screen keeps its old pictures until it is drawn anew (menus redraw on every page).

## Files

`dll/src/gp_pad.cpp`, `dll/src/gp_pad_logic.cpp/.h`, `dll/src/gp_pad_prompts.cpp/.h`, `dll/src/gp_glyphs.cpp/.h`,
`dll/src/gp_pad_kb.inc`, `tests/dll/test_pad.cpp`, `tests/dll/test_pad_prompts.cpp`, `tests/dll/test_glyphs.cpp`,
`dll/tools/pad_prompt_audit.py`, `dll/HOOKS.md` "Pad".
