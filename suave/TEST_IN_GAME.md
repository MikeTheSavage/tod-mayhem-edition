# Testing Suave in game

Use a separate COPY of the game folder, not your main install. Every step writes to `TODGameplay.log` next to
TOD.exe; the lines quoted here are what the DLL logs. Mark each item PASS / FAIL and keep the log lines that go with
a FAIL.

## Setup

1. `dll\build.bat` -> ALL OK. Build the music segments from your own copy of the song
   (`python suave\audio\make_segments.py --mp3 <file>`, or let the installer do it).
2. Data: `python data\build_gameplay_mod.py --game <copy> --set suave=on_start2 --set suave_anims=on --out work\t\todgameplay_data.zip`
   -> PASS; install it on the copy with `install_gameplay_mod.py install`.
3. Copy `dist\TODGameplay.dll`, `dist\dsound.dll`, `dist\TODGameplay.ini` next to the copy's TOD.exe; in the ini set
   `[suave] enabled = 1`, `cheat_key = F7`.
4. Start the copy. Log: `suave: installed (slot 11 = Suave ...)` and `N site(s) patched`. FAIL if
   `reference ... differs` or `sites differ`.

## Slot-10 variant

With `[suave] slot = 10` and data `suave = on_slot10`, read every slot-11 step below as slot 10: the Pinata slot shows SUAVE (Pinata icon), F7 gives Pinata charges, LCONTROL on that slot near an enemy runs Suave (log `approved on slot 10 ... the slot's own effect skipped`); no Pinata is thrown. Away from enemies the charge stays. El Mariachi, Tequila and the other moves are vanilla.

## A. Data and HUD (no fight needed)

| # | Do | Expect |
|---|---|---|
| A1 | New game (on_start2), select slot 11 with Q / E | the selector shows **SUAVE** (+ the second line); the icon is the Time Stop clock icon (stand-in); 2 charges |
| A2 | An old save, press F7 twice, then a third time | log `cheat key: +1 slot-11 charge`; the HUD count goes 1, 2, stays 2 |
| A3 | Use slot 11 with no enemy within 5 m | log `no enemy within 5.0 m: charge kept`; count unchanged; nothing happens |
| A4 | Use it in a dive / on a ladder / while reloading | log `not now (the game's move eligibility says no)`; charge kept |

## B. The move (a fight with 3-8 soldiers close, e.g. The Ambush)

| # | Do / watch | Expect | Unknown it settles |
|---|---|---|---|
| B1 | Use slot 11 next to a soldier | log `approved`, `START partner model '...soldier...' -> pair soldier`, the charge drops by 1 | gate + charge path |
| B2 | First 0.6 s | world almost frozen, camera pushes in on Ram's face; Ram turns toward the partner and shifts 0.36 m so his intro starts where he stood; the partner snaps onto his intro start (about 1.1 m from Ram, turned away; hidden by the freeze) | freeze, camera node, yaw writes (log: no `yaw probe` warning) |
| B3 | Ram's armor | outside enemies' bullets do not hurt Ram for the whole move (health bar), no shield visual | god mode |
| B4 | Music | the soldier segment starts at the freeze end with a short fade-in; log `music segment 1`; plays ~17 s, then El Mariachi still plays its own music when used later | SoundSlot retarget + restore |
| B5 | Intro (3 s) | Ram plays the intro (hands up, step, reach, pull), the partner the grabbed clip, faces: Ram slicksmile, partner surprise -> disbelief -> joy; camera: push-in, over Ram's shoulder, two-shot | clips on Ram / NPC, faces on NPC models |
| B6 | Ring | nearby soldiers stop shooting, holster, walk to a 3 m ring, face the centre, sway once per beat with neighbours on opposite sides (1-beat offset); late ones appear on their slot | no_ai_control + auto_move walking, Victim park, setplaypos |
| B7 | Dance | the pair dances in sync (no drift over the dance), the partner stays on Ram's spot; camera orbits high, then cuts every 1.93 s on the bar lines | partner lock, speed 1.0 |
| B8 | Enemies outside 15 m | keep fighting (their bullets do nothing to Ram) | |
| B9a | Show | just before the push Ram holds the lit dynamite (fuse sparks) in his left hand (log `dynamite -> Ram's L Hand`, then `dynamite shown by Ram: in Ram's hand, flags ... (shown), ... (armed, fuse lit) ... L Hand at (...): 0.0x m (attached)`) | attachnode with the game's grenade hold |
| B9 | Plant | at the push contact a bundle of 3 red sticks with lit fuses sits on the FRONT of the partner's chest, clearly visible in the slow-motion chest close-up and in the panic cut (log `dynamite strapped: STRAPPED to the partner's chest (3 sticks), flags 902 (shown) ... (armed, fuse lit)`) | the DLL holds the bundle every frame; flag 4 cleared (it hid every pooled shell) |
| B10 | Push + dive | Ram pushes the partner away, TURNS away from him, then dives FORWARD toward the low front camera (5 m ahead, 1.2 m up) with the partner and his bundle behind him (log `dive: TURNED ... -> W (forward) + SPACE`, `takeoff: the partner's root ... 1.50 m behind him`) | the game's yaw path, the partner's root off Ram's spot |
| B10a | No falling | Ram stays on the floor through takeoff, flight and landing (no `Ram sank` line; at most `Ram's root ... under the floor: lifted onto it` before the dive) | partner clearance, floor lift, ground snap |
| B10b | Stumble end | the partner, about 2 m away, looks down at the belt, hands up, trembling (`suave_lookdown`), terrified face | lookdown clip timing |
| B10c | Guns | the partner's and the ring members' guns fall to the ground and stop firing when the move takes them (log `gun ... stops firing and drops (the gun's own drop)`); no gun floats in the air | default_weapon stop_firing + command_drop |
| B10d | Tight space | near a wall or in a room: the log line `space around Ram ... -> TIGHT` gives a smaller ring (e.g. 1.6 m, at most 4 members); cameras are pulled in or mirrored to the open side (`... mirrored to the open side`), the dive goes to the open side (`dive: free space away from the partner ...`) | line tests (Navigator::IsPointOccluded) |
| B10e | Panic cut | right after the takeoff the camera cuts to the partner looking down at the sparking bundle on his chest, terrified, for about 1.4 s (log `camera partner looks down` -> `camera front dive` 1.4..2.2 s apart; `Ram's animation speed 0.95 -> 0.30` while he is in the air, back at the cut back); then Ram mid-air toward the camera and the blast behind him | panic_min / panic_ram_speed |
| B11 | In the air | cut to the partner looking down, terrified; cut back to Ram mid-dive; **the explosion behind him** while airborne (log `BOOM - dynamite explode`, `kill pass`) | MoveState 0x24 = airborne, shell explode with radius 7, owner Ram |
| B12 | Blast | the partner and every ring member die and are thrown; kills count in the combo / mission kill counter | damage, impulses, kill credit |
| B13 | Landing | Ram lands; time back to normal; the camera blends back to the gameplay camera (mouse look works again) | camera release |
| B14 | After | log `done`; armor gone (a bullet hurts again); another power-up can be used; El Mariachi's music is its own again | clean end |
| B15 | `auto_dive = 0` | at the dive window the camera returns to gameplay and the keys work; dive yourself: the explosion happens in the air | player mode |
| B16 | civil / DEA partner | pair civil / dea clips; music segments 2 / 3 | pair choice by model path |

## C. Aborts (each must leave a clean game: enemies fight again, Ram moves, camera normal, time normal)

| # | Do | Expect (log `ABORTED (...)`) |
|---|---|---|
| C1 | With `lock_input = 0`, kill the partner during the dance (shoot him) | `partner died before the plant`; ring members get their AI back and their guns |
| C2 | A scripted cut-scene starting during the move | `cut-scene` |
| C3 | Pause (Escape) during the dance, resume | the move continues where it was (no abort) |
| C4 | TAB rewind during the escape window (with a rewind charge) | `rewind started` (hard: no NPC messages) |
| C5 | Quick restart (F10) / mission end during the move | `level change` or `player not in control`; no crash |
| C6 | Enter a car during the escape window (`auto_dive = 0`) | `player in a vehicle` |

## D. Shared memory (bridge / Unreal)

Read `Local\TODGameplay.Shared.v1`: version 2, size 128; during the move `suave_phase` runs 1..7, `suave_clock` rises
(game s), `suave_flags` has ARMOR (1) from the start and BOOM (8) after the explosion, `suave_shot` follows the camera.

## Reading the verbose log (`log_verbose = 1`)

Every 0.5 s: `[phase c t] tm playmode input-tick combo-music stereo-track diving ...` then one line per character:
`ram` / `partner` / `memberN`: `ai` (sentry AI state: 2 = taken, 28 = dead), `hp` damage/health, `move` (active MoveCtrl controller: victim = parked), `ms` (MoveState; Ram's dive state is expected to be 36), `weapon` (in hand / none), `pos`. `AI state N -> no_ai_control again` lines show the AI taking an enemy back.

## E. Known risks to watch

- **Faces during the dance**: the Suave clips carry the game's idle face values on the 8 face bones (as vanilla body
  clips do). If the expressions (slicksmile, joy, surprise, pain ...) do not show while the Suave clips play, the motion
  layer wins on the face: the fix is to drop the face channels from the Suave clips (the clip codec in `anim/lib` can; the expression
  clips themselves carry face channels only).

- Gamepad input is not locked during the dance.
- If the yaw probe fails (log warning), Ram / the partner / the ring may face wrong: set `rotate = 0` and note it in the bug report.
- If walking NPCs do not move (B6), they are teleported onto their slot at the gather deadline anyway.
- If no shell is free (log `no free shell in the pool`): no dynamite prop and no fire blast, the kill pass still kills.
