# Ragdoll (`[ragdoll]`)

## What it does

A hit knocks the body part that was hit; the enemy staggers and is pulled back into his animation. On death the
muscles fade, the body goes limp, tumbles and settles. Explosions throw bodies and bullets push them. It is a
from-scratch active ragdoll on Box3D (no NaturalMotion / Euphoria code or data).

## How it is implemented

1. **Engine facts.**
   - Bones: Scene::Update 0x897450 calls Character::Update 0x910CE0 at 0x897EFD for each visible character. Its
     compose 0x90EA50 writes each bone's local rotation (NodeMatrix +00h, stored as the conjugate) and Bip's local
     position, then marks the bones dirty (+14h |= 2). World matrices are lazy (0x892940) and the game skins on the CPU
     from them. Nothing reads the bones between that call and the render.
   - Rig: limb long axis = local -X; knees and elbows are pure +Y rotations of the child, positive = flexion, on both
     sides.
   - Hits: character_root's take_a_hit relay 0x5C4F40 hands every hit to the AI at 0x5C535E with esi = the hit record
     {damage, owner, inflictor, volume, impulse[3], position[3], type}. Bullets: type 0 / 1, impulse = a unit direction.
     Melee type 2 (~200). Explosives ~1500, damage 350.
   - Death: sentry state 28 (0x5D6B70). The corpse is handled by character_Take_A_Hit_Move_ctrl state 0x601410: take a
     death-pose placeholder, SLEEP 0.5 s, place and show it, fade the body out, give the pooled shell back. Vanilla:
     the body is gone 3 s after death.
2. **Bone override.** RD_CHARUPD 0x897EFD retargets the Character::Update call. The wrapper runs it, reads the animated
   pose, and for a ragdolled character writes the simulated rotations over the locals and marks the bones dirty again.
   Mesh, shadow blobs, held weapons and the renderer bridge all follow.
3. **Hits.** RD_HIT 0x5C535E records each hit record, then calls the original.
4. **Corpse delay.** RD_CORPSE 0x606D44 (JMP hook on the first 0.5 s SLEEP) stretches it to `corpse_seconds` for
   enemies, so the limp body is seen before the death-pose corpse takes over its pose. At most `corpse_max` wait at once.
5. **The body** (`ragdoll/code/rd_sim.cpp` in TODPhysics.dll): 12 capsules (pelvis, spine, chest, head, upper arms,
   forearms, thighs, calves), 80 kg in all; spherical joints with cone and twist limits, hinge knees and elbows;
   joint springs toward a target rotation; joint friction; a pelvis anchor with capped force. Same world as the props.
6. **Modes.**
   - active (non-lethal hit): built from this frame's pose, feet held to the animated feet, springs toward the
     animation, the hit part drops to `hit_strength` and recovers over `recover_seconds`, blended in and out.
   - limp (death): from the current pose plus the killing impulse and any recent blast; springs follow the death clip
     for `death_fade` s, then let go. The character node follows the pelvis.
   - frozen: settled bodies freed; the pose is still written until the game removes the corpse. A bullet or blast
     brings a frozen corpse back to limp.
7. **Without `[physics]`.** With `[physics] enabled = 0` and `[ragdoll] enabled = 1` the module builds only the level
   world (`g_world_only` in `phys_game.cpp`). The installer still requires Physics for this card.

## Telling a live enemy from a corpse

A pooled shell handed to a new placement still reads the last occupant's AI state 28 for a moment, and a re-armed
reinforcement can arrive with a lethal DamageTaken (see [difficulty.md](difficulty.md)). Treating either as a death
would turn a live enemy into a limp ragdoll from its spawn pose, pin its character node to the ragdoll's pelvis (so it
cannot move) and freeze it in that pose while its AI walks on. In the log that looks like
`died (AI state 28, mode none)` within 35 to 80 ms of a `reinforcement ... re-armed` line. The rules that prevent it:

- A death ragdoll needs the character seen alive (3 updates: not state 28, DamageTaken < Health) in this placement; the
  track starts over when the shell's placeholder changes. A DamageTaken >= Health without state 28 is a death only with
  a hit on it in the last second (`rd::life_alive` / `rd::life_dead`, unit tested).
- A limp body's node follows the pelvis only while its AI is in the death state; a corpse whose AI has left state 28
  is released.

## How bullet hits are spread over the body

A fixed push on the part that was hit makes a shot enemy flail like an inflatable tube man, worst on arm hits. With a
flat 110 N s per bullet, capped only at 48 m/s, a 2 kg forearm took the full 96 N s (48 m/s): the arm whipped over the
head and dragged the weak torso (hit part at 15 % strength, spine 1.1 damping, half gravity), and the foot anchors then
swung the whole body back past the pose. In the offline lab the chest folded 63 deg and swayed back 0.36 m, and the
hand reversed direction 6 to 10 times. A killing bullet had the same push on a limp body, so the legs flew up as it
fell. The current rules (`rd::share_push`, `rd::active_drive`, `rd::tighten_spine`):

- A live bullet pushes `hit_impulse` 60 N s. The part that was hit takes at most `hit_max_dv` (4 m/s) and half of
  the push; the rest goes to the torso (arms and head go to the chest, legs to the pelvis).
- While a body staggers: joint friction (`active_friction`), damping 2.5, full gravity, hit muscles at 40 %.
- Spine, chest and neck bend limits are scaled by `spine_limit_scale` (0.7), but never tighter than the pose
  already is.
- The killing bullet and shots on corpses are capped the same way (`death_max_dv`).
- Explosions keep their full impulses. A blast puts the bodies it reaches back on the base damping, gravity and joint
  friction (without the stagger settings above), so they still fly as far.

In the lab the chest now bends 17 deg with no sway back; the hand speed after a forearm hit drops from 11.9 to 1.9 m/s,
and on death from 10.2 to 4.3 m/s. In game the same save and the same scripted shots were compared side by side with
the old and new DLL.

## Ini keys (`[ragdoll]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature (needs TODPhysics.dll) |
| `death_ragdoll` | 1 | limp ragdoll on death |
| `active_hits` | 1 | powered stagger on non-lethal hits |
| `max_ragdolls` / `max_frozen` | 6 / 16 | simulated at once / settled corpses kept |
| `settle_seconds` | 6 | a limp body freezes at the latest after this |
| `corpse_seconds` | 4 | the dead body stays this long before the death-pose swap (vanilla 0.5) |
| `impulse_scale` | 1.0 | every push on a ragdoll |
| `joint_stiffness` | 18 | active joint springs (Hz) |
| `recover_seconds` | 0.7 | a hit body part back to full strength |
| `hit_impulse` | 60 | N s one bullet gives a live (staggering) body (instead of the 110 of `bullet_impulse`) |
| `hit_max_dv` | 4 | m/s at most for the body part a live hit lands on; the rest of the push goes to the torso |
| `active_friction` | 4 | joint friction while staggering: no sway back past the pose (0 = loose, wobbly joints) |
| `spine_limit_scale` | 0.7 | spine / chest / neck bend limits (1 = the full limits) |
| `death_max_dv` | 4 | m/s at most for one body part from the killing bullet |

More tuning keys with code defaults are listed in `ragdoll/README.md`. Test-only: `debug_draw`, `debug_bring_key`,
`debug_hit_key`, `debug_kill_key`, `debug_boom_key`, `debug_freeze_key`, `debug_turn_key`, `debug_vanilla_key`,
`debug_god`, `debug_ai_off`, `debug_shots`, `debug_force_bone`, `debug_force_angle`.

## Testing and limits

- Unit tests `ragdoll/code/test_rd.cpp` (327 checks on real skeletons from a bridge bone probe, including the hit lab:
  `RD_HITLAB=old,new build\test_rd.exe`); host test 23.
- Hit behaviour in game: before / after runs on a test copy of the game (The Ambush save, the same pistol shots from
  the `debug_sequence` each time).
- Checked in game on a test copy of the game: a grenade throws a dead enemy
  6 to 15 m; three hits in 0.7 s stagger and recover; a rifle burst kills mid-stagger and the body falls limp. The
  combined test had 20 limp ragdolls in one fight.
- Cost: 0.01 to 0.04 ms per ragdolled character per frame; Box3D step 0.13 to 0.28 ms per 0.04 s step with 1 or 2
  ragdolls.
- Not verified: stairs and ledges; shotguns (10 pellets = 10 hits); rewind; the renderer bridge running.
- Risks of the corpse delay: each waiting corpse holds one of the 9 pooled shells; a checkpoint saved inside the
  delay can bring that enemy back on reload; with no free death-pose placeholder the game skips the corpse.
- The player, cut-scenes and characters in cars are never ragdolled. Over-kill gibs a body (no ragdoll then).
- A body with no level geometry under its pelvis (outside the physics module's scanned 40 m circle: a far kill, or a
  car that outran the circle) gets no ragdoll and keeps the game's own death, so it never falls through the world. The
  world also steps while Ram drives, so a character hit by his car ragdolls.
- EXPERIMENTAL in the installer.

## Files

`ragdoll/code/rd_game.cpp`, `rd_logic.cpp/.h`, `rd_sim.cpp`, `rd_fixture.inc`, `test_rd.cpp`, `ragdoll/tools/`,
`ragdoll/README.md`, `dll/HOOKS.md` "Ragdoll".
