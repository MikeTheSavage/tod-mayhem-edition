# Difficulty: HARD, BRUTAL, PSYCHO in the menu (`[difficulty]`, data option `difficulty_menu`)

## What it does

SETTINGS > DIFFICULTY LEVEL offers EASY / NORMAL / HARD / BRUTAL / PSYCHO, picked with the arrows and saved with the
game's own "save settings" question. EASY and NORMAL are vanilla. HARD is vanilla Hard plus tougher enemies. BRUTAL
hits harder still and brings about 50 % more enemies. PSYCHO is BRUTAL plus enemy loco moves
([psycho_loco_moves.md](psycho_loco_moves.md)). The experimental option Bigger Fights adds more on top of BRUTAL and PSYCHO
([bigger_fights.md](bigger_fights.md)).

## How it is implemented

1. **Engine facts.**
   - The row stores Setting_difficulty in Slot11 (cache data +398h, member 1; read at 0x5B7379). The game reads an
     out-of-range value as Normal (0x5B73F0), but its menu would index past the value list. So Slot11 only ever gets
     0 / 1 / 2.
   - The row's entry has unused fields OptionValueType4 / 5 (+38h / +40h) that the code never reads, and the Menu text
     table has two texts unused on PC (#77 FILL_IN, #101 DEBUG COMPLETE).
   - The tuned values live in overdose.main's dictionaries (block 1, never reloaded): PlayerStatusCtrl DamageEasy /
     Normal / Hard (+90h / +94h / +98h), PlayerDamageMod (+9Ch), BulletDrop, WeaponDict SpreadEnemy_m and burst pauses,
     CharacterDict / ModelStat wait times.
   - Enemies are character placeholders; CharacterResourceCtrl (MaxAllocated 6 or 8, written at 0x763694 / 0x7636A6)
     hands them one of 9 pooled shells. No placement is ever spawned twice; there are no spawners or waves.
   - After a death the placeholder has Status 0, Resource 0, spatial mask without 80h (cleared by the kill at
     0x5A8ABB) and node flag 1 (hidden), so the collect never takes it again.
2. **The menu values.** The data option `difficulty_menu = on` replaces texts #77 / #101 with BRUTAL / PSYCHO (same table
   length, so a language not rebuilt shows the old text instead of crashing) and writes the two ordinals into
   OptionValueType4 / 5. Through the menu helper (MN_GET_INIT 0x7FF75C, MN_GET_RESET 0x7FE7C8, MN_SET 0x6BA420, option
   type 3) the DLL appends them as values 4 and 5. On set, index 3 or 4 becomes Slot11 HARD (2) plus an extra level.
3. **The extra level** goes to `Documents\Total Overdose\savegames\TODGameplay_difficulty.txt` next to Slot11 (written
   when the menu choice changes; it also accepts the older `brutal = 1` form). Without the DLL the game reads Hard.
4. **Run-time tuning.** The DLL captures the 142 tuned values after checking known vanilla values (e.g. DamageHard 1.4,
   PlayerDamageMod 1.0). It applies the `hard` or `brutal` factors without the member-write notify (never saved) and
   re-checks every ~4 s. If the data zip still carries a static difficulty level, the tuning is refused and logged.
   BRUTAL: DamageHard x2, PlayerDamageMod x0.6, enemy spread x0.25, burst pauses x0.5, reaction x0.5, dropped ammo x0.5.
5. **Reinforcements** (BRUTAL / PSYCHO, every 0.5 s in the input tick). Each counted enemy death adds `reinforce_share`
   (0.5). For each whole unit, one dead placement is re-armed once, `reinforce_delay` s after its death, with Ram
   18-45 m away, using the game's own setters: Status / Locked 0, spatial mask |= 80h (0x48F3A0, notify slot 6),
   SetFlags(flags & FFEh). The resource collect gives it a pooled character within 0.5 s, at its placement, armed as
   authored. Rules:
   - keep `reinforce_keep_slots` (2) pool slots free for the mission's own enemies;
   - never a placement whose mission death condition is registered and not yet met (CallbackTrigger_List +24h,
     condition IsMet +44h);
   - never bosses (1000 hp), buddies, civilians, or skip factions 6, 10, 13, 14;
   - forget everything on a mission change, a clock jump back (load / retry) or a submap change.
6. **Live cap.** `max_live_enemies` (up to 9, BRUTAL / PSYCHO only) writes MaxAllocated back every 0.5 s after the game
   resets it, without notify.
7. **Mayhem heat** drives the same reinforcement engine with its own waves ([mayhem_heat.md](mayhem_heat.md)).

## Safety rules for reinforcements

- **No born-dead reinforcements.** A dead placement keeps its last enemy's DamageTaken in placeholder property 0x287,
  and the AI init 0x5D4190 copies it into the new enemy. Left alone, every reinforcement would arrive with
  DamageTaken >= Health (one hit kills it, and the ragdoll, Smart AI and PSYCHO take it for a corpse). `rearm()` clears
  0x287 with the game's SetProperty 0x86A6C0 (REF DF_SETPROP) before the spawn; on arrival a reinforcement still at a
  lethal DamageTaken is cleared with the member write. Log: `stored DamageTaken 140 -> 0`, `arrived ... DamageTaken 0`.
- **The live cap** never hands out the 9th shell (the game's own values are 6 and 8), and stays the game's in a mission
  with a buddy, escort or friend (`max_live_with_buddies = 1` raises it there too).
- **Provoked friends are never re-armed:** the modified copy of a skipped faction (29 = a DEA friend Ram hit) is
  skipped, and so are factions 0 / 18 (and their copies) and sub types 4 (escort) / 8.
- **Mission log** (`mission_log = 1`, log lines only): `mission log: mission N ENDED with result 0 (FAILED)`,
  Mission_Result changes, `RAM DIED`, and `buddy / escort / friend / VIP ... DIED ... kill owner ..., killer ...` for
  those within 80 m.

## Ini keys (`[difficulty]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature (needs `difficulty_menu` and the static `difficulty = normal` in the data zip) |
| `menu` | 1 | BRUTAL / PSYCHO as extra values of the row |
| `tuning` | 1 | the hard / brutal factors at run time |
| `reinforcements` | 1 | more enemies on BRUTAL / PSYCHO |
| `reinforce_share` | 0.5 | extra enemies per enemy death |
| `reinforce_min_distance` / `reinforce_max_distance` | 18 / 45 | m between Ram and a re-armed placement |
| `reinforce_delay` | 4 | s after the death |
| `reinforce_keep_slots` | 2 | pool slots left to the mission |
| `max_live_enemies` | 0 | raise the live cap up to 8 (0 = the game's; 9 is read as 8) |
| `max_live_with_buddies` | 0 | 1 = raise it in buddy / escort missions too |
| `mission_log` | 1 | log mission ends and results, Ram's deaths, buddy / escort / VIP deaths |
| `skip_factions` | 6, 10, 13, 14 | factions never brought back |
| `brutal_text` / `psycho_text` | 0 | text ordinals when the data option is missing |
| `log_verbose` | 0 | diagnostics |

Test-only: `force_level`, `debug_kill_seconds`, `debug_kill_as_ram`, `debug_kill_stop`, `debug_no_damage`,
`debug_focus`.

## Testing and limits

- Unit tests `tests/dll/test_difficulty.cpp`; host test 16; data test `test_menus` (controller + difficulty_menu).
- Checked in game on a test copy of the game: the menu values, save and
  reload, BRUTAL picked with both arrows. The Ambush on BRUTAL: 7 enemies plus 3 reinforcements re-armed and fought;
  with `max_live_enemies = 9`, 6 enemies alive near Ram at once.
- **Open:** it is not proven whether reinforcements ever delay a mission step (the test save had no registered death
  conditions); "kill N" counters may complete early on BRUTAL; more live enemies leave fewer shells for corpses, Suave
  and Smart AI. Key injection on SETTINGS is unreliable on the test copy (vanilla too).
- The installer writes `todgameplay_difficulty.txt = menu` into the game folder as its marker (separate from the
  Documents side file above).

## Files

`dll/src/gp_difficulty.cpp`, `gp_difficulty_logic.cpp/.h`, `dll/src/gp_menu.cpp`, `data/gameplay/menus.py`,
`tests/dll/test_difficulty.cpp`, `dll/HOOKS.md` "Menus" and "Difficulty".
