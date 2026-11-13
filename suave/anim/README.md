# Suave: animations

The intro, dance, ring, push, stumble and look-down clips of the Suave loco move, in the game's animation format, as
glTF for the web trainer, and as preview videos.

## Credits and licences

- Dance motion: **CoMPAS3D, Rosie-Lab** (Simon Fraser University), "CoMPAS3D: A Dataset and Benchmark for
  Interactive Motion", https://huggingface.co/datasets/Rosie-Lab/compas3d. Licence **CC-BY-NC-4.0**: credit the
  authors; non-commercial use only. Every Suave dance clip (lead, follow, push, stumble) is derived from it. The
  dataset's song audio is not used.
- Body model: SMPL-X (used to compute the joints only; its licence applies to the model files, which are not
  copied here).
- Ring stand-in: Rokoko free mocap pack, `GentleSwayingDancing_mixamo.fbx`.
- Music in the previews: Elvis Crespo, "Suavemente" (a local copy; preview use only, the videos are never committed).

## The chosen section

CoMPAS3D **Pair5 (professional), song 2 ("Lluvia"), take 1, 13.84 s to 28.36 s**: six annotated 8-counts.

| clip beats | source | what happens |
|---|---|---|
| 0-4 | 13.92-16.20 | cross-body lead with an inside (left) turn for the follower, open hold |
| 4-8 | 16.20-18.64 | right turn for the follower, crossed hold |
| 8-12 | 18.64-21.08 | change of places, comb, late right turn for the leader |
| 12-16 | 21.08-23.52 | both turn (follower right turn + leader right turn): the spin |
| 16-20 | 23.52-25.92 | inside turn with no hands, then the leader's hand on the follower's waist (styling) |
| 20-24 | 25.92-28.36 | open hold that ends in a "break": the follower is sent out at arm's length |

Why: it has the arc the move needs (hold, turns, a spin, styling, and a natural send-away at the end that the push uses),
it starts and ends in an open hold, so it loops, and at beat 19 Ram's right hand is on the partner's back with no
grip in his other hand: the frame where the dynamite is planted.

Time: one salsa count = half a merengue beat, so one 8-count = one 4-beat bar of Suavemente. The source runs at
~200 counts/min; Suavemente is **124.17 BPM** (`src/bpm.py`, beat grid 0.373 s + k x 0.48321 s), so the dance plays
1.25x faster than recorded. The six annotated 8-count starts are fitted to beats 0, 4, ..., 24. Clip = 24 beats =
11.597 s; frame 0 is a downbeat.

## Pairings (matched sets)

One source, retargeted per pairing: Ram (`ram.model`, 1.91 m) leads; the partner model follows. Each pairing has
its own lead clip (Ram's arms differ per partner because of the contact fixing).

| pairing | partner model / height | lead / follow / relative scale | grips (5 holds): source grip kept within | hand on body (5) | interpenetration | partner pushed out |
|---|---|---|---|---|---|---|
| soldier | soldier.model 1.84 m | 1.208 / 1.265 / 1.119 | 0.0 cm, 4.0 cm at the one hand-switch | <= 0.8 cm | none (clearance >= 0.9 cm) | <= 8.6 cm |
| civil | civil.model 1.90 m | 1.208 / 1.305 / 1.156 | 0.0 cm, 3.7 cm at the hand-switch | <= 1.6 cm | none (>= 0.6 cm) | <= 11.3 cm |
| dea | dea_taskforce.model 1.90 m | 1.208 / 1.377 / 1.170 | 0.0 cm, 3.5 cm at the hand-switch | <= 1.2 cm | none (>= 0.9 cm) | <= 6.9 cm |

- Grip metric: palm-centre distance between the two holding hands compared with the source's own grip (scaled);
  palm centres sit 14-19 cm apart when gripping, as in the source. In the last 0.3 s (the loop cross-fade, after
  the release) a grip opens by up to 9.7 cm.
- Feet: soles on one ground (model-space y = -1.09), balls locked during source contacts, lowest toe 0.0 / -0.4 /
  +0.3 cm from the ground, planted-frame slide 0.7-3.5 mm per frame (mean).
- Pose fidelity: every retargeted bone points where its source bone points (direction error 0 for limbs before IK);
  L/R checked objectively: the TOD L Hand stays on the source left wrist's side of the pelvis in 99 % of frames.
- Codec: every clip re-encodes byte-identically; FK of the decoded clip matches the built joints within 0.2 mm;
  the loop clips' last frame equals the first.

## Clips

`clips/game/*.json` (game format, built into `overdose.main` by the `suave_anims` data option), catalogue
`clips/game/clips.json`; glTF in `export/` with `export/manifest.json`.

| clip | character | frames @25 | CPS | loop | stand-in |
|---|---|---|---|---|---|
| suave_lead_{soldier,civil,dea} | Ram | 291 | 0.086229 | yes | no |
| suave_follow_{soldier,civil,dea} | partner | 291 | 0.086229 | yes | no |
| suave_push_{soldier,civil,dea} | Ram | 55 | 0.463083 | no | yes: the dance from beat 19 on (hand on the back = plant, then the break) |
| suave_stumble_{soldier,civil,dea} | partner | 55 | 0.463083 | no | yes: the follower's break (sent out at arm's length) |
| suave_circle | any biped (authored on soldier.model) | 291 | 0.086229 | yes | yes: Rokoko sway with raised arms, ONE SWAY PER BEAT (2-beat left-right cycle x 12), in place, feet planted |
| suave_intro_{soldier,civil,dea} | Ram | 73 | 0.344917 | no | no: 6 beats, hands up, steps in, takes the hand on beat 4, pulls into the hold; last frame = lead frame 0 |
| suave_grabbed_{soldier,civil,dea} | partner | 73 | 0.344917 | no | no: turns in surprise, hand taken on beat 4, pulled in; last frame = follow frame 0 |
| suave_lookdown_{soldier,civil,dea} | partner | 39 | 0.666667 | no | no: 1.5 s from the stumble's last frame: looks down at the belt, hunched, hands by the chin, frozen |

### Arms
All clips use the game's own Biped arm convention, measured on 390 vanilla Ram clips (`src/armtwist.py survey`):
Forearm = pure hinge about the UpperArm's local +y, the forearm roll in the Hand shared 1/3 ForeTwist, 2/3 ForeTwist1
(`retarget.canonical_arms`; positions and hand rotations kept, so contacts do not move). Metrics per arm
(`python armtwist.py npz <npz> ram <partner>`): elbow roll, hand roll, skin candy on the real mesh weights, twist
jumps, hinge-axis and elbow-pole turn per frame. Before -> after: elbow roll 55-78 -> 0-0.5 deg, skin candy 32-78 ->
21-37 deg (vanilla up to 42), elbow flips 3 -> 0. Close-ups: `previews/arms/arms_<pairing>_before_after.png`. The
arrays from before the fix are kept in `work/sa_before/` (git-ignored).

### Face bones
Game clips carry the vanilla idle's static face channels (as every vanilla body clip does); the glbs leave the face
bones unanimated so the trainer layers `ram_facial_expression_*` on them.

Release (the switch to push / stumble): dance frame 230 at 25 fps (PlayPos 0.7931, 9.181 s, beat 19); 276 at 30 fps.
How the game plays them: [the Suave README](../README.md).

Conventions: GamePivot is static (the vanilla idle value), so no root motion moves a character's node; Bip carries
all movement. Partner node = Ram's node (shared frame). Soles at model-space y = -1.09 for every model.

## Previews

`previews/suave_{soldier,civil,dea_taskforce}.mp4` (1280x720, 30 fps, about 16.4 s; rendered locally and git-ignored,
since they carry a cut of the song) and `previews/suave_contact_sheet.png`. Blender 5.2 Eevee, the staged game meshes
and textures (the same glbs the trainer uses), CPU skinning of the exported joint matrices, mirrored to the game's handedness; 8 soldiers (three looks) on
a 3 m ring doing the one-sway-per-beat circle clip, one beat apart. Timeline: intro / grabbed (6 beats), dance
to the release, push / stumble, the partner's lookdown (Ram holds), hold. Camera in front of the pair (perpendicular
to the Ram-partner line), inside the ring. Music: the segment per pairing (`suave/audio/build/suave_mus_<k>.ogg`,
soldier 1, civil 2, DEA 3; its time 0 = the intro's beat 0).

## Rebuild

```
cd suave\anim\src
python skeletons.py                         # bind skeletons from the GOG blocks.naz (read only)
python bpm.py                               # tempo
python pair.py soldier --fps 25             # also civil, dea_taskforce; --fps 30 for glb / previews
python circle.py --fps 25                   # and --fps 30 (needs work/rokoko/*.npz from bl_fbxdump.py)
python intro.py soldier --fps 25            # after pair.py; also civil, dea_taskforce; --fps 30
python lookdown.py soldier --fps 25         # after pair.py; also civil, dea_taskforce; --fps 30
python armtwist.py npz ..\work\pair_soldier_25.npz ram soldier    # arm metrics (survey = vanilla convention)
python closeup.py soldier BEFORE.npz AFTER.npz OUT.png            # arm close-ups (Blender)
python export_tod.py                        # game clips + checks -> clips/game
python export_glb.py --circle               # glbs + manifest -> export
python render_all.py                        # previews (about 25 min)
cd ..\..\..\data
python build_gameplay_mod.py --game "C:\GOG Games\Total Overdose" --set suave_anims=on --out ..\work\x\todgameplay_data.zip
python ..\suave\anim\src\check_package.py ..\work\x\todgameplay_data.zip
```
Python 3.12 with numpy, scipy and Pillow; Blender 5.2; ffmpeg. The data option
itself needs only the stdlib.

## Files

- `src/smplx_fk.py` SMPL-X FK (numpy), `src/retarget.py` SMPL-X / Mixamo -> TOD Biped (mirror, rest alignment,
  2-bone IK), `src/pair.py` one pairing (time warp, scaling, contacts, IK, loop), `src/circle.py` ring loop,
  `src/export_tod.py`, `src/export_glb.py`, `src/preview.py` + `src/bl_preview.py`, `src/bl_fbxdump.py`,
  `src/check_package.py`, `src/survey.py` / `src/sticksheet.py` / `src/todsticks.py` (section choice).
- Arms, intro and look-down: `src/armtwist.py` (arm metrics + vanilla survey), `src/intro.py`, `src/lookdown.py`, `src/closeup.py` (arm
  close-ups before / after), `src/stills.py` (wide stills of any pair npz).
- `lib/`: copies of the animation codec and model reader from the author's reverse-engineering tools (not included).
- `data/gameplay/suave_anims.py` (in the data builder): the `suave_anims` option.
