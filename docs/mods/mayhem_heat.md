# Mayhem heat (`[mayhem_heat]`)

## What it does

The chaos Ram causes builds heat in five levels: kills (cops and civilians count more), his explosions, broken props,
barrels, cars, loco moves and stylish combo kills. The game's own radar shows it: a warm orange tint while it builds,
red at the top with the rim pulsing, and 5 chili peppers beside the disc: a lit one burns (a tiny flicker on the first,
up to a roaring flame on the fifth), an unlit one is a dark grey pepper. Heat cools once Ram has been quiet for a
while. More heat sends more enemies in waves, rooftops first at level 3 and up. Heat also raises the score: no heat
and level 1 score as usual, level 2 x1.25, 3 x1.5, 4 x1.75 and level 5 doubles every award, on top of the game's own
combo multiplier. (An optional small gold "x1.5" label can show it: `score_label = 1`, off by default.)

## How it is implemented

1. **Engine facts.**
   - Kills: sentry dying state 0x5D6B70 counts a death only when KillOwner (+178h) is set, into mission data +28h
     (hostile) or +2Ch (neutral). It does not check who the killer is; the DLL compares KillOwner with Ram's root.
   - Combo: Combo_Ctrl (cache +268h) data +3Ch CurrentActionPoints rises with style points.
   - Loco moves: Ram's PowerupCtrl (root data +8Ch) +20h RunningLocoMove goes 0 -> slot when a move starts.
   - Explosions: shells set node flag 4 at the blast (shell pool at cache +550h); destroyables die in
     `command_destroy_object` 0x671410; cars explode in vehicle_ai state 0x809910.
   - Radar: the `Radar_BG` sprite (HUDAuxCtrl.fragment) has corner colours +70h..+7Ch and a rebuild flag (+14h bit 3).
     The game already has a dark red radar texture but never switches to it.
   - Factions: 0 player, 6 civilian, 7 animal, 9 enemy, 10 friend, 11 neutral, 14 police, 18 Mexican buddy.
2. **Event sites** (the stubs only queue; the input tick applies):
   - HT_KILL_E 0x5D718D and HT_KILL_N 0x5D7225: the touch calls right after a hostile / neutral death is counted.
   - HT_DESTROY 0x6715C4 (6-byte JMP): the only way into the destroy tail.
   - HT_CAR 0x809DE9: the camera-shake message right after a car's big explosion.
3. **Polls** in the input tick: combo points, RunningLocoMove, the shell pool's flag 4 with owner Ram, and once a
   second the hostiles within 30 m (the Tornado's quadtree query).
4. **Heat points.** Enemy kill 10, cop 24, civilian 18, Ram's grenade / rocket / dynamite / molotov 6, broken prop 3,
   barrel 9, car 16, loco move 14, combo 0.04 per point; destruction by others x0.5. Levels at 30 / 90 / 170 / 270 /
   390 (cap 460).
5. **Cooling.** After `calm_seconds` (7) without chaos of his, one level per `decay_seconds` (10), x0.3 while live
   hostiles are within 30 m. A mission change, load or retry clears the heat.
6. **Radar.** The radar stage (`gp_hudstage.cpp`) is a sprite client of the shared overlay layer for `Radar_BG`: it
   writes the tint into the corner colours right before the sprite renders (no notify: never saved) and draws the
   chilis after the radar's list ran. The art is a resource inside the DLL (`gp_heat.rc`): `mayhem_heat/art/heat_icons.png`
   (1024 x 512), built by `make_heat_art.py` from art made for this mod (`mayhem_heat/art/chili/`: a no-flame chili
   and five flame sizes, 1254 px squares with the pepper at the same place in each). Slot k (the order they light, top
   down) always shows flame k; an unlit slot shows the no-flame chili greyscale x0.42, alpha x0.75; each icon has a
   soft drop shadow (4, 5 px, blur 3, alpha 0.55 at 64 px). The full square is drawn, 64 px at 1080p (`icon_scale`
   1.0), with the pepper's centre on the slot centre; the top slot is drawn first so each lower chili's flame lies
   over the chili above. A filling slot lights from the bottom up with a soft 10 % cross-fade band. With the minimap
   on, the map ink follows the tint.
7. **Escalation.** `heat_drive()` feeds the reinforcement engine of [difficulty.md](difficulty.md) every 0.5 s, with all
   its mission-safety rules, scaled by difficulty (easy 0.4, normal 0.7, hard 0.85, brutal 1, psycho 1.25) x
   `escalation_scale`:

   | Level | Extra per death | Wave at level-up | Again every | Comebacks per placement | Rooftops first |
   |---|---|---|---|---|---|
   | 1 | +0.25 | - | - | 1 | no |
   | 2 | +0.5 | 2 | 45 s | 1 | no |
   | 3 | +0.75 | 3 | 35 s | 2 | yes (>= 3 m above Ram) |
   | 4 | +1.0 | 4 | 28 s | 2 | yes |
   | 5 | +1.25 | 5 | 22 s | 3 | yes |

   A wave re-arms one placement per 0.5 s pass and is called off when cooling starts. With `[difficulty]` off the
   engine runs for heat alone (no menu, tuning or cap).

8. **Score multiplier.** The game adds score in one place: PlayerStatus
   `command_add_to_overall_score` (GOG command 0x25F), handler 0x64F560, which copies its argument and calls the body
   cdecl(thread, int* amount) 0x64CEF0 (`[0xA3B75C]` = the PlayerStatus node, data +24h OverallScore += *amount, then
   the reward thresholds). The award comes from the script at the end of a combo (CurrentActionPoints x
   ComboMultiplier, Combo_Ctrl data +3Ch / +34h), so the combo multiplier is already in it. Site HT_SCORE 0x64F575
   retargets that call to ScoreWrap: `*amount = floor(amount x m + 0.5)` (capped at 2e9, only positive awards), then
   the body. m = 1 for no heat and level 1, then linear to `score_top` at `max_level`: 1.25 / 1.5 / 1.75 / 2.0 with the
   defaults (`ht::score_multiplier`). REF HT_SCORE_ADD 0x64CEF9 checks the body. Log:
   `heat: score award +38 at heat level 5 (x2.00) -> +76; OverallScore 1200 -> 1276 (combo now 0 x 0)`.
   The optional label (`score_label = 1`, off by default) is drawn right of the middle chili, from glyphs in the atlas
   (Arial Black italic, cream to gold, `mayhem_heat/art/heat_glyphs.h`). No menu row (a new row breaks save loading).

Enemy vehicles with gunmen at heat 3+ needed new records, so they live in the save-breaking option Bigger Fights
([bigger_fights.md](bigger_fights.md)). They read the heat level from `heat_drive()` and leave this mod's waves alone.

## Ini keys (`[mayhem_heat]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 1 in the sample, 0 in code | the feature |
| `decay_seconds` / `calm_seconds` | 10 / 7 | cooling per level / quiet time before cooling |
| `max_level` | 5 | levels and chilis |
| `tint` / `icons` / `pulse` / `icon_scale` | 1.0 / 1 / 1 / 1.0 | radar tint, chilis, rim pulse, chili size (1.0 = 64 px at 1080p) |
| `escalation` / `escalation_scale` | 1 / 1.0 | more enemies with more heat |
| `score_multiplier` / `score_top` / `score_label` | 1 / 2.0 / 0 | heat scales the score award (x`score_top` at the top level, 1..4); the "x1.5" label (off) |
| `heat_gain` / `others_weight` | 1.0 / 0.5 | all heat x this; destruction by others x this |
| `near_cooling` / `near_radius` | 0.3 / 30 | slower cooling with hostiles near |
| `rooftop_height` | 3.0 | level 3+: placements this far above Ram come back first |
| `log` / `log_verbose` / `perf_log_seconds` | 1 / 0 / 10 | logging |

Test-only (in `[difficulty]`): `force_level`, `debug_no_damage`, `debug_kill_seconds`,
`debug_kill_as_ram`, `debug_kill_stop`; in `[mayhem_heat]`: `debug_heat_level` (holds the heat at that level),
`debug_score_seconds` / `debug_score_amount` / `debug_score_count` (sends the game's own `add_to_overall_score` to
PlayerStatusCtrl every N s, a fixed award to compare the multiplier with), `debug_heat_step_seconds` (with
`debug_heat_level = 0`: walks the held heat 0 -> 5, one level every N s, and logs screenshot marks).

## Testing and limits

- Unit tests `tests/dll/test_heat.cpp`; host test 21.
- Checked in game on a test copy of the game with The Ambush save on BRUTAL, with debug kills counted as Ram's:
  - on the Farm (no enemies): no false heat in 8 minutes;
  - level 5 reached at 49 s; waves re-armed 12 placements and all 12 arrived; cooling 5 -> 0 exactly every 10 s;
  - the radar tint and chilis at every level, up to the red disc at level 5;
  - in the combined test: heat 0 -> 5 with 15 reinforcements.
- Score multiplier: the same award `add_to_overall_score(38)` was sent to PlayerStatusCtrl through the game's handler.
  Heat 0: +38 -> +38 every time (OverallScore 11745915 -> 11745953). Heat 5: +38 -> +76 (OverallScore 11745915 ->
  11745991). Host test 21: 38 -> 38 / 57 / 76 at levels 1 / 3 / 5. The optional x label is drawn (log) but has not
  been seen in a screenshot.
- Cost: HUD 0.008 to 0.011 ms per frame; the score wrap runs once per award (no per-frame cost).
- Not verified: kills by Ram's own aimed fire (every test kill was a debug kill the game counted as Ram's), the rooftop
  preference (no Ambush placement is 3 m above Ram), the car-owner field (inferred).

## Files

`dll/src/gp_heat.cpp`, `gp_heat_logic.cpp/.h`, `gp_heat.rc`, `dll/src/gp_hudstage.cpp`, `dll/src/gp_difficulty.cpp`
(the heat drive), `tests/dll/test_heat.cpp`, `mayhem_heat/art/` (`make_heat_art.py`),
`dll/HOOKS.md` "Shared HUD stages" and "Mayhem heat".
