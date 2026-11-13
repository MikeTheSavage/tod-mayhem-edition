# Core gameplay (bullet time, timing fixes, mouse, FOV, restart, driving tweaks)

## What it does

Bullet time on a key (middle mouse by default) with a meter that refills from kills and combos. Dives can stop
triggering their automatic slow motion. Timers, dropped pickups and mouse turning behave the same at any frame rate.
Optional: wider FOV, key rebinding, a hold-F10 quick restart, gradual steering return and S braking like V.

## How it is implemented

1. **Engine facts.** Slow motion belongs to TIME_CTRL (cache data slot +0x280): `command_trigger_slowmotion` (message
   -8, handler 0x784D70) and `command_auto_slow_motion` (-12, sent by the dives). The world speed is WorldTime (cache
   data +0x850); the game snaps it back after its own slow motion. The integer-ms clocks in Scene::Update 0x897450
   truncate each frame's milliseconds, which stalls timers in slow motion at high frame rates. The mouse turn is
   written to PhysicCtrl.turn_speed every frame but only consumed at physics steps, so most of it is lost.
2. **Bullet time.** In the input tick the DLL sends `command_trigger_slowmotion {scale, ramp_in, no_blur}` and then
   latches WorldTime to the scale after the game's notify (0x86B4B0), so the game's own "back to normal" returns to
   bullet time. A meter drains in real time and refills per kill (MissionCtrl data +0x28), per combo point (Combo_Ctrl
   data +0x3C) and slowly when idle. It refuses to start in exclusive modes (Tornado, power-up menu) and cut-scenes,
   and ends cleanly on death, vehicles, cut-scenes and mission starts (`command_force_normal_time`, -6 / -5).
3. **Shared memory.** The meter is published in `Local\TODGameplay.Shared.v1` (`dll/src/todgameplay_shared.h`) for
   other programs such as the renderer bridge.
4. **Dive slow motion off.** ARM_DIVE1..3 (0x53A5BE, 0x53C526, 0x53CB4F) and ARM_WIRE (0x5F4429) retarget the
   `SendMessage(TIME_CTRL, -12)` calls to a stub that returns 0 while suppressed.
5. **Clock fix.** CLK_GAME 0x897E3C, CLK_REWIND 0x897C31, CLK_REAL 0x8974F8 retarget the `_ftol2` calls to stubs that
   carry the fraction. At time scale 1 the result equals vanilla.
6. **Pickup step fix.** PICKUP 0x79D0A3 (prop_move_ctrl 0x79C700) keeps the step remainder instead of `t = 0`.
7. **Mouse.** YAW_WRITE 0x73FFD5 accumulates every frame's turn; YAW_READ 0x57D504 hands the accumulated angle to the
   next physics step; YAW_PAD 0x73E3F9 leaves mouse mode for the pad. MOUSE_DX 0x73ED99 / MOUSE_DY 0x73EDF0 scale and
   invert MouseDeltaX / Y. DIAG 0x55FCEF clamps the keyboard stick copy to length 1 (W+D no longer 41 % faster).
8. **Key rebinding.** After each poll (POST_INPUT 0x43C516) the DLL rewrites the game's keyboard arrays (object
   [0xA35E80]: +0 edges, +0x100 state) and mouse buttons ([0xA35EAC]), so menus see the new keys too.
9. **FOV.** CAM_ENTRY 0x87DE40 computes an effective fov; CAM_VIEW 0x87DE5F and CAM_CULL 0x87DFE5 read it instead of
   Camera +0xB4. The stored 64 is never written, so game logic still sees 64. Zooms keep their tangent ratio.
   CAM_ASPECT 0x87E054 (`cull_fix`) uses the real aspect for culling; it is shared with the renderer bridge, so the
   first one to patch wins.
10. **Frame limiter.** LOOP_TOP 0x43C4E8: a waitable timer plus a short spin before the input poll. Off by default:
    use one limiter only (TOD HD's `fps_cap` or the bridge).
11. **Quick restart.** Holding the key sends MissionCtrl (+0x25C) `Command_End_Mission(0)` (-10, only in the running
    state 0x788520): the game's own failed-mission screen with its retry. Refused in intermissions, free roam, the
    prologue (MM010 / MM013) and the tutorial.
12. **Driving tweaks (experimental, every car including AI).** STEER_RET 0x4F60C5 moves wheel_angle (+0x378) toward 0
    by `steering_return_rate` per 0.04 s step instead of snapping. SBRAKE_A 0x4F6D85 and SBRAKE_B 0x4F6F43 turn two
    `ja` into `jmp`, so S takes the pedal path (mass factor 1.0, like V). The constant at 0x9B74F4 is shared (also the
    headshot factor), so it is never written.

## Ini keys

Default = the sample `dll/TODGameplay.ini`. A missing key is off.

| Key | Default | Meaning |
|---|---|---|
| `[bullettime] enabled` | 1 | manual bullet time |
| `[bullettime] key` / `pad_button` / `mode` | MOUSE3 / PAD_RS / toggle | trigger and toggle or hold |
| `[bullettime] scale` / `ramp_in` / `ramp_out` | 0.30 / 0.25 / 0.25 | world speed and ramps (s) |
| `[bullettime] meter_seconds` / `refill_per_kill` / `regen_per_second` | 8 / 0.12 / 0.02 | meter size and refills |
| `[bullettime] dive_slowmo_in_bullettime` | 0 | 0: a dive keeps the bullet-time speed |
| `[slowmo] dive_auto_slowmo` / `wire_auto_slowmo` | 1 / 1 | 0 = no automatic slow motion on dives / the wire |
| `[timing] clock_fix` / `pickup_step_fix` | 1 / 1 | frame-rate fixes |
| `[timing] fps_limit` | 0 | frame cap (0 = off) |
| `[controls] mouse_turn_fix` / `diagonal_normalize` | 1 / 1 | mouse turn and diagonal fixes |
| `[controls] mouse_sensitivity_x/y` / `invert_y` | 1.0 / 0 | mouse scale and invert |
| `[controls] mouse_y_fps_normalize` | 0 | experimental vertical fps normalisation |
| `[keys] <action> = <key>` | none | rebinding (DirectInput key names, MOUSE1..5) |
| `[camera] fov` / `fov_all_cameras` / `cull_fix` | 0 / 0 / 0 | 0 = vanilla 64; 73.7 gives 90 deg horizontal at 16:9 |
| `[vehicles] steering_return_rate` / `s_brake_parity` | 0 / 0 | experimental driving tweaks |
| `[restart] enabled` / `key` / `hold_seconds` | 1 / F10 / 1.0 | quick restart |
| `[loader] load` / `chain_dsound` | 1 / unset | the dsound proxy (0 = forward only) |

## Testing and limits

- Verified statically: site bytes (`check_sites.py`), the host test (the clock and dive stubs are executed there),
  a disassembly review of the stubs and the unit tests (`tests/dll/test_logic.cpp`).
- These features have no recorded in-game test run; quick restart in particular is unproven in game. The in-game
  checklist is `dll/TEST_IN_GAME.md`.
- There is no meter bar on screen yet, and turn-to-face movement is not built.
- Camera distance is a data option, not code ([data_zip.md](data_zip.md)); on old saves it does not apply (saves store
  the camera position).

## Files

`dll/src/gp_game.cpp` (glue, stubs, ticks), `dll/src/gp_logic.cpp/.h` (meter, bullet-time state machine, mouse maths,
limiter, keys, ini, md5), `dll/src/sites.h`, `dll/HOOKS.md` "Patch sites", `dll/README.md`, `dll/TEST_IN_GAME.md`.
