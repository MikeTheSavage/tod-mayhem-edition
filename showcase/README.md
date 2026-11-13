# TOD Gameplay+ showcase video

`TOD_GameplayPlus_showcase.mp4`: 65 s, 1920x1080, 30 fps, H.264 (yuv420p, faststart), silent. `showcase_sheet.jpg`: 12
frames of it. Everything was recorded on a separate test copy of the game with the merged build of
master 49e2109 (`build\TODGameplay.dll`, `dsound.dll`, `TODPhysics.dll`, data zip `work\merge_install`); the main
install and its saves were only read (hashes).

| Time | Segment | Source run |
|---|---|---|
| 0:00.0 | Title card | Pillow |
| 0:03.5 | SETTINGS > DIFFICULTY LEVEL stepped EASY > NORMAL > HARD > BRUTAL > PSYCHO with the d-pad, then the VIBRATION row | `m2_menu` 5.3-17.9 s |
| 0:16.1 | Button prompts: keyboard, Xbox, PlayStation (same SETTINGS page) | `m2_menu` 30.0 / 21.95 / 25.75 s |
| 0:24.0 | BRUTAL fight, The Ambush | `f1_probe_brutal` 15.0-20.9 and 21.6-29.6 s |
| 0:37.9 | Explosion, vanilla | `x1_vanilla` 5.35-12.9 s |
| 0:45.4 | Same explosion, Box3D physics | `x2_physics` 5.4-14.9 s |
| 0:54.9 | Both side by side (from 1 s before the blast) | `x1_vanilla` + `x2_physics` |
| 1:01.4 | End card | Pillow |

## How it was made

The clips were recorded in game with an in-game test harness on a copy of the game (not included). The input was
scripted: a key script plus the DLL's scripted pad (`[debug] pad_script`, the same fake DirectInput device a real pad
feeds), with the window moved off the desktop so the menu cursor stays out of the picture. The window was grabbed with
PrintWindow at about 30 grabs a second into near-lossless clips that keep real time. The fight recording started about
2.5 min after the load, with enemies already around Ram; a new recording of the fight will not be frame-identical,
since the AI differs run to run.

`tools/make_showcase.py` cuts the clips into the video: title and end cards and captions drawn with Pillow, segments
cut and captioned with ffmpeg, joined, plus the contact sheet. Run it with `python -I showcase\tools\make_showcase.py`
(it reads the recorded runs from `work\showcase\runs` and writes its work files to `work\showcase\edit`; check the
cut times in the script after a new recording).

## Where the caption numbers come from

| Caption | Log |
|---|---|
| "Live-enemy cap raised from the game's 6 to 9" | `f1_probe_brutal\TODGameplay.log` 16:42:47.608 `live-enemy cap (CharacterResourceCtrl MaxAllocated) 6 -> 9` |
| "this fight reached 9 alive at once" | same log, 16:44:19 .. 16:45:20 `BRUTAL fight: 9 live enemies near Ram (max 9)` (5-7 alive during the shown 14 s) |
| "Test run of The Ambush: 7 enemies + 3 reinforcements = 10" | the log of a merged-build test run of The Ambush (kill timer 6 s, cap off): `enemies seen 7 ... reinforcements re-armed 3 / arrived 3 (= 10 enemies in this fight)` |
| "crates thrown 14-25 m, the chair 34 m" | `x2_physics\TODGameplay.log` 17:03:26-27: crates moved 13.55 / 18.42 / 24.71 m, ADD_MetalChair 34.31 m |
| "the crates do not move" (vanilla) | `x1_vanilla` footage (`debug_vanilla = 1`: same rig and keys, no simulation) |

## Known limits of the footage

* The fight footage has no debug kills (no `debug_kill_seconds`), but `debug_no_damage = 1` was on and the input is a
  blind pad script (look, RT bursts, shoot-dodges): Ram mostly fights at walls and doorways; enemies are seen firing and
  throwing a grenade, nobody is killed on camera. Probes with the kill timer (`f2_probe_brutal`) or target lock + RT
  (`f3_lock_brutal`, 0 deaths) did not give better footage: the kills happen 25-57 m away, off screen. No NORMAL clip:
  the DLL does not count enemies on NORMAL and a second dark factory clip would not show the difference; the caption
  gives the game's own cap (6) instead.
* The prompts are switched by the scripted pad (`kind xbox|playstation`) and a key press; no physical pad was used.
* The props in the physics clips were put in front of Ram by the test rig (`debug_gather`), before the recording starts.
