# Car physics (`[car_physics]`)

## What it does

Moving cars shove and scatter the physics props. Grenade, rocket, dynamite and barrel blasts throw and spin cars, and
an exploding car throws what is around it. Destroyed cars become wrecks that tumble, flip and settle. No damage is added.

## How it is implemented

1. **Engine facts.**
   - Each car has a `car_move_ctrl` node (class handler 0x4DB730). It is the car's RigidBody: mass +50h, Position_t
     +ECh, Orientation_t +FCh, momentum +10Ch, angular velocity +1FCh.
   - Its parent is the `vehicle_root` Model (handler 0x824A70), the node the game moves and draws. vehicle_root data
     +4Ch is the `vehicle_ai` (handler 0x803A50): ControlState +18h (0 none, 1 player, 2 rail traffic, 4 after a
     traffic accident), Destroyed +C8h.
   - `vehicle_ai take_a_hit` (0x80D750) shows how the game throws a car: `command_activate_without_first_frame` (377),
     `command_traffic_accident` (382), tumbling, frictions, in_control 0, then linear (0x500C20) and angular (0x932560)
     velocity. The out-of-control car physics (0x4F6FC0) then flies and lands it on the real level collision.
   - Rail traffic is re-placed by the traffic system every frame; only `command_traffic_accident` makes it a physics car.
2. **No patch site of its own.** It runs inside `[physics]`: the scan collects cars (and leaves vehicle models out of
   the static mesh), and the step loop and blast path call into `car_glue.h`.
3. **Pushing props.** Each live car within `car_radius` of Ram is a kinematic Box3D box (the bounds of its collision,
   every pivot but `Replace*`, plus a margin). Each 0.04 s step the box is swept from its last pose to the car's current
   one, so it carries the car's velocity into every prop it meets. The car is never slowed. The first touch adds a kick
   (forward, out to the side, up, spin), at most one per prop per 0.5 s.
4. **Blasts on cars.** Every blast (`explode_at`) calls `car_blast`: a live car gets the game's own take_a_hit sequence
   above, with velocity away from the blast and a roll that lifts the near side. Every member write goes through the
   rewind notify (0x86B4B0) first. When a car's Destroyed goes 0 -> 1, its position becomes a blast for props and other
   cars. A car explosion is blasted once even with chain reactions on.
5. **Wrecks.** A destroyed car (never the player's while he is inside) becomes a dynamic Box3D box with the game's
   velocity and a random flip. Each frame its pose is written to the vehicle_root (SetPos / SetOrient) and to the
   RigidBody (Position_t / Orientation_t, velocities zeroed), so the game's car code starts from where Box3D put it. A
   wreck that leaves the scanned circle is handed back to the game. Bullets push wrecks.
6. **TODPhysics.dll** exports `TODPhysics_GetCarApi`; an older DLL leaves cars off (logged).

## Ini keys (`[car_physics]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature (needs `[physics] enabled = 1`) |
| `push_props` | 1 | cars shove props |
| `blast_cars` | 1 | blasts throw cars |
| `wreck_dynamic` | 1 | destroyed cars become Box3D wrecks |
| `bullets` | 1 | bullets nudge wrecks |
| `impulse_scale` | 1.0 | one knob for every push, kick and spin |
| `extra_props` | stand,cart,stall,kiosk | street stands and carts added to `[physics] props` |

Tuning keys with code defaults (not in the sample ini) are listed in `physics/CARS.md`. Test-only: `debug_log`,
`debug_shots`, `debug_list_key`, `debug_kick_key`, `debug_wreck_key`, `debug_place_key`, `debug_drive_key`,
`debug_drive_props`, `debug_drive_ahead`, `debug_drive_side`, `debug_drive_delay`, `debug_drive_seconds`,
`debug_place_hold`, `debug_forward_negz`.

## Testing and limits

- Unit tests `physics/code/test_car.cpp` (69 checks, including a Box3D crate shoved by a kinematic car box).
- Checked in game on a test copy of the game at the Downtown plaza: a real shell flips a pickup onto its roof, a blast throws the taco stand, and a rail traffic
  pickup at 15.6 m/s ploughs through the stand and crates.
- Cost: car frame 0.015 to 0.13 ms on average; scene walk for cars 0.5 to 1.1 ms every 3 s.
- Not verified: the player driving through props on video, bullets on wrecks, Destroyable_Object blasts on cars,
  rewind with wrecks, a save made while a wreck is a Box3D body, more than 4 cars near Ram. Cars are never slowed by
  what they hit (kinematic).
- EXPERIMENTAL in the installer (built on the physics prototype).

## Files

`physics/code/car_game.cpp`, `car_logic.cpp/.h`, `car_glue.h`, `phys_car_sim.cpp`, `car_api.h`, `test_car.cpp`,
`physics/CARS.md`, `dll/HOOKS.md` "Car physics".
