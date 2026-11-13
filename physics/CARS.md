# [car_physics]: the game's cars in the Box3D world

Builds on `[physics]` (Box3D props, [README.md](README.md)). The cars of the original GOG `TOD.exe` push and scatter
the prop bodies, blasts throw and spin cars, and destroyed cars become Box3D wrecks that tumble, flip and settle. It is
dramatic by default (`[physics] shell_impulse = 450`, `blast_lift = 4` unchanged).

**Off by default** (`[car_physics] enabled = 0`). It also needs `[physics] enabled = 1` and a `TODPhysics.dll` from
the same build (it exports `TODPhysics_GetCarApi`; an older DLL leaves the feature off, logged). It has no patch site
of its own.

## What it looks like in game

Checked on the Downtown City Hall plaza:

* A real game shell beside a pickup: the game destroys it, `[car_physics]` throws it and hands the wreck to Box3D. It
  flips through the air and lands on its roof 4.3 m away, 3.1 s later.
* A shell-sized blast among the props and beside a car: the taco stand (a 12-hull, 80 kg body) flies and lands on its
  end, crates fly 4-20 m.
* A rail traffic pickup at 15.6 m/s ploughs through the taco stand, two crates and a bin laid on its road: kicks of
  5.5-10 m/s, a crate rides off on its bed, the stand lands upside down on the plaza.
* A blast beside a parked pickup (game-state route): up 2.2 m, onto its roof (up y -0.96) in 1.1 s, then onto its side.

## Performance

With 2-4 cars near Ram and the debug log on:

| What | Cost |
|---|---|
| Car frame | 0.015-0.13 ms average, 1-2 ms max on a normal frame |
| Scene walk for cars | 0.5-1.1 ms every `car_rescan_seconds` (3 s) |
| First sight of a new car model | 9-17 ms once (its pivot walk and, with `debug_log`, the pivot log lines; boxes are cached per model and one new car is measured per frame) |
| Box3D step with the kinematic car boxes | 0.01-0.05 ms average per 0.04 s step |

## How it works

| Part | Where | What |
|---|---|---|
| glue | `code/car_game.cpp` (in TODGameplay.dll) | car list, poses, kinematic sweep, prop kicks, blasts on cars, wrecks, bullets, test-copy rig |
| Box3D side | `code/phys_car_sim.cpp` (in TODPhysics.dll, interface `code/car_api.h`) | kinematic car boxes, wreck boxes, begin-touch reports, wreck ray; same world and body list as the props (`code/phys_sim_internal.h`) |
| pure logic | `code/car_logic.cpp/.h` | config, car box from collision points, sweep targets, blast / push / self kicks, velocities from poses |
| hooks into [physics] | `code/car_glue.h`, calls in `code/phys_game.cpp` | scan (cars collected, vehicle models left out of the static level), per-step / per-frame calls, blasts, bullets, keys |
| tests | `code/test_car.cpp` (run by `dll\build.bat`) | logic, plus a Box3D crate shoved by a kinematic car box, a wreck that lands and sleeps, the wreck ray |
| clips | `tools/make_car_clip.py` | MP4 clip and contact sheet from a test run's screenshot bursts |

1. **Cars.** Every `car_move_ctrl` node (script class main handler 0x4DB730) is collected while `[physics]` walks the
   scene, and again every `car_rescan_seconds` (a walk of ~17,500 nodes costs 0.5-0.9 ms). Its parent is the
   `vehicle_root` Model (handler 0x824A70): the node the game moves and draws. vehicle_root data +4Ch is the
   `vehicle_ai` (handler 0x803A50): ControlState +18h (0 not controlled, 1 the player's, 2 rail traffic, 4 after a
   traffic accident), Destroyed +C8h. The car_move_ctrl node itself is the `RigidBody`: coll mass +50h, Position_t
   +ECh, Orientation_t +FCh (stored quaternion), momentum +10Ch, angular velocity +1FCh (world space), read and written
   with the engine's own accessors.
2. **Pushing props** (`push_props`). Each live car within `car_radius` of Ram is a kinematic Box3D box: the bounds of
   its model's collision (every pivot but `Replace*`) grown by `margin`. Each 0.04 s step the box is swept from its
   last pose to the car's current one (interpolated over the steps of the frame, `lead` seconds ahead), so Box3D gives
   it the car's velocity and it shoves every prop it meets; the car itself is never slowed. The first touch of a prop
   adds a kick (`push_gain` of the car's speed forward, 0.2 of it out of the contact side, `push_lift` up,
   `push_spin`). Vehicle models (and their parts) are left out of `[physics]`' static level mesh. With `extra_props`
   the street stands and carts (`stand,cart,stall,kiosk`) become prop bodies too.
3. **Blasts on cars** (`blast_cars`). `[physics]`' blast hooks (weapon_shell: grenades, launcher, rockets, dynamite;
   Destroyable_Object: red barrels, gas pumps) and a car's own explosion call `car_blast`. A live car gets exactly what
   the game's `vehicle_ai take_a_hit` (0x80D750) does for a hit with an impulse:
   `command_activate_without_first_frame` (377) to its car_move_ctrl (a parked car starts simulating),
   `command_traffic_accident` (382) to its vehicle_ai (a rail traffic car becomes a physics car), tumbling = 1, the
   spinning frictions, in_control = 0, then linear velocity (0x500C20) and angular velocity (0x932560) changed by
   `blast_kick`: away from the blast at `blast_gain * impulse per area * blast_area / mass` times the falloff, up by
   `blast_lift * blast_lift_gain` plus a third of that, and a roll / pitch of `blast_spin` that lifts the near side.
   The game's own out-of-control car physics (0x4F6FC0) flies, flips and lands the car on the level's real collision.
   Every member write goes through the rewind record (0x86B4B0) first.
4. **Car explosions.** When a car's vehicle_ai Destroyed goes to 1, its position is a blast for the props and the
   other cars (`car_blast_radius`, `car_blast_impulse`; the exploding car is skipped). Damage stays the game's: chain
   explosions are handled by `[mayhem]` (see [mayhem/README.md](../mayhem/README.md)).
5. **Wrecks** (`wreck_dynamic`). The destroyed car (never the player's while he is in it) becomes a dynamic Box3D box
   (`wreck_mass`) with the game's velocity and a random flip (`self_spin`). Each frame its interpolated pose is written
   to the vehicle_root (SetPos / SetOrient, as for the props) and to the RigidBody (Position_t / Orientation_t,
   velocities zeroed): the game keeps running its car code on wrecks, so it starts every frame from where Box3D put it.
   A wreck that flies out of the scanned level (Box3D only has the level near Ram) is handed back to the game with its
   speed and spin. Blasts throw wrecks through Box3D; bullets push them at the hit point (`bullets`,
   `wreck_bullet_gain`). A rescan (never while a wreck moves) re-creates resting wrecks.

## Engine behaviour worth knowing

* Near the test spot every car is rail traffic: the traffic system re-places the vehicle_root every frame, the
  RigidBody's Position_t is stale (tens of metres off), and a SetPos is undone at once. `command_traffic_accident`
  (382) makes it a physics car (ControlState 4, the driver gets out).
* A car moves under the game's out-of-control physics only when it is thrown with an upward part. Every blast flew,
  flipped and landed cars on the game's own collision; a purely horizontal launch of a crashed traffic car standing on
  its wheels was zeroed the next frame, and its accelerator did nothing (the crashed car's script no longer drives). A
  driven car integrates its pose from the axle points (front_point_t +2A8h / rear_point_t +2B8h), not from the root,
  so the test rig moves those too.
* The game already throws an exploding car about 8 m/s up (vehicle_ai destruction), so `self_kick` is 0 by default and
  only a random flip (`self_spin`) is added.
* Destroyed cars keep running their car code (gravity every frame). Writing the root and the RigidBody pose with zero
  velocity every frame keeps them exactly on the Box3D wreck (0.000 m off in every logged frame).
* Rail traffic brakes and swerves for props on its road (its TrafficCollisionProbe): one car slowed from 13.5 to
  4 m/s before it reached them, another came through at 15.6 m/s.
* A multi-hull prop (the taco stand: 12 hulls) reports one begin-touch per hull, so kicks are limited to one per prop
  per 0.5 s.
* Havok props are still broken by the game when hit hard (bench, bin, chair, stand "dropped (disabled)" after blasts
  or Ram running into them). The broken version is not simulated.
* F beside the middle of a car grabs its roof (Ram climbs onto it); at the front door it gets him in.

## Settings (`[car_physics]` in TODGameplay.ini)

`enabled`, `push_props`, `blast_cars`, `wreck_dynamic`, `bullets`, `impulse_scale` (one knob for every push, kick and
spin), `extra_props`.

Tuning keys (code defaults, not in the sample ini): `margin`, `lead`, `push_gain`, `push_lift`, `push_full_speed`,
`push_min_speed`, `push_spin`, `blast_gain`, `blast_area`, `blast_lift_gain`, `blast_spin`, `blast_reach`,
`max_speed`, `max_spin`, `self_kick`, `self_spin`, `car_blast_radius`, `car_blast_impulse`, `wreck_mass`,
`wreck_friction`, `wreck_restitution`, `wreck_linear_damping`, `wreck_angular_damping`, `wreck_bullet_gain`,
`teleport_distance`, `car_rescan_seconds`, `car_radius`, `max_cars`.

Test copy only: `debug_log`, `debug_shots`, `debug_list_key`, `debug_kick_key` (a shell-sized blast beside the
nearest car), `debug_wreck_key` (a real game shell beside it), `debug_place_key` / `debug_drive_key` (the rig below),
`debug_drive_props`, `debug_drive_ahead`, `debug_drive_side`, `debug_drive_delay`, `debug_drive_seconds`,
`debug_place_hold`, `debug_forward_negz`.

## Testing

Every car near the test spot is rail traffic (the traffic admin re-places it every frame), so the test rig makes the
nearest live car a physics car the game's way (`command_traffic_accident`) and holds it at its start pose for
`debug_place_hold` s. `place` parks it broadside `debug_drive_ahead` m in front of Ram; `drive` puts it
`debug_drive_side` m to the left, lines props up on its path, and holds its accelerator (the game drives it).

The tests ran through an in-game test harness on a copy of the game (not included). After every run the test copy, the real saves and the
GOG files were checked byte-identical, and the registry resolution was restored.

## Known issues and untested cases

* The player driving a car through props was tried with the enter rig: Ram got in once (and the rig then moved the car
  with him inside), but the drive through the props was not captured. The kinematic box follows any car's own motion
  the same way (the traffic run is that case), but a player-driven impact has not been filmed.
* Cars are not slowed by what they hit (kinematic). A heavy prop (80 kg max) never stops a car.
* The props and wrecks live in `[physics]`' scanned circle (40 m around Ram); a wreck that leaves it goes back to the
  game's own physics.
* The camera cannot be steered by the test rig, so the impacts were seen from behind Ram.
* Not run in game: bullets on wrecks (code and unit test only), Destroyable_Object blasts (no red barrel at the test
  spot), rewind with wrecks, a save made while a wreck is a Box3D body (the wreck's pose is in the node; the game saves
  the node), many cars at once (more than 4 near Ram), long normal play.
* A parked traffic car that went through the test rig no longer simulated in later tests of the same session. Only
  the test rig does that to a car.
