# Smart AI (`[smart_ai]`)

## What it does

Enemies of one type stop being clones: some rush in, some hang back, most hold the middle. A group no longer reacts in
the same frame. With three or more enemies on Ram, at most two close in while the rest hold and shoot, taking turns.
An ally dying nearby can frighten a hurt enemy into running.

## How it is implemented

1. **Engine facts.** The enemy brain (`character_sentry_ai`) already has per-enemy fields for fight distance
   (StopDistance), back-off (Retreat_Range), shoot-dodge distance and weights, the move mix of state 20 and its times,
   a reaction delay (Wait_until_Aggressive), a hold flag (DontHunt) and a morale test (CS_HealthLeft, CS_Change_for).
   In the shipped game they are the same for every enemy of a type. A loaded save restores enemies without running the
   AI init.
2. **One code site.** SM_INIT 0x5E7C28 retargets the call of the AI init 0x5D4190 in the reset handler. The wrapper runs
   the init, then gives the enemy a personality and registers it.
3. **Personality.** About 30 % assault (close to 7 m, more strafing and dodges), 25 % cautious (13.5 m, more crouching,
   backs off further), the rest riflemen; every time and weight jittered up to 25 %. Only non-zero row values are
   scaled: a type without shoot-dodge, back-off or a move never gets one. ShootDodge_Min_dist is never written.
4. **The tick** (input tick, every `tick_seconds`): reaction stagger once per enemy (not within 8 m); approach tokens per
   target (DontHunt: `max_advancing` advance, the turn passes after `token_seconds`; holders beyond 22 m or without a
   shot for 5 s close in anyway); fear from nearby ally deaths raising CS_HealthLeft / CS_Change_for, fading over time.
   Every write follows the game's member-write notify. A discovery pass finds enemies a save restored (60 m, once a
   second).
5. **Left alone:** buddies, `skip_factions` (6 civilians, 10 DEA, 13 wrestlers), special / escort types, enemies at or
   above `boss_health` (1000), authored hold flags, and anything another script changes afterwards. A re-found enemy
   keeps its first personality (a table of 64 written personalities survives forgetting).
6. **Saves.** On pause the tick writes DontHunt and the morale values back to the game's own, so a menu save holds
   vanilla values for those. Personality values made while it runs stay in such a save.

## Ini keys (`[smart_ai]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature |
| `personality` / `assault_share` / `cautious_share` / `jitter` | 1 / 0.30 / 0.25 / 0.25 | roles at spawn |
| `shootdodge_bonus` | 4 | percent over the row's own shoot-dodge weights |
| `reaction_stagger` / `reaction_min` / `reaction_max` | 1 / 0.2 / 1.0 | first-reaction delay (s) |
| `squad` / `max_advancing` / `min_squad` | 1 / 2 / 3 | approach tokens |
| `token_seconds` / `token_cooldown` / `hold_max_distance` / `hold_stuck_seconds` | 8 / 4 / 22 / 5 | token rules |
| `morale` / `morale_radius` / `fear_per_death` / `fear_last_man` / `fear_decay_per_second` | 1 / 15 / 0.3 / 0.3 / 0.04 | morale |
| `fear_health_bonus` / `fear_flee_chance` | 80 / 25 | how fear raises the game's flee test |
| `boss_health` / `skip_factions` | 1000 / 6, 10, 13 | enemies left alone |
| `tick_seconds` / `discover_seconds` / `discover_radius` | 0.1 / 1 / 60 | tick and save discovery |
| `log_spawns` / `log_states` | 1 / 0 | logging |

The old keys `retreat_range` / `retreat_chance` are ignored.

## Testing and limits

- Unit tests `smart_ai/code/test_smart.cpp`, static checks `check_smart.py` (every member and reader against the exe),
  host tests 9 and 10.
- Played in a real game over two days. A crash in the post-game Harbour during that play turned out to be an engine
  null-owner bug, now fixed ([crash_fixes.md](crash_fixes.md)). The rules were tightened at the same time: only
  existing values are scaled, DEA and special types are skipped, and an enemy never gets a second personality. Three
  test-copy runs on that save showed the tightened rules in the log ("dodge 0.0->0.0" for melee rows, DEA not managed,
  re-finds with 0 writes).
- Not built yet: group alert, flanking, searching instead of giving up (each needs an in-game probe first).
- The in-game checklist is `smart_ai/TEST_IN_GAME.md`.

## Files

`smart_ai/code/smart_game.cpp`, `smart_logic.cpp/.h`, `test_smart.cpp`, `smart_ai/code/tools/check_smart.py`,
`smart_ai/README.md`, `smart_ai/TEST_IN_GAME.md`, `dll/HOOKS.md` "Smart AI".
