# [ragdoll]: active ragdolls for the original Total Overdose

An independent take on the "active ragdoll" idea (Euphoria is NaturalMotion's; none of its code or data is used):
Box3D bodies driven by joint springs toward the animated pose.

* A hit knocks the hit body part and weakens its muscles for a moment. The body staggers and is pulled back into the
  animation.
* On death the muscles fade and the body goes limp, tumbles and settles.
* Explosions throw bodies, bullets push them.

It is built on the [physics] module's Box3D world (`TODPhysics.dll`, the same level collision as the props, see
[physics/README.md](../physics/README.md)). **Off by default** (`[ragdoll] enabled = 0`): with it off nothing is
patched.

## What it looks like in game

Checked on the test copy in The Ambush (factory) and on the Downtown plaza:

* The game's own grenade shell explodes next to an enemy: the limp ragdoll is thrown up out of the picture and lands
  6 to 15 m away.
* Three hits in 0.7 s: the body bends over its planted feet, reels and is pulled back into the animation (recovered
  1.9 s after the first hit). One hit recovers after 1.16 s.
* A rifle burst staggers an enemy and the last bullet kills him mid-stagger: the powered ragdoll goes limp and falls
  backwards.
* Killed standing: the muscles follow the death clip for 0.35 s, then go limp; the game's death-pose corpse takes over
  the pose 4 s later.
* With `debug_draw = 1` the 13 capsules of a settled corpse can be projected with the game camera over a screenshot.

The hit tuning was compared against an older, softer build on the same save with the same scripted pistol shots
(the "inflatable tube man" problem). Three quick forearm hits used to fold the body double and throw the rifle arm
straight up; now the body stays upright and flinches. One forearm hit now lifts the rifle briefly and settles back
(still a visible arm jerk). A killing burst used to fling the legs up; now the body staggers upright under the hits and
drops.

## Performance

Measured in game from the `ragdoll: stats` lines:

* Per ragdolled character per frame (read the animated pose, forward kinematics, write 48 bone locals): 0.01-0.04 ms
  average, up to 1.7 ms in the frame a ragdoll is created (log lines included); creating one costs 0.08-0.6 ms.
* Box3D step (the whole world: 17 props, ~1110 static level hulls, 1-2 ragdolls): 0.13-0.28 ms average per 0.04 s step
  with 8 substeps, worst 2.7 ms (a blast frame). Without ragdolls the physics module keeps its 4 substeps (0.003 ms).
* Unit test (32-bit, same DLL code): 6 ragdolls at once cost 0.4-0.6 ms per step, worst 1.9 ms.
* Characters without a ragdoll: one table lookup per Character::Update (ignore list), nothing else.

## How it works

| Part | Where | What |
|---|---|---|
| Bone override | `code/rd_game.cpp`, site RD_CHARUPD 0x897EFD | Scene::Update calls Character::Update 0x910CE0 for every visible character; its skeleton compose 0x90EA50 writes each bone node's local rotation (NodeMatrix +00h, stored = conjugate) and Bip's local position, then marks the bones dirty. The wrapper runs it, reads that animated pose, and for a ragdolled character writes the simulated pose over the locals and marks the bones dirty again. World matrices are lazy and the game skins on the CPU from them, so the mesh, the shadow blobs (Bip / calves), held weapons and the renderer bridge all follow. |
| Hits | site RD_HIT 0x5C535E | character_root's take_a_hit relay hands every hit to the AI; at that call the hit record {damage, owner, inflictor, volume, impulse, position, type} is in esi. It is recorded (bullets: type 0, impulse = unit direction, position = the impact point). |
| Death | AI Current_state 28 (state 0x5D6B70) | or DamageTaken >= Health: the limp ragdoll replaces the death clip (MoveCtrl command_takeahit_death). |
| Simulation | `code/rd_sim.cpp` in TODPhysics.dll | 12 capsule bodies, 11 joints (Box3D spherical joints with cone + twist limits, revolute knees / elbows), joint springs (Box3D's own soft joint springs toward a target rotation / angle), joint friction (a motor at zero speed), a pelvis anchor (kinematic body + motor joint springs with a force cap), ray casts, blasts. Same world as the props and the scanned level mesh. |
| Pure logic | `code/rd_logic.cpp/.h` | bone map, capsules from the rig, joint frames from the bind pose, forward kinematics, the pose written back, motor targets, hand-off velocities, strength / recovery, settling, config. Unit tests in `code/test_rd.cpp` on real skeletons (`code/rd_fixture.inc`, from a renderer-bridge bone probe). |

### The body

| Body | Bone (frame) | Capsule | Mass | Joint to parent | Limits |
|---|---|---|---|---|---|
| pelvis | Pelvis | hip to hip, r 0.13 | 11 | - | - |
| spine | Spine (+ Spine1) | to Spine2, r 0.12 | 10 | sphere | cone 26 deg, twist +-20 |
| chest | Spine2 (+ Spine3, Neck, clavicles) | to Neck r 0.15 + shoulders r 0.075 | 15 | sphere | cone 23, twist +-17 |
| head | Head (+ face bones) | along its -X, r 0.10 | 6 | sphere | cone 43, twist +-52 |
| upper arm x2 | UpperArm | to Forearm, r 0.055 | 2.5 | sphere | cone 92, twist +-69 |
| forearm x2 | Forearm (+ ForeTwist, hand, fingers) | to the hand + 10 cm, r 0.045 | 2 | hinge (+Y of the forearm) | -3 .. 149 deg |
| thigh x2 | Thigh | to Calf, r 0.08 | 9 | sphere | cone 63, twist +-29 |
| calf x2 | Calf (+ foot, toe) | to Foot + 6 cm, r 0.06 | 5.5 | hinge (+Y of the calf) | -3 .. 143 deg |

80 kg in all (`total_mass`). Limits are centred on the model's bind pose (asset pivot records). Knees and elbows bend
about the child's local +Y (every forearm / calf local is a pure +Y rotation, positive = flexion; the rig is mirrored
L/R only in Unreal Engine imports, the game's own transforms need no mirroring). Bones that are not bodies keep their
offset to the body that drives them (fingers, face, twist bones), so the skin never tears: only rotations are written,
positions follow the hierarchy (Bip also gets its position from the pelvis body).

### Modes

* **active** (a non-lethal hit on a living character): a ragdoll is built from the animated pose of this frame with no
  hand-off velocity (in game the frames after a teleport or clip switch gave 4-9 m/s spikes). The feet are held to the
  animated feet (`foot_hertz`, `foot_force`), so a push bends the body over its planted feet instead of pivoting it
  about the hips (without this, standing enemies back-flipped). Every joint spring targets the animated pose
  (`joint_stiffness` Hz, damping `joint_damping`), gravity is `active_gravity`, and the pelvis is held to the animated
  pelvis by a spring capped at `balance_force` / `balance_torque` (a hard push wins: stagger). The hit body part drops
  to `hit_strength`, its neighbours to `hit_neighbour`, and every part recovers over `recover_seconds`. The output is
  blended from the animation and back (`blend_in`, `blend_out`). Pushed further than `stumble_distance`, the character
  node steps along.
* **limp** (death): from the current pose with its velocities, plus the killing hit's impulse and any blast within
  `blast_memory` s. The springs follow the death clip for `death_fade` s and then let go; joint friction damps it. The
  character node follows the pelvis (SetWorldPosition + PhysicCtrl reset_to_parent) so culling, LOD and shadows stay
  right. Once settled (or after `settle_seconds`) it is frozen.
* **frozen**: the bodies are freed; the pose is written every frame until the game removes the corpse. A bullet or a
  blast brings a frozen corpse back to limp.

## Settings (`TODGameplay.ini`, `[ragdoll]`)

| Key | Default | What |
|---|---|---|
| enabled | 0 | the feature |
| death_ragdoll | 1 | limp ragdoll on death |
| active_hits | 1 | powered stagger on non-lethal hits |
| max_ragdolls | 6 | simulated at once (active + limp); a new death freezes the oldest limp one |
| max_frozen | 16 | settled corpses whose pose is kept |
| settle_seconds | 6 | a limp ragdoll freezes at the latest after this |
| impulse_scale | 1.0 | every push on a ragdoll (bullets, blasts, the game's hit impulse) |
| joint_stiffness | 18 | active joint springs (Hz) |
| recover_seconds | 0.7 | a hit body part back to full strength |
| hit_impulse | 60 | N s one bullet gives a live (staggering) body |
| hit_max_dv | 4 | m/s at most for the part a live hit lands on (the rest goes to the torso) |
| active_friction | 4 | joint friction while staggering (0 = the older, swaying recovery) |
| spine_limit_scale | 0.7 | spine / chest / neck bend limits (1 = the full bind-pose limits) |
| death_max_dv | 4 | m/s at most for one part from the killing bullet or a shot on a corpse |
| debug_draw | 0 | test copy: capsule segments + camera in the log; `tools/rd_overlay.py` draws them on a screenshot |

Tuning keys (defaults are the tuned values): `total_mass`, `scale`, `joint_damping`, `hit_strength`, `hit_neighbour`,
`bullet_impulse`, `game_hit_scale`, `blast_scale`, `blast_lift`, `blast_memory`, `death_fade`, `limp_friction`,
`active_gravity`, `hit_core_share`, `hit_burst`, `active_ang_damping`, `death_core_share`, `limp_ang_damping`,
`blast_strength`, `balance_damping`, `foot_damping`, `balance_hertz`, `balance_force`, `balance_torque`, `blend_in`,
`blend_out`, `stumble_distance`, `stumble_rate`, `substeps` (Box3D substeps of the 0.04 s step while ragdolls
simulate), `max_speed`, `track_radius`, `root_follow`, `civilians`, `rest_pose_limits`, `log_hits`, `log_seconds`.

Test copy only: `debug_bring_key`, `debug_hit_key`, `debug_kill_key`, `debug_boom_key`, `debug_freeze_key`,
`debug_turn_key`, `debug_vanilla_key`, `debug_god`, `debug_ai_off`, `debug_shots`, `debug_force_bone` /
`debug_force_angle` and their distances.

## Install

Copy `dist\TODGameplay.dll`, `dist\TODPhysics.dll` and `dist\dsound.dll` next to TOD.exe and set
`[ragdoll] enabled = 1` (optionally `impulse_scale`). `[physics]` may stay 0. TODPhysics.dll exports
`TODPhysics_GetRagdollApi`; the ragdoll API is version 2, so the two DLLs must come from the same build. Nothing in the
data zip changes and saves are not touched (the ragdolls are runtime-only).

## Testing

The tests ran through an in-game test harness on a copy of the game (not included): test copy only, isolated USERPROFILE, everything restored and compared afterwards.
The tools in `tools/` work on that harness's run folders: `make_clip.py` (clips from screenshot bursts),
`side_by_side.py` and `sheet.py` (before / after comparisons and contact sheets), `rd_overlay.py` (draws the
`debug_draw` capsules on a screenshot). `make_fixture.py` builds `code/rd_fixture.inc` for the unit tests, and
`build_test_rd.bat` builds the ragdoll unit test.

## Engine notes (how the engine handles bodies)

* Bone pipeline: Scene::Update 0x897450 -> Character::Update 0x910CE0 (call 0x897EFD) -> UpdatePose 0x90FFF0 (layers
  0x902F20 / 0x906540, blend 0x9067E0) -> compose 0x90EA50 (local rotation 0x5A1710 into NodeMatrix +00h, Bip /
  GamePivot position 0x88BB20, then +14h |= 2 on every bone). World matrices are lazy (0x892940), the renderer skins
  on the CPU. The override runs right after Character::Update returns. A bone with node flag +28h & 4 is skipped by
  the compose (an engine-side lock that is not needed here).
* Rig: limb long axis = local -X; elbows and knees are pure +Y rotations of the child (positive = flexion) on both
  sides. The L/R mirroring seen in Unreal Engine imports is an import artefact; the game's own transforms need none.
* Hits: take_a_hit relay 0x5C4F40 -> SendCommand(AI, 0x534) at 0x5C535E (esi = the hit). Bullets carry a unit
  direction as "impulse" (type 0 pistols / 1 rifles), melee ~200 N s (type 2), explosives ~1500 N s, damage 350.
* Death: sentry state 28 (0x5D6B70). The corpse is handled by character_Take_A_Hit_Move_ctrl state 0x601410: a
  character_deathpose placeholder is taken over, after 0.5 s (RD_CORPSE: `corpse_seconds`) it is placed at the body
  and shown, the body fades out (command_fade_out 0.5 s, opacity Character +1B8h) and the enemy's placeholder is
  disabled, so the pooled shell goes back (giveup_resource). In the vanilla game the body is gone 3 s after the death.
  The ragdoll pose is handed to that death-pose corpse (it is placed at the Character node of the body), which then
  stays. Over-kill (damage far above the health, e.g. a grenade on a civilian) gibs the body (FUN_005D2D20): no ragdoll
  then.
* Controller: a living character's root belongs to its PhysicCtrl / AI. The active ragdoll never moves it, except a
  stumble step (at most 12 cm per frame, SetWorldPosition + reset_to_parent). A ragdoll whose character the game moves
  more than 1.5 m away (cars, scripts) is released. A corpse's root follows its pelvis the same way.

## Known issues and untested cases

* Stairs and ledges: a body falling down stairs or off a ledge has not been filmed. Bodies collide with the level mesh
  the [physics] scan builds (40 m around Ram, rescanned when nothing moves); a body thrown out of it lands on nothing
  and is frozen after falling 60 m.
* Hits: a single forearm hit still lifts the rifle arm for about 0.2 s. `hit_max_dv` 2-3 makes it smaller (not tried
  in game). Shotguns and melee have not been filmed with the current tuning. Melee and explosive death pushes are
  unchanged.
* The stagger is milder in game than in the offline sweep (`RD_SWEEP=1 test_rd.exe`) for the same push; the defaults
  were raised for that (`bullet_impulse` 110). Not tuned against shotguns (10 pellets = 10 hits).
* Saves: a checkpoint saved during the `corpse_seconds` window can bring that enemy back on reload (the game's "stays
  dead" mark comes later). Rewind (TAB) is not handled specially: ragdolls keep their pose while it runs.
* Corpse pool: at most `corpse_max` corpses wait at once, but BRUTAL's live cap 9 and gang wars share the 9 shells.
* Cut-scenes, vehicles (characters inside cars) and the player's own death are untouched (the player is never
  ragdolled).
* No level geometry under the pelvis (outside the [physics] circle): no ragdoll, the game's own death (logged "has no
  level geometry under it", counted as "outside the circle" in the stats line).
* Not tried with the renderer bridge running (it reads the same bone locals at render time, so it should send the
  pose).
