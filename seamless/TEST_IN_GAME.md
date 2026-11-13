# Testing seamless map borders in game

Setup: install with the installer's Seamless World card on (data option `seamless = on_nomarkers` or `on`,
`TODGameplay.dll` with `[seamless] enabled = 1`, `TODGameplay_seamless.txt` next to `TOD.exe`). Turning Smart AI on
as well lets you check both together (see the last section).

## What "pass" looks like at every border

| Check | Pass | Fail (note x, z, what you saw) |
|---|---|---|
| No fade | the picture never goes black and no load-screen picture or progress bar appears | black screen, load picture or bar |
| Short hitch | the game freezes for a moment on the last picture (about 0.3-0.8 s; the first crossing after starting the game can be longer), then carries on | a freeze of several seconds every time |
| No teleport | you are where you were, still walking or driving in the same direction | you jump to another spot (only expected at the farm pass, see 5) |
| Right map | the area across the border has its people, cars and pickups after the hitch | empty streets that stay empty, falling through the ground |
| No ping-pong | one hitch per crossing | a second hitch right after the first, or a loop of them |
| Minimap / HUD | minimap, health, weapon and power-up bar back and correct after the hitch (they may blink off for the frozen moment) | HUD missing afterwards, minimap of the wrong area |
| Sound | music and street sound continue (they may dip during the hitch) | silence that stays, or stuck sounds |
| Save / load | save at a save point after crossing, quit to the menu, load it: you load in the new area, the next crossing works | the save does not load, or loads in the wrong area |

`TODGameplay.log` (next to `TOD.exe`) has one line per load. A border crossing shows
`seamless: load #N: <area> + <map> -> SEAMLESS border swap ...`, then `UpdateLoadedBlocks ... ms (seamless border swap ...)`
and `seamless: border swap #N done ...: blocks swapped in X ms, trigger to play Y ms`. Missions, taxis and deaths show
`-> normal load (...)` and keep the vanilla fade and load screen. `seamless prefetch: ...` lines show the next area
being read in advance near a border. Keep the log after the test: the swap times are the measurement.

With `seamless = on_nomarkers` the walk borders show no marker:
the loader beam (the tall translucent MapTeleport light) and the area sign (the small ExitMap_<area> icon) of every walk
border are shrunk to zero size and opacity 0. Pass: no beam or sign at any city / jungle border; the farm pass, the
ZAP stops and mission loaders still show theirs. (With plain `on` the beams stay and the two moved boxes carry theirs.)

## Where to walk (main city borders first)

Coordinates are game metres (x east-west, z north-south; north = +z). The game shows no coordinates; the
descriptions and the log tell you when you crossed.

1. **Downtown <-> Industrial** (walk borders, vanilla data; now without load screen)
   - Downtown 01 -> Industrial 04: the street at the west edge of Downtown, box at x -376, z 53; back east at
     x -329, z 20. Cross on foot and in a car, both ways.
   - Inside the Industrial district: Industrial 04 <-> 01 / 02 / 03 (40+ border boxes, x -940..-370). Drive around
     the district: every swap must be a short hitch.
2. **Downtown <-> Harbour, the bridge** (was a teleport, now a walk border)
   - From Downtown 01 drive south onto the long harbour bridge (x about -306, heading south from z -300).
     Old: a load at the north end (z -329) put you 190 m further south. New: **no** swap there; the swap fires near
     the south end (z about -532, the beam marks it) and you are still on the bridge at that spot, Harbour around you.
     Drive on into the harbour: no second swap. No beam or sign anywhere on the bridge.
   - Back north: the swap fires at z about -509, you stay where you are on the bridge (old: you were moved to
     z -304), Downtown loads; drive on north: no second swap.
   - Repeat on foot (no falling through the deck at either swap point).
3. **The tunnel, Downtown 02 <-> Industrial** (was a teleport, now a walk border)
   - The road tunnel at the west edge of Downtown 02 (east portal near x -297, z 476; west portal near x -480, z 428).
     Westbound: old: a load at the east end (x -319) put you 136 m further west. New: the swap fires near the west
     end (x about -449), you stay in the tunnel, Industrial 04 loads, drive out of the west portal.
   - Eastbound from the Industrial side: the swap fires at x about -428 (old: you were moved to x -297), Downtown 02
     loads around you in the tunnel; drive on east: no second swap at the old Downtown box.
   - Also enter the tunnel from Industrial 02 (the part of the Industrial district north of z 0, x -940..-420): same
     eastbound swap.
4. **Jungle 01 <-> 02** (walk border, vanilla data; now without load screen)
   - Map09 (the jungle missions, first one: Ernesto Cruz). The border boxes are at x -515, z -2395 (Jungle 01 side)
     and z -2355 (Jungle 02 side), about 100 m below the plateau. You cross it during the mission where the path
     goes down there; the log line confirms it. The two boxes are 18 m apart in height: watch for falling or a
     second swap.
5. **Farm pass, Farm 01 <-> Farm 02** (unchanged: still a teleport)
   - The two farm blocks do not share the ground of the pass (their terrain meshes differ by up to 20 m and neither
     has ground at the other's border box), so it cannot become a walk border. Pass = it still works as before: the
     load screen appears and you arrive at the usual spot. The log shows `-> normal load (a teleport ...)`.

## Not seamless on purpose (should look exactly like vanilla)

Mission starts and ends, taxis, ZAP stops, cutscene changes, dying (game over / retry), loading a save, the main menu.
Spot-check one taxi ride and one mission start: fade and load screen as before.

## Smart AI on the same install

With Smart AI (`[smart_ai]`) switched on too (its own checklist is [../smart_ai/TEST_IN_GAME.md](../smart_ai/TEST_IN_GAME.md)):
enemies in the area you leave are unloaded by the swap as in vanilla (Smart AI forgets them when their nodes go away);
enemies of the new area get their personality as they spawn. A fight across a border is a good combined test:
after the hitch the enemies behind you are gone (vanilla), the ones ahead behave as Smart AI says.

## Undo

Restore a backup in the installer, or switch the Seamless World card off and install again. To keep everything else
and only turn the DLL half off: `[seamless] enabled = 0` in `TODGameplay.ini` (the bridge and the tunnel then still
swap without teleport, with the vanilla load screen).
