# Gang wars (`[gang_wars]` in TODGameplay.ini)

Rival gangs fight each other when they meet, so Ram can start a fight, step back and watch. Built into
`TODGameplay.dll`. Off by default: with `enabled = 0` (or no section) nothing is touched.

No code is patched. The feature uses the game's own faction system: it moves a gang member into a faction of its own
while a rival gang member is near, and writes that faction's relations into the game's own relation table. The game's
vision scan, target choice, aiming, firing, hit reactions, provoke rule and kill counting then do the fighting.

## What the game already has (GOG TOD.exe)

| Question | Answer | Where (GOG addresses) |
|---|---|---|
| Who can an enemy target? | Any character whose faction its own faction is **hostile** to, not only Ram. The vision scan (every 0.2-0.3 s) asks the TargetDatabase for `query_target_list(1, my faction, 4)` and tests one candidate per call; the best one wins: TargetPriority (1 for Ram and every AI), then distance. DEA agents and cartel already fight each other this way in vanilla (The Ambush). | `can_see_enemy_characters` 0x7B29C0 -> 0x7B1B80 (REF GW_TDB_QUERY), candidate test 0x7AFE90, the per-faction cached query 0x7B6940 |
| Faction / team ids | `FactionID` (raw property 680) on every character_root (data +18h, slot 2) and placeholder. 19 base factions (CentralTargetSystem.fragment, FactionTable): 0 Player, 1 Morales, 2 ExplosiveObject, 3 Unknown, 4 Virgilio, 5 MissionTarget, 6 Civilian, 7 Animal, 8 Taskgiver, 9 Enemy, 10 Friend, 11 Neutral, 12 JohnsonTarget, 13 Wrestler, 14 Police, 15 IdiotDriver, 16 Gang01, 17 Unused02, 18 MexicanBuddy, plus 19 "modified" copies (faction + 19, e.g. 29 = provoked Friend). | root layout (SM_RD_FACTION), base count [[0xA3B590]+18h]+258h (GW_BASECOUNT) |
| Relation table | `FactionRelations`, a list of int lists [my faction][other faction]: 1 friend, 2 neutral, 4 hostile, 8 self (38 x 38). Commands `get_relation` / `set_relation` / `get_modified_faction`; mission trigger action 92 (Trigger_Activate_Faction) writes cells (Police5 encounters make police hostile to Ram for 75 s). | get_relation 0x7B76A0 (REF GW_GETREL), set_relation 0x7B7700, action 92 worker 0x7E4D90 |
| Who uses which faction (all 2,830 placeholders of all 116 blocks, `tools/faction_census.py`) | **Every hostile enemy of every mission is faction 9**; the gangs differ only by model. Police 14 = free-roam cops in the Los Toros submaps (142); 10 = DEA friends / Marco / Angel; 6 civilians; 8 task givers; 11, 1, 0, 13, 29 a few specials. Factions 3, 4, 5, 7, 12, 15, 16, 17, 18 are used by **no** entity (`tools/faction_users.txt`). | |
| Shipped hostilities | Enemy 9 is hostile to everyone but civilians (incl. police 14 and DEA 10, both ways); Police 14 is hostile to 9; Morales 1 and Virgilio 4 hate each other but no enemy uses them. So cops vs gangsters already fight in vanilla when they meet; two gangs of one mission never can (same faction 9). | the author's reverse-engineering notes (faction inventory, not included) |
| NPC-vs-NPC damage, friendly fire | Every hit does damage, whoever the shooter (the sentry's take_a_hit adds Take_a_hit_damage for every shooter). The relation [victim][shooter] picks the reaction: 8 / 1 friend -> random "seize friendly fire" message to the shooter; **2 neutral -> the provoke rule**: the victim (and same-faction characters it knows) move to the modified copy of their faction, which is set hostile to the shooter (and Ram's row to it when the shooter is Ram); 4 hostile -> nothing special. Any hit in combat makes the shooter the lock target ("whoever shot you last"). | take_a_hit 0x5E3580, relation read 0x5E3CB1..0x5E3D00 (REF GW_HIT_REL) |
| How an NPC's enemy is set | Vision (above) -> combat controller CurrentTarget (sentry +234h -> data +8); a hit -> MoveCtrl target_lock = the shooter and state 13 again; mission scripts: `set_priority_target` -> state 12 (shoot that target). | combat states 0x522430 / 0x521220 / 0x51FC70 |
| Do NPC-caused deaths count for missions? | Yes. The death state counts an enemy kill (EnemiesKilledOnCurrentMission +1, logic kill events 1 / 9 for "kill N enemies" objectives) when **Ram's row** holds 4 for the victim's faction and the hit had an owner, whoever the owner was. Per-placement death conditions (TrueOn 3, ActivatorType 4) fire on the placeholder's death, not on the killer. | death state 0x5D6B70: row-0 test 0x5D6F3B (REF GW_KILLROW0), counter 0x5D7172 (G_KILLS_R), KillOwner 0x5E4E8C (REF GW_KILLOWNER) |

## What gang wars does

1. Every 0.25 s (input tick) it looks at the live enemies within 80 m of Ram (the game's spatial query) and sorts the
   **eligible** ones into gangs by model (CharacterDict ModelStat number, sentry data +D4h):

   | Gang | Models | Where they meet a rival in the shipped game |
   |---|---|---|
   | cartel | CMLong 0, CMShort 1 | MM075 Hacienda (with elvez, 147 pairs within 25 m), sub-missions 01 / 25 (with gangster) |
   | virgilio | VB_Small 2, VB_Tall 3 | (never together with another gang) |
   | elvez | ElvezGuard 22 | MM075 Hacienda, sub-mission 17 (with gangster) |
   | gangster | Gangster 27 | sub-missions 01, 17, 25; MM110 |
   | army | Soldier 15 | MM080 Highway Bridge (with mexpolice, 97 pairs) |
   | mexpolice | MexicanPolice 24 | MM080 |
   | border | BorderPolice 5 | MM100 |
   | dea | DEA_Taskforce 9 (enemy DEA of the last missions) | MM100 / MM110 / MM120 (with outlaw: 344 / 173 / 82 pairs) |
   | outlaw | DEA_Outlaw 30 | MM100 / MM110 / MM120 |

   (`tools/mixed_groups.txt`; `tools/start_area.py` lists who stands near each mission start.)
   Eligible = faction 9 at its placement, a gang model, not a buddy, not a special sub type (escort 4, player 7,
   never-aggressive 8, civilian 9), health below `boss_health` (1000: bosses and specials), not a VIP placement, and not
   right now in state 2 / 3 / 12 / 31 (no AI control (scripted, Suave), mounted gun, mission priority target, buddy).
   Allies (faction 10), escorts, civilians, police and bosses are never moved.
2. A member with a member of a **rival** gang (`factions` pairs) within `range` goes to **war**: it gets a spare
   faction of its gang (`spare_factions`, one per gang at war; 4 / 16 / 17 / 3 by default). The spare faction's row and
   column in the game's table are faction 9's, except: hostile to the rival gangs (both ways), friend to the other
   gangs and to faction 9 (so an excluded enemy, such as a boss or a mounted gunner, is never a rival's target), and, with
   `provoke = 1`, **neutral towards Ram** while Ram's row keeps it hostile (his kills count, Loco moves and auto-aim
   pick them). The TargetDatabase's per-faction lists are marked stale (its own `touch_cache`), so within one scan
   (0.2-0.3 s) the member's own target choice switches to the nearest rival.
3. **Provoke**: when Ram shoots a member at war, the game's own neutral rule moves it and the friends it knows to the
   modified copy (spare + 19), which is hostile to Ram and to the rivals: they turn on him (and still fight the rival
   gang if it is closer). With `provoke = 0` Ram stays a target for everybody (free for all).
4. **Calm**: no rival within `range + hysteresis` for `calm_seconds`, or at war for `idle_seconds` without a rival as
   its own target (a wall between them): the member gets faction 9 back and hunts Ram again (no new war for
   `retry_seconds` after an idle calm). A spare nobody alive holds goes back to its shipped values. Timers run on real
   seconds while the game plays.
4b. **Wake**: a member still idling (passive states 4-8) when its war starts gets the game's own `alarm` (the message a
   hit civilian sends its friends; handler 0x5E7CA0, REF GW_ALARM): it goes aggressive and its vision picks the rival.
5. **Spark** (`spark = 1`): a fight starts only when Ram shoots a gang member while a rival gang member is within
   `range`; that member goes after Ram (provoked), the members within `spark_radius` fight each other.
6. **Paused** (the menu, where the game saves): every moved member gets its own faction back and the spare cells their
   shipped values, so a save holds what the game made; play resumes where it was. A root that still holds a spare
   faction when it is first seen (an older save, a retry) is repaired to its placement's faction.
7. The game may change a member's faction itself (a script, a trigger): that member is released and never written
   again.

Each member's faction write goes through the game's member-write notify (touch slot 2), as the game's own provoke rule
does. The matrix cells are written in memory only.

## Mission safety

* Mission scripts are not touched; no data change, no code site.
* Excluded from moving: buddies, escorts (sub type 4), allies (faction 10), civilians, police, bosses / specials
  (health >= 1000), VIP placements, mounted gunners (state 3), scripted characters (state 2), mission priority targets
  (state 12).
* Kills: the death state counts an enemy kill whenever Ram's row is hostile to the victim's faction and the hit had an
  owner (REF GW_KILLROW0); every spare faction keeps Ram's row hostile (unit test `test_matrix`), so a member killed by a
  rival counts exactly like one Ram killed. Per-enemy death conditions fire on the placeholder's death whoever killed.
  The log line of every death names the killer and the mission counter after it.
* Score: rival kills are counted as enemy kills (as DEA friends' kills are in vanilla). Whether the score / combo
  message of the death state (0x1FF, sent with the killer) gives Ram points for a rival's kill is not checked.

## Testing

Tested on the test copy with NEW GAME plus test-only override zips that make MM010 end at once and load the target
mission (not included).

* **MM075 Hacienda, no aid**: a natural cartel vs Elvez guard fight. An Elvez guard was killed by cartel fire 6 s in
  and the mission's EnemiesKilled went to 1; with no rival left in sight the cartel went back to faction 9 and attacked
  Ram, as designed.
* **MM075 with `debug_split = cartel`**: a long gang fight while Ram watches from the pool. 296 target switches between
  rivals, 9 deaths by rival fire (2 of them natural Elvez guards killed by cartel), `EnemiesKilled` 3 -> 9 without Ram
  firing a shot, Ram's health full throughout.
* **MM120 Runaway Train start**: 5 DEA taskforce and 1 DEA outlaw went to war and the game's own targets switched to
  the rivals; then they lost sight of each other between the wagons and stood idle next to Ram, ignoring him
  (provoke). That case is why the idle rule (`idle_seconds`) exists.
* The game-ms clock lags under load (an 8 s calm took 60 s), so the timers run on real seconds.

After every run the test copy, the real saves and the GOG files were checked byte-identical, and the registry
resolution was restored.

**About the demonstration aid.** Two gangs do meet in the shipped missions (MM075 cartel + Elvez guards, MM080
soldiers + Mexican police, MM100 / MM110 / MM120 DEA taskforce + outlaws), and natural fights happen. But the game's
character pool (6, at most 9 live enemy shells) is handed to the placements nearest Ram, so in MM075 the Elvez guards
lose their shells to cartel placements seconds after the start, and in MM120 the wagons keep the two groups out of
each other's sight. The long test therefore uses the test-copy aid `debug_split = cartel` (every second cartel member
joins a hostile copy of the cartel) next to the natural Elvez guards, in the place where both gangs stand; the
fighting, aiming, deaths and kill counting are all the game's own.

**Performance**: one tick every 0.25 s, 0.1-0.9 ms average per tick during a fight (0.006-0.06 ms per frame at
60 fps), one 11 ms tick at the mission start (discovery of 9 new characters with their log lines, the first matrix
write).

## Known issues and untested cases

* **Provoke by Ram in game**: the test key scripts cannot fire (mouse only). The rule is the game's own neutral-hit
  path (REF GW_HIT_REL) and is unit-tested (`test_step_game_changes`), but has not been seen in game. Same for `spark`
  (unit tests only).
* **Saves outside the pause menu**: the game autosaved at MM010's end (an "AUTO SAVE" screen). If such a save happens
  while members are at war, they are stored with the spare faction; with the mod they are repaired at first sight,
  without it such an enemy starts neutral to Ram until Ram shoots it (the game's provoke rule then turns it on him).
* Long play, mission completion with rival kills from start to end, MM080 / MM100 / MM110 and the sub-missions were
  not played. The kill counter rose with rival kills in the tests above; a "kill N enemies" objective completing on rival kills was
  not watched to the end.
* The score / combo message for a kill (0x1FF) when the killer is not Ram is not checked.

## Settings

```
[gang_wars]
enabled = 0
range = 25
hysteresis = 8
calm_seconds = 8
idle_seconds = 6
retry_seconds = 12
wake = 1
provoke = 1
spark = 0
spark_radius = 40
factions = cartel-virgilio, cartel-elvez, cartel-gangster, virgilio-gangster, elvez-gangster, army-mexpolice, dea-outlaw, dea-border, outlaw-border
spare_factions = 4, 16, 17, 3
boss_health = 1000
; group_<name> = model numbers adds or replaces a gang
tick_seconds = 0.25
scan_radius = 80
log_switches = 1
log_targets = 1
log_found = 1
status_seconds = 0
status_members = 0
; test copy only: debug_face_war = 1 (with [controls] mouse_turn_fix = 1), debug_split = <gang>
```

## Files

| Path | What |
|---|---|
| `code/gw_logic.h/.cpp` | pure logic: gangs, pairs, spare allocation, the matrix design, war / calm / provoke / spark states |
| `code/gw_game.cpp` | game glue: discovery, reads, faction writes, the matrix, pause restore, logging (SEH-guarded) |
| `code/test_gw.cpp` | unit tests (built and run by `..\dll\build.bat`) |
| `tools/faction_census.py`, `census_all.tsv` | every character placeholder of every block with its FactionID |
| `tools/faction_users.py`, `faction_users.txt` | every entity that sets FactionID, by type and value (which factions are free) |
| `tools/mixed_groups.py`, `mixed_groups.txt` | where two gangs stand within range of each other |
| `tools/start_area.py` | gang members near a mission's start teleport |
| `tools/gw_base.ini` | the base ini of the test runs |
