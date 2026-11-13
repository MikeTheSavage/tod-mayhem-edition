# Testing TODGameplay.dll in game

**Setup.** Use a copy of the GOG game under `<repo>\work\<copy>`. `dll\install_copy.ps1 -Target <copy>` puts
`dsound.dll`, `TODGameplay.dll` and the sample `TODGameplay.ini` next to `TOD.exe`. A save at the mission "The Ambush"
is a good place for the combat tests. Windowed is fine. After each run read `TODGameplay.log` next to the exe.

Mark each line PASS / FAIL and note what was seen.

## 0. Loading
| # | Do | Pass if |
|---|---|---|
| 0.1 | Start with the sample ini | The game starts and plays sound. Log: `TODGameplay build 1 loading ... md5 d9beb351...`, one `site ...` line per enabled feature, `N site(s) patched`, no `differ` lines |
| 0.2 | Same with the renderer bridge's dinput8.dll and TOD HD's d3d9.dll present | Both of their logs show their usual hooks; ours too; game runs |
| 0.3 | Delete TODGameplay.ini | Log says `nothing patched`; the game plays exactly as vanilla |
| 0.4 | `[loader] load = 0` | Only forwarding: no TODGameplay lines after the loader line; sound works |

## 1. Manual bullet time (`[bullettime]`)
| # | Do | Pass if |
|---|---|---|
| 1.1 | In combat press the middle mouse button | Within ~0.25 s the world slows to ~0.3 with blur and slow-motion tracers; log `BT on: trigger_slowmotion(0.300, 0.25 s ...)` then `BT latch: WorldTime -> 0.300` |
| 1.2 | Press again | Back to normal in ~0.25 s; log `BT off` |
| 1.3 | Keep it on | It ends by itself after ~8 s (meter empty); a new press is refused until the meter refills above 10 % |
| 1.4 | Kill enemies with it off | The meter refills (+12 % per kill; check `meter` in the shared memory or that bullet time lasts longer); it also refills slowly 2 s after use |
| 1.5 | Dive (SPACE + direction) during bullet time near enemies | The dive stays at the bullet-time speed (no deeper slow motion); landing keeps bullet time (no blip to normal speed) |
| 1.6 | Set `dive_slowmo_in_bullettime = 1`, repeat 1.5 | The dive goes deeper (about 0.03), landing returns to 0.3 |
| 1.7 | Die during bullet time | The game's death slow motion and rewind prompt behave as vanilla; log `BT ended by the game` or an abort line; after the rewind the speed is normal |
| 1.8 | Enter a car during bullet time | Normal speed in the car; bullet time cannot start in the car (expected: on foot only) |
| 1.9 | Use a Tornado or open the power-up menu (P) during / before bullet time | Refused while exclusive; after the Tornado / menu time is the bullet-time speed or normal, never stuck |
| 1.10 | Trigger an in-engine camera sequence (e.g. MM013 start fly-over) with bullet time on | The sequence plays at normal speed (log `abort (cut-scene)`), afterwards normal speed |
| 1.11 | Pause (Esc) during bullet time, wait 10 s, resume | Meter did not drain while paused; bullet time continues |
| 1.12 | Mouse look in bullet time | Turning still responds to the mouse at full speed (it is applied at the slowed physics steps: note how smooth it looks) |
| 1.13 | `mode = hold` | Active only while the button is held |
| 1.14 | Pad: right stick click (XInput pad) | Same as 1.1 |
| 1.15 | Shared memory: run any reader of `Local\TODGameplay.Shared.v1` | magic TGPS, version 1, meter changes while bullet time runs, flags IN_CONTROL / BT_RUNNING |

## 2. Dive slow motion off (`[slowmo] dive_auto_slowmo = 0`, bullet time off)
| # | Do | Pass if |
|---|---|---|
| 2.1 | Dive next to enemies | No slow motion at all; stamina unchanged by the dive |
| 2.2 | `wire_auto_slowmo = 0`, ride a slide wire with enemies in view | No slow motion on the wire |

## 3. Timing (`[timing]`)
| # | Do | Pass if |
|---|---|---|
| 3.1 | `clock_fix = 1`, TOD HD `fps_cap = 0` (uncapped, > 400 fps), dive repeatedly next to enemies for 20 s | Enemies keep deciding, moving to cover and starting bursts during the slow motion (vanilla: they freeze) |
| 3.2 | Same at a 60 fps cap | No visible difference to vanilla at normal speed; timers (mission countdowns) unchanged |
| 3.3 | Rewind after a death with clock_fix | Rewind plays as vanilla |
| 3.4 | `pickup_step_fix = 1`, kill a cop who drops his gun at 60 and at 144/240 fps | The drop/bounce takes the same time at both rates |
| 3.5 | `fps_limit = 60` with TOD HD's `fps_cap = 0` | 60 fps steady (log line `limiter: high-resolution waitable timer`) |

## 4. Controls (`[controls]`, `[keys]`)
| # | Do | Pass if |
|---|---|---|
| 4.1 | `mouse_turn_fix = 1`: sweep the mouse the same distance at 30, 60 and 240 fps caps | Same turn angle at all three (vanilla: 2x at 30, 0.25x at 240), no jerky turning |
| 4.2 | Gamepad turning | Unchanged |
| 4.3 | `invert_y = 1` | Mouse up aims down |
| 4.4 | `mouse_sensitivity_x = 2` | Turning twice as fast |
| 4.5 | `diagonal_normalize = 1`, hold W+D | Same speed as W alone or slower (5 m/s), not faster |
| 4.6 | `[keys] jump = SPACE`, `shootdodge = LSHIFT` | SPACE jumps, LSHIFT dives, menus still work |
| 4.7 | `mouse_y_fps_normalize = 1` (experimental) at 30 / 60 / 240 fps | Vertical aim speed the same at all rates |

## 5. Camera (`[camera]`)
| # | Do | Pass if |
|---|---|---|
| 5.1 | `fov = 75` | Wider view on foot; El Toro and rewind zooms still work; cut-scene cameras unchanged |
| 5.2 | `cull_fix = 1` without the bridge, 16:9 window | No objects popping at the screen edges |

## 6. Quick restart (`[restart]`)
| # | Do | Pass if |
|---|---|---|
| 6.1 | In a story mission (e.g. MM055 The Ambush) hold F10 for 1 s | The mission ends as failed through the normal fade and result screen; choosing retry restarts the mission. Log `Command_End_Mission(0) sent for mission 11` |
| 6.2 | Hold F10 in free roam / an intermission | Nothing happens; log `has no retry screen` |
| 6.3 | In a sub-mission | Same as 6.1 |
| 6.4 | Tap F10 shortly | Nothing happens |

## 7. Vehicles (`[vehicles]`, experimental)
| # | Do | Pass if |
|---|---|---|
| 7.1 | `steering_return_rate = 0.25`: steer full lock, release | Wheels straighten over ~0.16 s instead of snapping; AI traffic drives normally |
| 7.2 | `s_brake_parity = 1`: from 100 km/h brake with S, then with V | Similar stopping distance (vanilla: S about 1.5x longer) |
