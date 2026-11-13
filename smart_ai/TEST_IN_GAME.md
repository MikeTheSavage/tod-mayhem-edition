# Testing Smart AI in game

A checklist for The Ambush mission. Run it on a separate copy of the game, never on your normal install, and never
while another copy of the game (TOD.exe / TotalOverdose.exe) is running.

## Setup

1. Make sure no game process runs: `tasklist | findstr /i "TOD.exe TotalOverdose.exe"` must print nothing.
2. Back up the game's registry settings from a plain shell:
   `reg export "HKCU\Software\Eidos\Total Overdose" %TEMP%\tod_reg_before.reg /y`. Restore it after the test with
   `reg import` and compare.
3. In the test copy of the game folder, move any other `dinput8.dll` proxy aside, then copy `dist\dsound.dll`,
   `dist\TODGameplay.dll` and a `TODGameplay.ini` that contains only:
   ```
   [smart_ai]
   enabled = 1
   log_states = 1
   ```
   Add the switches each test below asks for. If the test copy is hard-linked to the GOG folder, only add files there,
   never edit a linked one.
4. Use a separate Windows profile for saves (launch with `USERPROFILE` pointing at it) so your real saves are never
   touched. Put a test save into its `Documents\Total Overdose\savegames\Slot07`:
   * **A**: The Ambush at the mission start. Enemies spawn fresh through the AI init, which tests the spawn path.
   * **B**: The Ambush with Ram next to an active enemy. Enemies are restored from the save with no init, which tests
     discovery.
   * Or a copy of your own "The Ambush" save (a copy, never the original).

   Load it through the menu (load, slot 7). At the spot, turn and keep moving forward around the obstacles: machete
   enemies and one gunman come at Ram.
5. After every test: close the game, check that your real saves are unchanged, restore the registry, and remove the
   three DLL files (or leave them with `enabled = 0`). Put back any `dinput8.dll` you moved.
6. Evidence: `TODGameplay.log` in the game folder (every line below starts with `smart:`), plus screenshots or short
   captures.

Mark each line PASS or FAIL and note what you saw.

## 0. Loading and switches

| # | Do | Pass if |
|---|---|---|
| 0.1 | Start with `enabled = 1` | Log: `site SM_INIT 5E7C28 call -> ...`, `site POST_INPUT`, `KB_PROC`, `PAD_PROC`, `smart: installed - personality 1 (reaction 1), squad 1 ..., morale 1 ...`; no `differs` line |
| 0.2 | `enabled = 0` | No `smart:` line at all; `every feature is off: nothing patched` when no other feature is on |
| 0.3 | Main menu and load | Game starts and loads the save normally (no crash, no hang) |

## 1. Personalities (save A)

| # | Do | Pass if |
|---|---|---|
| 1.1 | Load A, walk into the first fight | One `smart: spawn XXXXXXXX <role> faction 9 hp ...: stop 10.0->7.0 / 13.5 / 8.5..11.5 ...` line per cartel enemy that appears; roles mixed (about 3 : 4 : 2.5 assault : rifleman : cautious); DEA friends (faction 10) also listed or `not managed` for buddies |
| 1.2 | Watch a group of gunmen | They no longer stop at the same distance: some come close, some stay back; some back off (run backwards / dodge back) when Ram walks up to them (cautious and most riflemen) |
| 1.3 | Compare with `personality = 0` | Vanilla: all stop at ~10 m and move alike |
| 1.4 | Melee enemies | Still pure melee (machetes sprint in and swing): the game resets their melee values in state 13; no change expected besides the reaction stagger |

## 2. Reaction stagger (save A, `log_states = 1`)

| # | Do | Pass if |
|---|---|---|
| 2.1 | Get seen by a group from more than 8 m | Log shows their `state 4 -> 13` within the same moment, then `13 -> 14` (waiting) for each, and the `14 -> 17/19/20` exits spread over ~0.2-1.0 s; on screen they start moving / firing one after another, not in one frame |
| 2.2 | Get seen within 8 m | No waiting (state 14 ends at once when the target is within 8 m) |
| 2.3 | Later re-engagements of the same enemy | No second wait (the log shows no new `-> 14` for it after its first fight) |

## 3. Squad: covering fire and turns (save A or B; needs 3+ gunmen on Ram)

| # | Do | Pass if |
|---|---|---|
| 3.1 | Fight a group of 3+ gunmen at 12-20 m | Log `smart: squad: N holding (DontHunt), max 2 advancing per target`; at most 2 close in at a time, the rest shoot from where they stand (states 19/20 with `DontHunt 1` in the state lines) |
| 3.2 | Keep fighting 10-20 s | The `holding` count changes, other enemies take their turn to advance (leapfrogging) |
| 3.3 | Hide (break line of sight) | Holders do not stand idle for long: after 5 s in state 19 without a shot they close in |
| 3.4 | Run away beyond 25 m | Everybody follows (no holds beyond 22 m) |
| 3.5 | Compare with `squad = 0` | Vanilla: everybody closes in to ~10 m |
| 3.6 | Pause (Esc) and resume | On pause the holds are released (DontHunt back to 0 in memory); after resume the `holding` line comes back |

## 4. Morale (save A, a group of 3+ cartel enemies)

| # | Do | Pass if |
|---|---|---|
| 4.1 | Kill one enemy next to others | Log `smart: an ally died at (...): N enemies within 15 m afraid` |
| 4.2 | Kill several quickly, wound the rest | Some survivors break: `state ... -> 23` in the log, they stop shooting and run; later (hitting a wall) they turn passive and fight again when they see Ram |
| 4.3 | Wait 20 s without kills | Fear fades; no more breaks |
| 4.4 | Stand-fast enemies (5 in The Ambush) and bosses | Never break |
| 4.5 | Compare with `morale = 0` | Vanilla: only badly hurt enemies flee, rarely |

## 5. Discovery (save B)

| # | Do | Pass if |
|---|---|---|
| 5.1 | Load B | Within ~1 s: `smart: found XXXXXXXX <role> ...` lines for the enemies around Ram (no init ran for them); a fighting one gets no reaction delay (`react 0.00`) |
| 5.2 | Fight | Squad and morale lines as in 3 and 4 |

## 6. Safety

| # | Do | Pass if |
|---|---|---|
| 6.1 | Mission change / death and retry / load another save | `smart: mission changed: N registered enemies forgotten` (or `game clock went back`), no crash |
| 6.2 | A mounted-gun enemy (if met) | Log `DontHunt changed by the game ...: squad leaves it alone`; the gunner behaves as vanilla |
| 6.3 | Rewind (if used) | No crash; enemies changed by the rewind are left alone (back-off lines) |
| 6.4 | Save in the menu with Smart AI on, uninstall, load that save | Loads; enemies behave as vanilla except the kept personality values ([README.md](README.md), "Saves") |
| 6.5 | 30 minutes of play | No crash, no hang; the log has no `exception` line |

## 7. Optional observations for future features

* Group alert: with `log_states = 1`, note whether passive enemies behind a wall react at all when their friends start
  fighting (vanilla: no). If a later build sends `alarm`, note how long an alerted enemy without sight stays in 13/19
  before `-> 4`.
* Flanking: note how often enemies hit walls on the straight approach (`17 -> 13` without reaching Ram).
