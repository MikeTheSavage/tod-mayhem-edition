# Bigger Fights (`[bigger_fights]`, data option `bigger_fights`, experimental)

## What it does

On BRUTAL and PSYCHO a fight starts with more enemies without anything having to die first: 9 at once instead of 6 on
BRUTAL, 11 on PSYCHO (measured, MM090). At heat 3 and up, a police car (with the mission's own gunmen in it, in a mission)
drives in on the road near Ram, stops, and three gunmen get out and fight him; heat 4 sends two cars at once, heat 5 three.

**It breaks saves.** With the option on, the game saves to `BFSlot00..` and shows only those: old saves are not offered and
a new game is needed. The old `Slot00..` files stay untouched and come back when the option is off. EASY, NORMAL and HARD
keep the game's numbers. The extras sleep there, and the vehicles only come on BRUTAL / PSYCHO too.

## How it is implemented

1. **Engine facts.**
   - Enemies are placeholders; CharacterResourceCtrl hands them one of 9 pooled shells, at most MaxAllocated (6, free roam
     8) at once. There are no spawners. So "more enemies from the start" needs more shells, a higher cap and more
     placements. A shell must stay spare: 0x7660E0 asks for a free one before it would steal one.
   - Sub-controllers MasterCtrlAux/ResourceCtrl/{Character,Vehicle,Weapon,Wear}ResourceCtrl: data +18h MaxAllocated, +48h
     wants, +4Ch has. Constants: 0x763694 / 0x7636A6 (6 / 8), 0x76355F weapon 15, 0x76350A wear 6, 0x7635C5 vehicle 4.
   - Saves address nodes by (block, creation index), so a create record in the middle of a block moves every node after
     it. That is why this is a save break. Slot11 is a settings file without node indices and is shared.
   - The slot name is formatted at five call sites of `sprintf(out, "Slot%02d", n)` (0x8976B8, 0x8978A1, 0x92654F,
     0x895D30, 0x926B6E, all calling 0x415300).
   - The police assaults are pure data: a vehicle_placeholder with `Vehicle_Passengers`, a Track "Vehicle_track" as its
     sibling (0x81C010 finds it by name, so each car needs its own folder) and a trigger of type 43, which is id 413 on the
     placeholder with {evil 1, passive 0, look ahead 10, radius 1}. Without a shell the placeholder moves along the track as a
     ghost (0x816DF0). The vehicle sub-controller gives it a shell inside a sphere of radius 30 m, 30 m ahead of the
     camera (0x763FF0), and hands the pose over (0x823420). **The AI of the shell does not take the order over: id 413
     has to be repeated when the shell arrives** (seen in game: without it the car coasts to a stop and nobody gets out).
     Past the last point the car brakes and the passengers exit (0x80C710, 0x80BE30, Stay_In_Vehicle false).
   - A passenger's spatial bit 80h is set only to ask for a character (0x5A37A0), and it is off while he has one. Dead is:
     had a shell, has none, placement hidden.
   - The road graph is database_traffic_point (class handler 0x655480, data +1Ch NextPointList, +20h PreviousPointList),
     about 250 points per submap, none on maps 6, 7, 8, 9 and 10 (MM032, MM075, MM090, MM120, the hacienda and jungle).
   - The traffic can fill all 4 car slots and hold them while Ram stands still (MM110, MM066, MM040: 4 of 4 for minutes);
     in the junkyard they turn over and a slot is free most of the time. A car also needs Ram's camera near the end of its
     route: a ghost 65 m away behind walls never gets a shell (IM04, Ram indoors).
2. **Data (`data/gameplay/bigger_fights.py`).** Built from the player's own blocks like every data option.
   - `overdose.main`: pools 9 -> 14 characters, 16 -> 24 weapons, 7 -> 10 wear, copies inside DefaultEnemy / Weapon / Wear
     resources. Still a 32-bit exe that is not large-address aware, so memory is measured below.
   - Mission blocks: dormant copies of plain gunmen, next to their donors (`BF1_nnn` for BRUTAL, `BF2_nnn` for PSYCHO; flag 1,
     MissionObjectiveKill, IsVIP, DontHunt off; entity references leaving the copy cleared). A block with buddies or an
     escort gets the small tier `BF3_nnn`. Where every enemy is a kill target (the SUBM kill missions) the targets are the
     donors; the copies are never targets. Own checks: old creates keep their order, nothing in a copy reaches outside,
     round trip.
   - Three dormant cars per block (`BFS<i>` { `BFV<i>`, `BFG<i>_<j>` x 3, Vehicle_track with 8 points }), copied from the
     game's `Police_Vehicle_01` (Police_Assault_MissionStructure_03), not in pure races, the tutorial, game-over blocks or
     blocks with buddies or an escort; in the intermissions yes (their friends only stand around mission starts). Mission
     gunmen take the look of the mission's commonest donor because gang models are loaded per mission; in free roam they
     stay cops (model 23 is the only character model always loaded).
3. **DLL (`dll/src/gp_bigfights.cpp`, rules in `gp_bigfights_logic.cpp`).** Everything is off unless `enabled = 1`, and every
   address is byte-checked (`sites.h` BF_ rows).
   - Five CALL patches: the slot name stub writes `BFSlot%02d` for every slot but 11.
   - Once per second in the input tick: the live cap follows the level (`bf_live_cap`, read by gp_difficulty.cpp's cap
     code), the weapon and wear caps rise by what the data added, and on a mission change or a load the tree is walked
     once for `BF*` nodes (15,000 nodes, under 1 ms).
   - A copy wakes when its donor is live within `wake_radius` of Ram and its tier belongs to the level. Its spot is the end
     of the donor's own path, or a ring around the donor; each spot needs a clear line from the donor, a floor below, room
     above and nobody on it (line tests with Navigator::IsPointOccluded 0x91D020). Then SetFlags, and the resource controller
     treats it as any other placement. Near Ram it is told to fight (id 0x19F).
   - Vehicles: at heat 3+ when the level allows it, with no buddy or escort seen, and a car slot free. The traffic points are
     collected once for the start. A random near point (else the nearest road within `vehicle_stop_far`) is walked back
     along the graph for `vehicle_drive_in` metres and resampled to the 8 track points, so the car comes in with the
     traffic. Then the track points and the car move (0x88E600), the folder is enabled, and id 413 goes out on the next tick
     and again when the shell arrives. When its gunmen are dead the folder is hidden once the car is out of the way and
     the three can go out again (re-armed through `df_rearm_placeholder`, which also clears the stored DamageTaken,
     see [difficulty.md](difficulty.md)).
4. **The rest of the mod.** Copies and gunmen are normal placements, so gang wars, ragdolls, Suave and the
   reinforcement engine see normal enemies. The heat vehicles sit on top of heat's waves. They read `heat_drive().level`.
   Vehicles are normal cars for car physics.
5. **With elite drops.** A copy is an ordinary placement, so it can become an elite if its health is 200
   or more; one placement drops once, also when the same pooled shell comes back on another placement. A car's gunmen can
   never drop: they take `IsVIP = True` from the game's own `Cop_Assault_01` prefab and PSYCHO treats a VIP as special (no
   moves, no drop). Only the copies clear IsVIP. A copy is never a kill objective (`tests/data/test_bigger_fights.py` checks
   every block). The bigger pools leave the five goodie props alone: only the weapon and wear caps are raised, and the
   `DefaultGoodieRessources` fragment stays byte-identical.

## Ini keys (`[bigger_fights]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | the feature; the installer's toggle |
| `separate_saves` | 1 | BFSlotNN instead of SlotNN |
| `extra_enemies` / `heat_vehicles` | 1 / 1 | the two halves |
| `cap_scale_brutal` / `cap_scale_psycho` | 1.5 / 1.85 | live enemies: the game's cap x this, at most shells - 1 |
| `wake_radius` / `join_radius` | 90 / 30 | m from Ram: a copy wakes / is told to fight |
| `heat_vehicles_in_missions` | 1 | 0 = free roam only |
| `vehicles_heat3` / `4` / `5` | 1 / 2 / 3 | vehicles out at once |
| `vehicle_every_seconds` | 30 | at most one new vehicle per this many seconds |
| `vehicle_stop_min` / `max` / `far` | 12 / 34 / 110 | where the route ends |
| `vehicle_drive_in` / `vehicle_seconds` | 80 / 150 | road driven in on / when a squad is given up |

Test-only: `force_tier`, `force_heat`, `debug_stats`, `debug_crash_log`, `extra_weapon_props`, `extra_wear_props`, `log`;
in `[difficulty]`: `debug_kill_seconds`, `debug_kill_as_ram`, `debug_kill_range`.

## Testing and limits

- Unit tests `tests/dll/test_bigfights.cpp`; host test 24 (off = no patches; on = the 5 slot sites, the input tick and
  the fixes); data test `tests/data/test_bigger_fights.py` (among other checks, no copy is ever a kill objective).
- Checked in game on a test copy of the game: in MM090
  a fight holds 9 enemies at once on BRUTAL and 11 on PSYCHO, against 6 without the option.
- Treat it as experimental: it changes save files and has had less play than the rest of the mod.

Limits worth knowing:
- A vehicle needs a road (`no road route` in the log: maps 6-10, or Ram deep inside a building) and a free car slot
  (`no vehicle now`); otherwise it is skipped and tried again. In buddy and escort missions there are none at all.
- Old data + BF saves, or BF data + old saves, crash the game on load. `BFSlot` names keep them apart;
  `separate_saves = 0`, or leaving the data option on with the DLL feature off, removes that protection.

## Files

`dll/src/gp_bigfights.cpp`, `dll/src/gp_bigfights_logic.cpp/.h`, `data/gameplay/bigger_fights.py`,
`tests/dll/test_bigfights.cpp`, `tests/data/test_bigger_fights.py`, `bigger_fights/tools/`, `dll/HOOKS.md`
"Bigger Fights".
