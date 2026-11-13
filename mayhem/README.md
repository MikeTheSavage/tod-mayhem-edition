# [mayhem]: chain reactions and breakable cover

Two Mayhem features on top of the `[physics]` Box3D module ([physics/README.md](../physics/README.md)):

* **Chain reactions**: an explosion (grenade, launcher, rocket, dynamite, a red barrel / gas tank / pressure tank, a
  car) sets off the explosive objects and the cars around it, each after its own short delay (0.15 .. 0.6 s of game
  time, nearer ones first), and the Box3D blast of every link throws the props, the debris and the shards.
* **Breakable cover**: crates, tables, stalls, carts, benches, bins ... fall apart under fire (their health is scaled
  down), burst into wooden / metal pieces that Box3D throws, the game's broken model is hidden (the pieces replace it),
  and enemies crouched behind them stand up.

Off by default (`[mayhem] chain_reactions = 0`, `breakable_cover = 0`). Both need `[physics] enabled = 1` and
`TODPhysics.dll`; without them nothing is patched. Never writes anything that goes into a save (see "Saves").

## How it works

| Part | Where | What |
|---|---|---|
| glue | `code/mh_game.cpp` (in TODGameplay.dll) | blasts, chain schedule, cover tracking, debris bodies, survey / test keys |
| drawing | `code/mh_draw.cpp` | the debris pieces as textured boxes with the game's own D3D9 device, in its world pass |
| pure logic | `code/mh_logic.cpp/.h` | config clamps, delays, schedule (dedupe, depth, rate, per-frame cap), fired log, debris pieces, pool |
| tests | `code/test_mh.cpp` (run by `dll\build.bat`), host test 18, `tests/dll/check_sites.py` | |
| clips | `tools/make_clip.py` (side-by-side video of two test runs) | |

### Game paths (GOG TOD.exe; sites and REF rows in `dll/src/sites.h`, details in `dll/HOOKS.md` "Mayhem props")

* **Destroyable_Object** (script class, handler 0x6691A0, data 340 B): `take_a_hit` (global command 215) ->
  method 0x670C70 -> `Destroyable_Objects_AI_Ctrl.take_a_hit` 0x678050: DamageTaken (+BCh) grows; at Health (+114h,
  = Health_m +B8h + the dictionary value, init 0x66F9B0) the object dies: IsDead (+144h) = 1,
  `command_destroy_object` 0x671410 (locks it; when IsExplodeable +7Ch: the explosion call 0x668BA0 = PH_DESTROY,
  camera shake, time-control `do_explosion` freeze), then the dying thread 0x66C1C0 swaps Current_mesh (+138h) to
  Damaged_mesh (+12Ch) and sends `do_explosion` (1328, method 0x66A600) when there is no Replacement.
* **do_explosion 0x66A600**: quadtree query (mask 440h, mode 1) within ExplosiveRange (+118h) and, for every entity
  found, `take_a_hit` {ExplosiveDamage +11Ch, DestructorOwner +20h, the object, the entity, impulse, position, type 1}
  by Node::SendCommand 0x86A340 (one call at 0x66BCDF for destroyables, cars and characters). So vanilla already sets
  off explosive neighbours inside ExplosiveRange, all in the same frame.
* **Cars**: vehicle_ai (handler 0x803A50) state 0x809910 calls its explosion cdecl 0x806860 (thread, 0) with
  [0xA3B75C] = the vehicle_ai node at 0x80A756 and 0x80A8EC (quadtree query within ExplosionRange +BCh); Destroyed =
  data +C8h. A car takes damage through `take_a_hit` on its vehicle_root (handler 0x824A70; AICtrl = data +4Ch).
* **Shells**: weapon_shell `command_explode` -> blast 0x8460B0 (PH_SHELL).
* **AI and cover**: TOD has no cover points. The class registry has no cover member or command; enemies crouch by the
  probabilities of their character type (character_sentry_ai ProCrouch +88h, Crouch_time_from/to +54h/+58h, Crouching
  +120h) at their authored placements / GotoPath points, and line of fire is the game's collision. So "cover" is a prop
  that happens to stand between Ram and an enemy, and the link to invalidate is the prop's collision: when it breaks,
  its wreck model is hidden (node flag 1 = not drawn and no collision, the probe's `(flags | flags >> 16) & 1` test),
  so bullets reach the enemy, and the enemy gets `command_crouch_out` (1054) on its character_animation_ctrl
  (character root data +50h) so it stands up.

### Chain reactions

1. The physics wrappers PH_SHELL / PH_DESTROY and the own car sites MH_CAR_A / MH_CAR_B report each blast (position,
   node, owner) into a small queue; nothing else happens inside the game's script call.
2. The input tick queries the quadtree around each blast (`chain_radius`, mask 440h, mode 1, as do_explosion) and
   schedules every explosive destroyable (IsExplodeable, alive, not Locked, not ImmuneToDamage) and every car (not
   Destroyed, not the one Ram sits in) at `now + delay`, delay = min + (max - min) x (0.6 x distance / radius + 0.4 x
   random).
3. MH_DO_HIT: do_explosion's own instant `take_a_hit` on an explosive destroyable is held back and scheduled the same
   way (so a cluster ripples instead of popping in one frame); everything else it hits gets the original call.
4. A due entry is checked again (still registered, same class, same placeholder, still at the scheduled spot, still
   alive) and gets `take_a_hit` {`chain_damage` 5000, the first explosion's owner, the source as inflictor, impulse
   away from the source, type 1}. The game's own death / explosion / fx / sound / score follow, and its explosion is
   a new blast with depth + 1.
5. Limits: `chain_max_depth` links, `chain_max_per_second` (token bucket, burst = the rate), `chain_max_per_frame`,
   32 pending at most, no node twice. Rewind drops every pending link.

### Breakable cover

* Every 0.5 s the destroyables within 45 m are looked at; a non-explosive one whose model or placeholder name matches
  `cover_props` (and not `cover_exclude`) gets Health x `cover_health_scale` (written without the member-write notify:
  never in a save; re-applied when the pool re-initialises the resource).
* Each frame the tracked ones are watched; IsDead -> the break: debris pieces from the prop's collision box (CM pivot,
  else every pivot but `Replace_*`), wreck hidden for 2 s (the swap comes a few frames after IsDead), enemies within
  `expose_radius` get `command_crouch_out`.
* Blasts also hit cover props within `cover_blast_radius` (an extra `take_a_hit`).

### Debris

Box3D bodies in the physics world (they collide with the level mesh, props and each other; blasts and bullets push
them), `debris_pieces` per broken prop (planks / slats of the prop's box, outward burst plus a push away from Ram),
`blast_debris` small shards per exploding barrel / tank. At most `debris_max`, the oldest go first; each lives
`debris_life` s and shrinks away in its last 0.8 s. Drawn by `mh_draw.cpp` through MH_DRAW: GfxInternal::Render
0x421B30 draws the world layers (0x421C66), then calls the frame-end motion blur 0x45D5E0 (0x421C71) before the
overlay layers; the pieces are drawn right before that call with the device's own VIEW / PROJECTION (the world layers
set them), depth-tested, so they sit in the world, get the motion blur and stay under the HUD. Procedural textures
(wood planks, metal, chipped red paint), per-face sun shading. Every touched D3D state is read back and restored; the
textures are D3DPOOL_MANAGED (no state block, nothing that blocks a device Reset).

### How the game handles destroyables (from `debug_survey` runs)

* Destroyables are a **pool**: about 10-11 Destroyable_Object resources (spatial mask 60h) are handed to the nearest
  `destructable_object_placeholder`s (mask 1400h; 100-190 per area). Walking 20 m re-assigns them (the physics module
  logs "dropped (disabled)" when that happens). Explosive objects 60 m away simply do not exist yet.
* The mesh entities (Current_mesh +138h, Default_mesh +134h, Replacement +140h) are plain nodes with vtable 0x9D1F98
  (a no-op Render); the drawn Model is a separate Model node at the object's place.
* Health +114h: crates 0 .. 1 ("Crate_Small", BreakOnSecondImpact), chairs / benches / bins / signs 8, burrito wagon
  350, explosive barrels and pressure tanks 0 until hit (IsDead is the only safe "still there" test). ExplosiveRange
  4 .. 5 m, ExplosiveDamage 100 .. 300.
* `command_destroy_object`'s explosion call (PH_DESTROY) did fire for the meat-factory barrels but never for the
  Downtown burrito wagon; do_explosion's query (MH_DO_QUERY) sees every one.
* Vanilla: an explosive destroyable within another's ExplosiveRange gets that blast's `take_a_hit` in the same frame;
  barrels thrown by a blast often go off on impact too (Destroyable_Object_Move_Ctrl, DamagedByImpact).

## Saves

No data entities are created and nothing is written through the game's member-write notify: the Health scaling and
the debris are runtime only. The hidden wreck uses Node::SetFlags (a builtin property; a save made in the 2 s after a
break may keep that wreck hidden, which is what the player saw). Chain detonations are ordinary game damage.

## Testing

Checked on the test copy with The Ambush save and a no-damage test setting:

* **Chain reactions**: three pressure tanks 6 m apart (beyond their 4 m ExplosiveRange), same rig and keys with the
  feature on and off. Vanilla gives one blast. With `[mayhem]` the first tank sets off the second 0.69 s later, which
  sets off the third 0.68 s after that, and Box3D throws the crates, the shards and the planks.
* **Breakable cover**: crates break into wooden planks that Box3D throws (bullets and blasts), with the game's own
  broken model hidden.

The test rig (test copy only):

| Key | What |
|---|---|
| `debug_place_ph = pressuretank` | moves the Havok *placeholders* in front of Ram (the pool then allocates their tanks there) |
| `debug_ignite_key` | hits the nearest explosive destroyable |
| `debug_place_cover` | puts the nearest cover prop 1 m in front of the nearest enemy |
| `debug_hit_key` | gives that prop a 10 hp hit |
| `debug_survey` | logs every destroyable, car and Havok placeholder |
| `debug_shots = 1` | makes the DLL ask the harness for frame bursts at the first blast and the first cover break |

The tests ran through an in-game test harness on a copy of the game (not included).

## Performance (measured in game, `[mayhem] log_seconds` statistics)

* Tick (input tick, everything but drawing): 0.004 .. 0.02 ms average per frame when nothing happens; 0.1 ms average
  in the 5 s of a 3-tank chain; worst frame 6.2 ms = the frame a blast spawns ~22 Box3D debris bodies and sends the
  cover hits. Quadtree query per blast 0.014 ms; one detonation (`take_a_hit`) 0.17 ms average, 0.36 ms max.
* Debris draw (34 pieces): 0.04 .. 0.08 ms average per frame; the first draw makes the three textures once (8.2 ms).
* The physics step with 34 extra pieces: 0.07 .. 0.17 ms average per 0.04 s step.
* Survey / debug keys cost 20 .. 66 ms per use (test copy only).

## Known issues and untested cases

* **Enemies behind cover**: TOD has no cover points (see "AI and cover"); the "exposure" is the hidden wreck (no
  collision, bullets pass) plus `command_crouch_out`. The code path ran in game but no enemy was within 2.6 m of a
  broken prop in the recorded runs, so the stand-up of a crouched enemy is **not verified** in game.
* Cars as links: MH_CAR_A / MH_CAR_B and the car take_a_hit are byte-checked and host-tested, but not seen in game (no
  car came within 9 m of a test blast).
* The cover debris box comes from the prop's collision only while its Model is visible; crates whose Model is a
  pooled one fall back to a named default box (Crate_Small 0.64 x 0.60 x 0.64 m).
* Only the ~11 destroyables the pool has handed out near Ram exist; a chain cannot reach an object the game has not
  allocated yet.
* Health scaling barely matters for crates (Health 0 .. 1 already); it matters for chairs, benches, bins (8 -> 2.8).
* The debris is drawn with procedural textures (wood / metal / red paint), not the prop's own texture.
* Exploded barrels / tanks keep IsDead 0: the chain remembers every node that blew up (60 s) so none is set off twice.

## Settings

Copy `dist\TODGameplay.dll` and `dist\TODPhysics.dll` next to `TOD.exe`, and in `TODGameplay.ini`:

```
[physics]
enabled = 1
[mayhem]
chain_reactions = 1
breakable_cover = 1
```

Every other key is in `dll\TODGameplay.ini` section `[mayhem]` (the code defaults equal it).
