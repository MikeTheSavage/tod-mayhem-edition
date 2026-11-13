# Suave Trainer

A 3D "training room" in the browser for previewing the new Total Overdose loco move **Suave** without the
game: a 3 s intro (title, time freeze, Ram raises his hands, takes the partner's hand and pulls them into
the hold), the dance while the enemies form a ring and outside shooters miss (Ram has the move's armour, the
rounds spark on the ground), the lit dynamite slipped into the partner's belt, the partner spun away while Ram
launches a slow-motion shoot dodge, the partner looking down terrified, and the blast behind Ram mid-air. The game is
never launched; everything runs from the extracted game meshes and the Suave clip and music exports.

## Run

```
cd suave\trainer
npm install            # once
npm run dev            # http://127.0.0.1:6390
```

Paths come from `config.local.json` (git-ignored; copy `config.example.json`). The dev server reads them
at runtime, read only. Relative paths resolve from the folder the dev server starts in (`suave/trainer`).

| key | what | example default |
|---|---|---|
| `todOut` | the game character glbs (`ram/soldier/civil/dea_taskforce.charanim.glb`, TODViewer exports) | empty: set it |
| `saExport` | the Suave clips + `manifest.json` (`suave/anim/export`) | `../anim/export` |
| `sbAudio` | the Suave music segments (`build/*.ogg` + `build/manifest.json`, made by `suave/audio`) | `../audio` |
| `musicPath` | your own copy of the song, for the longer song cuts on the 18 s design timeline | empty: optional |

The mp3 and any copy or cut of it are never committed (`.gitignore`, `../previews/.gitignore`).

## What you can do

- **Timeline**: play / pause (Space), restart (R), scrub, phase markers (Activate, Gather, Dance, Plant,
  Escape, Boom; click a phase to jump), beat and bar ticks, speed 0.25x-2x, frame step with the arrow keys.
- **Move setup**: pairing (Ram + soldier / civil / DEA), timing, music, ring size 0-8, ring radius
  (default 3 m), circle-dance speed, outside shooters on/off, use the Suave clips on/off.
  - *Timing "Game"*: a 6-beat intro, then the beat-locked timeline of the music segments (segment beat 0 = dance
    frame 0, ring walks in on dance beats 0-4, release at the clips' release frame, push/stumble clip, boom on the
    segment's beat 28), the slow-motion dive around the boom, landing, 19.0 s in all.
  - *Timing "Design doc"*: the design's phases shifted by the 3 s intro (Activate 0-3, Gather 3-5, Dance 5-15,
    Plant 15-16, Escape 16-18.05, Boom 18.05-20.65; the escape is longer because the boom lands inside
    the slow-motion flight).
  - *Music "Auto"*: per partner type, soldier = segment 1, civil = 2, DEA = 3, else 4. Cuts of the mp3 at
    the segment beats (32, 64, 192, 448) with a 6-beat pre-roll, so the music fades in on the intro's first
    downbeat; the 28-beat segment files are listed too (they start at the dance, the intro is silent with them).
- **Cameras** (keys 1-6): *Cuts* (the default; the design's shot list: intro push-in with the SUAVE
  title and time freeze, over Ram's shoulder as he takes the hand, two-shot as he pulls the partner in; a
  high orbiting wide while the ring forms; a cut on every bar line during the dance: low-angle hero, over
  the partner's shoulder on Ram's face, a ring member's view, over Ram's shoulder on the partner's face,
  wide; 4a close-up of the dynamite going into the belt; 4b low in front of Ram as he spins the partner
  away and launches the slow-motion dive; 5 the partner looking down at the sparking dynamite; 6 front on
  Ram mid-dive with the blast behind him, then his landing), *Gameplay* (over the shoulder), *Front
  three-quarter*, *Top-down*, *Orbit*, *Free orbit* (mouse).
- **Faces**: the game's own facial clips (`ram_facial_expression_*`) layered on the face bones only, per
  moment: Ram slicksmile / joy, the partner surprise, disbelief, joy, then terrified (surprise into pain);
  the ring joy, surprised when the partner looks down, pain when thrown. They work on Ram, soldier, civil and
  DEA (soldier.model has no eyebrow weights, so his brows don't move; chin, mouth and eyelids do).
- **No shield**: Ram has the move's armour (god mode, as in the other loco moves); outside rounds show
  tracers and muzzle flashes and spark with a puff of dust on the ground around him.
- **Debug** (G): grab 5 m / ring / pull 15 m circles, the dive lane, ring slots, skeleton lines, and the
  hand-contact readout. With the Suave clips it measures all four hand pairs (the grips sit 14-19 cm palm to
  palm, as in the mocap source) plus a whole-dance summary; with the placeholder it shows the IK hold.
- **Play as Ram** (P, or the button): WASD walks Ram around the yard (Shift runs, drag to look), 14 idle
  or patrolling enemies; **E** next to an enemy (within 5 m) starts Suave on the nearest one with the
  ring pulled from 15 m and the farther ones shooting; the knocked-down stay down; R resets, Esc leaves.
- **Record**: the button saves a WebM of the current camera (canvas + music, no DOM title).

## Clips: the real Suave clips, placeholders as fallback

The app polls `suave/anim/export/manifest.json` every 5 s and reloads the clips whenever the manifest
changes (re-exports are picked up while it runs). With the Suave clips:

- lead + follow share one root frame (both roots at one origin; the trainer picks the origin so Ram's
  dance frame 0 pelvis stays where he stood), then push + stumble from the release frame;
- the ring plays `suave_circle` with +2 beats per member, at 4x until the manifest marks the circle as one
  sway per beat (or pick a speed);
- the intro clips (manifest role `intro`: `suave_intro_<pairing>` for Ram, `suave_grabbed_<pairing>` for the
  partner) are used when both exist; otherwise the intro is built from a hands-up pose, a procedural
  reach and pull, and the dance's first frame blended in at the end;
- the dynamite goes into the front of the partner's belt, Ram's right hand placed there by IK over whatever clip
  plays. (The game itself places the bundle on the chest at Spine2; see `../README.md`.)

Without the Suave clips the trainer uses the game's own clips from the TODViewer exports plus procedural
motion: a merengue step (walk cycle at low weight, two beats per cycle), hip roll, arm IK for the hold,
spins under the joined hands, procedural claps, the game's shoot-dive clips for the escape.

Quirks carried over from the TOD Asset Viewer: CPU skinning (three.js GPU skinning breaks these glbs),
metalness 0 (TOD diffuse textures), skeleton overlay never refits the camera. The raw exports are mirror
images of the game (DEA lettering reads backwards), so the models are flipped on X, as the clip manifest says.

## Scripts

```
npm run verify     # headless Chrome: loads, plays every phase, pairings, cameras, debug, play as Ram -> proof/
npm run record     # MP4 previews (cuts camera, all three pairings) -> ..\previews\
npm run record -- --cams=gameplay,front --pairings=soldier --timing=design
node scripts/contact_report.mjs   # hand-contact summary per pairing
node scripts/review.mjs   # proof/shots_review.png: one frame per shot + 3 per slow-motion shot, all pairings
node scripts/smoke.mjs <dir> <t...> --cam=cuts   # quick stills
```

The scripts write their screenshots into a local `proof/` folder (not part of the repo). They use the installed
Google Chrome through `playwright-core` (Playwright's own Chromium download can be quarantined by antivirus) and start
a Vite server in-process if none is running on 6390. `record` finds ffmpeg on PATH, in `FFMPEG_PATH`, or through
`python -m pip install imageio-ffmpeg`. The videos carry a cut of the song and stay local (git-ignored).

## Files

- `src/main.js` UI, playback, clip manifest pickup, play mode; `src/camera.js` presets and the shot list;
  `src/audio.js` music (audio clock drives the timeline while music plays).
- `src/move/suave.js` the move as a pure function of time (timing profiles, scenario, root plans, bullets,
  evaluator); `src/move/clips.js` clip roles and the manifest parser.
- `src/engine/character.js` CPU skin, pose sampler, mirrored-parent-safe bone rotation, two-bone arm IK.
- `src/scene/world.js` the yard; `src/scene/fx.js` tracers, sparks, dust, dynamite, guns, explosion (the old shield
  code is unused).
- `public/env/` Poly Haven CC0 ground texture and props (1k copies; each folder has a `SOURCE.txt`).
