# Gang wars (`[gang_wars]`)

## What it does

Rival gangs fight each other when they meet. Ram can start a fight, step back and watch. While two gangs fight they
ignore Ram until he shoots one of them; then that one and his friends turn on him. Kills by a rival count for the
mission like Ram's own.

## How it is implemented

1. **Engine facts** (table in `gang_wars/README.md`):
   - Targeting is faction based, not player-only: the vision scan (0x7B29C0 -> 0x7B1B80) asks the TargetDatabase for
     characters whose faction is hostile to its own, then picks by priority (1 for everyone) and distance.
   - The relation table FactionRelations is 38 x 38 (19 base factions + 19 "modified" copies = faction + 19): 1 friend,
     2 neutral, 4 hostile, 8 self (get_relation 0x7B76A0).
   - Every hostile mission enemy is faction 9; gangs differ only by model. Factions 3, 4, 5, 7, 12, 15, 16, 17, 18 are
     used by no entity.
   - NPC damage always applies. A neutral hit triggers the game's provoke rule (victim and known friends move to the
     modified copy, hostile to the shooter). Any hit makes the shooter the lock target.
   - A death counts as an enemy kill when Ram's row holds 4 (hostile) for the victim's faction and the hit had an
     owner (0x5D6F3B), whoever the owner was.
2. **No code site.** It runs every 0.25 s in the input tick and only writes factions and relation cells.
3. **Gangs.** Live enemies within 80 m are sorted by model: cartel (0, 1), virgilio (2, 3), elvez (22), gangster (27),
   army (15), mexpolice (24), border (5), dea (9), outlaw (30). Eligible: faction 9 at its placement, a gang model, not
   a buddy, escort, civilian or special sub type, below 1000 hp, not a VIP placement, not in state 2 / 3 / 12 / 31
   (scripted, mounted gun, priority target, buddy).
4. **War.** A member with a rival gang member (a `factions` pair) within `range` gets a spare faction of its gang
   (`spare_factions`, one per gang at war). That faction's row and column copy faction 9's, except: hostile to the
   rival gangs, friendly to the other gangs and faction 9, and with `provoke = 1` neutral toward Ram while Ram's row
   stays hostile (so his kills, loco moves and auto-aim still work). The TargetDatabase's caches are marked stale, so
   the member's target switches to the nearest rival within one scan.
5. **Provoke.** When Ram shoots a member at war, the game's own neutral-hit rule moves it and its friends to the
   modified copy (spare + 19), hostile to Ram and to the rivals.
6. **Calm.** No rival within `range + hysteresis` for `calm_seconds`, or at war for `idle_seconds` without a rival target
   (a wall between them): faction 9 comes back, no new war for `retry_seconds`. Wake: an idling member gets the game's
   own `alarm` when its war starts. Spark (`spark = 1`): a fight starts only when Ram shoots a member with a rival near.
7. **Saves.** Each faction write goes through the member-write notify, as the game's provoke rule does; matrix cells are
   memory only. On pause every moved member gets its own faction back, so a menu save holds what the game made. A root
   that still holds a spare faction when first seen is repaired. A member whose faction the game changes is released.

## Ini keys (`[gang_wars]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature |
| `range` / `hysteresis` | 25 / 8 | m to start a fight / extra m to keep it going |
| `calm_seconds` / `idle_seconds` / `retry_seconds` | 8 / 6 / 12 | calm rules |
| `wake` | 1 | alarm idle members when their war starts |
| `provoke` | 1 | gangs at war ignore Ram until he shoots one |
| `spark` / `spark_radius` | 0 / 40 | fights start only from Ram's shot |
| `factions` | 9 pairs | which gangs hate each other |
| `spare_factions` | 4, 16, 17, 3 | unused factions lent to gangs at war |
| `group_<name>` | none | add or replace a gang by model numbers |
| `boss_health` / `tick_seconds` / `scan_radius` | 1000 / 0.25 / 80 | limits |
| `log_switches` / `log_targets` / `log_found` / `status_seconds` / `status_members` | 1 / 1 / 1 / 0 / 0 | logging |

Test-only: `debug_face_war`, `debug_split`.

## Testing and limits

- Unit tests `gang_wars/code/test_gw.cpp`; host test 19. A faction census of all 2,830 placeholders:
  `gang_wars/tools/faction_census.py`.
- Checked in game on a test copy of the game (NEW GAME plus override zips that jump to a mission):
  - MM075 Hacienda with the demonstration aid `debug_split = cartel`: 296 target switches, 9 deaths by rival fire,
    EnemiesKilled 3 -> 9 without Ram firing.
  - MM075 without the aid: a natural Elvez guard killed by the cartel; the cartel then hunted Ram.
  - MM120: DEA taskforce and outlaws targeted each other, then lost sight of each other (this is why the idle rule
    exists).
- Cost 0.1 to 0.9 ms per 0.25 s tick in a fight.
- The 6 (max 9) pooled shells go to the placements nearest Ram, so natural two-gang fights are short.
- Not verified: Ram's provoke shot and `spark` in game (the scripted tests had no mouse input), a full mission completed with
  rival kills, the score message when the killer is not Ram, autosaves taken mid-war.

## Files

`gang_wars/code/gw_game.cpp`, `gw_logic.cpp/.h`, `test_gw.cpp`, `gang_wars/tools/`, `gang_wars/README.md`,
`dll/HOOKS.md` "Gang wars".
