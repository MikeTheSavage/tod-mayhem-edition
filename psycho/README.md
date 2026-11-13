# PSYCHO: enemies use loco moves

`[psycho]` in TODGameplay.dll: on the PSYCHO difficulty, elite enemies and bosses can use Ram's own loco moves against
him. Phase 1 has the two self-contained moves, the **Tornado spin** and the **El Toro charge**. A killed elite also
leaves its move behind as a pickup (section 6).

| Part | Where |
|---|---|
| Pure logic + unit tests | `psycho/code/psycho_logic.*`, `test_psycho.cpp` |
| Game glue | `psycho/code/psycho_game.cpp`, `psycho_drops.inl` |
| REF rows | `dll/src/sites.h`, section `[psycho]` |
| Hooks | `dll/HOOKS.md`, "PSYCHO enemy loco moves" |

## 1. How a loco move runs (RE, GOG TOD.exe)

Based on the author's reverse-engineering notes on the loco moves and the time controller (not included), the Suave
work (`suave/`) and reads of the move bodies in the decompile and the raw instructions.

### 1.1 The path every move shares (all of it is Ram's)

| Step | Where | Hard-wired to the player |
|---|---|---|
| Trigger | LCONTROL -> realtime input / `character_player_ai` -> `character_powerup_ctrl.command_use_power_up` 0x5C0CB0 | the key, Ram's own power-up controller (root data +8Ch), his PowerUpInventory (charges, 12 slots), CurrentPowerUp |
| Eligibility | 0x5BCD90 | reads Ram's MoveCtrl state (no move in a dive, ladder, reload, another move) |
| Owner / running | coroutine 0x5BF2F0: RunningLocoMove (+20h) = CurrentPowerUp, the charge spent | Ram's controller; enemies have no power-up controller at all |
| HUD meter | `HUD_power_up_effects_ctrl.command_start_effect(type, 60)` / `command_end_effect` (0x5BF38D, 0x5C08C0) | the one HUD power-up widget |
| Taunt | DB_SOUND_POWER_TAUNTS_LIST for the player, spaced by 700 ms (AmbientSoundCtrl +104h) | Ram's voice lines |
| Dispatch | Golden Gun / Mariachi / Pinata -> Ram's WeaponCtrl state; Tornado / El Toro / Sombrero / Wrestler -> `MoveCtrl.command_do_combo_move` -> NormalMoveCtrl -> Combat_Move_ctrl states 26 / 44 / 47 / 48 | Ram's controllers; the NormalMoveCtrl sets the player's layer speed (PLAYER_SPEED_MULT 0.95) |
| End | `command_power_up_used` 0x5C08C0 | HUD end, Ram's selection refresh, his weapons restored |

### 1.2 Each move

| Move | Body | What it does | Hard-wired to the player | Which side its damage hurts |
|---|---|---|---|---|
| **Tornado** (slot 3) | Combat_Move_ctrl 0x5342C0 (fire on / off 0x527020 / 0x5270A0, cleanup 0x524020) | dual Mac-10 spin: ram_locomove (cache +73Ch, events 19 shoot start / 33 stop), blank shots on both guns' EffectCtrl (`command_fire_blank_shot` 1266, 0x535CB5 / 0x535D3B), one target hit every 0.66 real s | **slow motion**: TIME_CTRL change_timemultiplier(0.01, 0.3) at 0x53519E + ExclusiveMode (cache +864h) for the whole world; **camera**: CAMERA_NODE transitions / look-at (0x537D36, 0x5356E6, 0x537F09, 0x537F99, 0x5357A0); **HUD**: hide_target_brackets (0x534780); the start sting Play_Speak with speaker = Ram (0x5346AD) or menu sound 95; the player's layers x100 against the freeze; Ram's WeaponCtrl given dual Mac-10s; `command_combo_hack` / TriggerCtrl combo event | targets = a 30 m quadtree query (mask 40h) filtered by **relations row 0 = the player's row**, value 4 = hostile (0x534CD3..0x534D1C): whoever is hostile to Ram; `take_a_hit(300, owner = the mover ...)` 0x538148 + bullet_impact |
| **El Toro** (4) | 0x526730 (outer), 0x525490 (run), interrupt 0x526E50 | charge: ram_rammove_idle (cache +740h) then ram_rammove_run (cache +744h, root 11.17 m/s), move_speed 8 / 10 / 8 | **camera**: RamMoveCamPos on CAMERA_NODE (0x525604), fov 64 -> 69 -> 104 (0x525671, 0x52592B); **motion blur** GFX_controller (0x525740); **HUD** start_countdown(20) (0x52584D); god mode 20 s on GoodieCtrl; combo music Play_Speak / stop_speak (SoundDict 108 / 109); the player's physics model (`command_SetPhysicsModel`); `combo_move_finished` to the player AI | whatever the runner touches: its EnemyMoveCtrl gets Request_new_State (0x5259F7) into the ramMove_run_hit_* reactions; no faction test found |
| **Golden Gun** (2) | WeaponCtrl 0x611900, weapon_effects_ctrl 0x83D670 | GoldenGun (350 dmg, 100 m) | Ram's inventory backed up and replaced; the player's auto-aim cone PC_GoldenGun*; the **bullet camera** with time 0.01 (0x83E75C) | the player's targets (his auto-aim), bullets owned by Ram |
| **El Mariachi** (9) | WeaponCtrl 0x616450 + coroutine | guitar-case gun for 15 game s | Ram's weapon slot; **god mode** on Ram's GoodieCtrl; the **music** stream (SoundDict 110, the slot Suave borrows); the player's MariachiShootAngle / VisionAngle aim assist | the player's targets |
| **Pinata** (10) | WeaponCtrl 0x6167D0, `command_pinata_launched` 0x61C7E0 | thrown pinata shell (750 dmg), sentry AI `command_pinata_exploded` 0x5D1D50 | Ram's weapon slot; **spawns a helper** (a pooled weapon_shell with one of 3 pinata models) | the shell's blast (owner Ram); the enemies' sentry handler reacts to it (they gather at it) |
| **Sombrero** (6) | 0x5252E0 -> buddy state 0x5BDD60 | hat throw (ram_sombrerodecoy_*), then the Crazy Mexican buddy | **spawns a helper** `RamStuff Crazy Mexican/Mexican_Placeholder` with **the player's faction** (player_ai RamMexicanFriend +110h), buddy music / lines, 25 s life | the buddy fights the player's enemies (faction) |
| **Mexican Wrestler** (8) | 0x525160 -> 0x5BDD60 | luchador buddy with a bat | **spawns a helper** `Wrestler/Wrestler_Placeholder`, the player's faction (RamWrestlingFriend +10Ch) | the player's enemies |
| Time Stop (7, cut) | 0x5C0420 | WorldTime 0.01 for 15 real s | the whole world clock, the player's layers x100, the sound pitch factor | - |
| Rewind (1), Tequila (11), Medkit (5) | Scene rewind / no effect / not usable | | global or nothing | - |

Conclusion: no move can be "run on an enemy" through its own code. Every body takes the world clock, the camera or
the HUD, and its target filter is the player's relations row. What IS reusable is each move's **content**: the clips
(the cache's own anim slots), the gun's blank-shot effect, the game's power-up particles and the loco-move sting, and
`character_root.take_a_hit`, whose owner argument decides whose damage factor applies.

## 2. What the enemy does (phase 1)

Every check (0.5 game s) on PSYCHO, the DLL looks at the characters within 20 m of Ram (the Tornado's quadtree call).
An enemy may start a move when it is alive, hostile to Ram (the Tornado's faction test), not a buddy, in a fight
(sentry state 17..21), an **elite** (Health >= `elite_health`, default 200: CMLong, Soldiers) or a **boss** (>= 1000),
has been in the fight `first_delay` s, its own cooldown is over, Ram is at a distance that move allows, one roll of
`chance` passes, and a line test from its chest to Ram's chest is clear (the charge also needs a clear run at knee
height). Never while another enemy move runs (one at a time, then a `global_gap`), and never while Ram is in a vehicle,
a cut-scene, a rewind / pause, his own loco move, or the world time is frozen.

1. **Telegraph (1 s, `telegraph_seconds`)**: the enemy's AI is switched off (`command_no_ai_control`, as Suave's
   partner), its burst stopped, its MoveCtrl parked in the Victim controller (EngageBeating's `{3, 22}`) so its AI
   cannot overwrite the clip, turned toward Ram; the game's **red power-up flash** (ParticleDict 30
   PickupPowerups_Trigger_Red) bursts and the **orange goodie glow** (180 Goodie_Glow_Orange) burns on its chest, and the
   Tornado's own **loco-move sting** plays (SoundDict 107 Loco_Move_Serious, `loco move_02.wav`, the call at 0x5346AD).
   - Tornado: ram_locomove plays from frame 0 at the speed that puts the clip's shoot-start frame 19 exactly at the end
     of the telegraph (0.76 for 1 s): the spin's own wind-up is the warning.
   - El Toro: ram_rammove_idle (the bull stance) while the enemy keeps turning toward Ram.
   - **Shooting the enemy in the wind-up cancels the move** (any damage: `interrupt_damage`); the AI gets control back
     and the cooldown is halved.
2. **Active**:
   - Tornado: the firing frames 19..33 at speed 0.4 (1.4 s; the Tornado runs them at 0.25), the enemy's gun fires
     blanks every 0.09 s through its own `weapon_effects_ctrl` (muzzle flash, sound, casings), and every 0.35 s Ram is
     hit if he is within 12 m, in the enemy's line of sight (cover works) and not in a shoot-dodge (a dive dodges).
   - El Toro: ram_rammove_run along the line to where Ram stood when the telegraph ended, 9 m/s, up to 3 m past that
     spot; the enemy's own root motion may carry it, else the DLL moves it along that line (never pulled back); its
     collision is off for the run (it must not push its allies) and a line test 1.2 m ahead at knee and chest height
     ends it at a wall. Within 1.3 m of Ram he is hit once (unless he is diving) and pushed along the charge.
3. **Hit**: one `character_root.take_a_hit` on **Ram's** root with the enemy as owner (as a bullet's hit): the root relay
   0x5C4F40 multiplies by the enemy's Damage_mod, the player AI's take_a_hit applies the difficulty's damage factor
   (DamageHard; the BRUTAL / PSYCHO tuning makes it x2), god mode, and the health bar. Defaults 25 per Tornado hit (4
   hits at most), 60 for the charge. Nobody else is ever hit: no area damage, the blanks are blanks.
4. **Recover**: the rest of the clip (Tornado) or the stance (El Toro) for 0.5 s, then the enemy's MoveCtrl back to its
   default controller, its AI regains control (KnockedOutTimer cleared first, as Suave does).

No slow motion, no camera, no HUD, no power-up meter and no inventory is touched. Helper-spawning moves (Pinata,
Sombrero, Wrestler) are phase 2: they need a pooled shell / buddy placeholder and must respect the live-character cap.

## 3. The ini (`[psycho]`, active only on PSYCHO)

| Key | Default | |
|---|---|---|
| `enemy_loco_moves` | 0 | 1 = on (needs `[difficulty] enabled = 1` and PSYCHO picked in SETTINGS) |
| `moves` | tornado, toro | the moves enemies may use (aliases: spin, charge, el_toro) |
| `cooldown` | 20-40 | game seconds between two moves of one enemy (random in the range; "30" = fixed) |
| `telegraph_seconds` | 1.0 | the wind-up (0.3..3) |
| `elite_only` | 1 | 1 = elites and bosses; 0 = every hostile enemy in a fight |
| `elite_health` / `boss_health` | 200 / 1000 | |
| `chance` / `check_interval` | 0.35 / 0.5 s | |
| `first_delay` / `global_gap` | 6 / 5 s | |
| `interrupt_damage` / `interrupt_cooldown` | 0.5 / 0.5 | damage that cancels the wind-up; cooldown factor afterwards |
| `dive_dodges` | 1 | a shoot-dodge is never hit |
| `tornado_min_distance` / `tornado_max_distance` / `tornado_radius` / `tornado_damage` / `tornado_hit_interval` / `tornado_fire_speed` / `tornado_blank_interval` | 2.5 / 11 / 12 m / 25 / 0.35 s / 0.4 / 0.09 s | |
| `toro_min_distance` / `toro_max_distance` / `toro_speed` / `toro_overrun` / `toro_max_seconds` / `toro_hit_radius` / `toro_damage` / `toro_impulse` | 5 / 16 m / 9 m/s / 3 m / 2.5 s / 1.3 m / 60 / 9 | |
| `toro_collision_off` / `toro_sink_guard` | 1 / 0.5 m | the charging enemy is put back on its start height if it drops this far |
| `flash_effect` / `glow_effect` / `glow_kill` / `sound` / `sound_speaker` | 30 / 180 / 0 / 107 / 0 | ParticleDict / SoundDict ids; glow_kill 1 = the glow vanishes at once instead of fading |
| `scan_radius` | 20 m | |
| `elite_drops` / `drop_seconds` / `drop_max` | 1 / 30 s / 3 | section 6: a killed elite leaves its ability as a pickup; how long it stays; at once on the ground |
| `drop_track_radius` / `drop_settle_seconds` / `drop_body_reach` / `drop_body_drop` / `drop_give_up_seconds` / `drop_steal_distance` | 45 / 1.5 / 3 / 1.5 / 15 / 20 | how far elites are tracked; the body's settling; body further than this / lower than this = where the enemy stood; a drop with no prop waits this long; a level goodie this far from Ram gives its prop back |
| `log_verbose` / `perf_log_seconds` | 0 / 10 | |
| test copy only | | `debug_force_psycho`, `debug_start_key`, `debug_move` (1 tornado, 2 toro), `debug_dodge` (1 every move, 2 every second), `debug_dodge_lead`, `debug_heal_below`, `debug_face_enemy`, `debug_marks` |

## 4. Testing

The tests ran through an in-game test harness on a copy of the game (not included). It uses an isolated profile,
restores and compares the test copy, and checks the real saves and GOG files byte-identical afterwards. Save: an Ambush save; PSYCHO through the difficulty
sidecar (`level = psycho`) plus a Slot11 on HARD, `[difficulty] enabled = 1`. The seed saves live in the git-ignored
`work\psycho\seeds` folder and are not included.

Test settings: `difficulty.enabled=1`, `psycho.enemy_loco_moves=1`, `psycho.log_verbose=1`, `psycho.debug_dodge=2`,
`psycho.debug_heal_below=0.5`, `psycho.debug_face_enemy=1`, `psycho.debug_marks=1`, `debug.menu_mouse_off=1`; the
interrupt test adds `psycho.debug_interrupt=1` and `psycho.global_gap=3`.

Seen in game: El Toro dodged (red glow, the charge, Ram dives left, the bull runs past), El Toro hitting Ram, the
Tornado hitting Ram, a Tornado partly and fully dodged, and a wind-up cancelled by a scripted shot. The log shows the
TELEGRAPH / SPIN / CHARGE / HIT / miss / ENDS lines and the cost.

Every move was started by the enemies themselves under the shipped rules (elites only, chance, cooldown);
only Ram's dodge, the shot into the wind-up, his turn toward the enemy (for the camera) and the health refill are
test-copy helpers. Measured: one Tornado hit = 25 raw = 70 of Ram's 800 hp on PSYCHO; a full spin 280; the charge 168.
Cost 0.012-0.039 ms per tick on average, 2-5 ms on the 0.5 s check with `log_verbose = 1`.

## 5. Known issues and open work

- Phase 2: the helper moves (Pinata shell, Sombrero / Wrestler buddies) need a pooled shell / buddy placeholder for the
  enemy's side and must respect the live-character cap; Golden Gun / Mariachi would be an enemy weapon swap.
- Bosses were not met in a test fight; El Toro on slopes / stairs / near ledges not tried; damage values are first
  guesses; dodges and the interrupt were scripted (no human player).

## 6. Elite drops (`[psycho] elite_drops`, `psycho/code/psycho_drops.inl`)

An elite with a loco ability leaves that ability when it dies: a real goodie on the ground, Ram walks over it and has one
charge more of that move. On PSYCHO only, with the moves (`enemy_loco_moves = 1`); inert otherwise and on any other exe
(the REF rows of `sites.h`, section elite drops; a mismatch turns the drops off, not the moves).

### 6.1 How the game's own pickups work (RE, GOG TOD.exe)

| What | Where | Fact |
|---|---|---|
| Inventory | `character_powerup_ctrl`, slots 3 Tornado, 4 El Toro; goodie ids 10, 11 | data +14h -> {counts, maxima}; max 2 each; LocoMoveNum / RamMoveNum 1 |
| The grant | `character_goodie_ctrl.activate_goodie(id, amount)` 0x543010 (cmd 4CCh, msg -7) -> 0x542DF0 | ids 7..17; count + amount over the maximum = refused whole; a slot is selected when none is; `command_update_fast` on the HUD list |
| HUD | `HUD_pickup_effect.command_show_pickup` 0x6E7AA0 (cmd 2F6h) | played by the prop's pick state when the grant succeeded |
| The prefab | `PropRootGoodie.fragment`, 5 instances in `DefaultGoodieRessources.fragment` (overdose.main, nodes 0x1016CA..CE) | prop_root (Sub_Type 2) + PropCtrl `Goodie_Default` + MoveCtrl `prop_move_ctrl` + AICtrl `prop_ai_ctrl`; no new entity is possible without data, all goodies of a level share these five |
| The look | `Goodie_Default` command_reset 0x68AE90 -> 0x689F50 | model from cache +430h DB_MODELDICT_GOODIE_LIST[Sub_Type], Bonus, GlowType (B6h for 10, 11), PickUpsound, PickUpEffect; particle Trigger(entity) with the glow |
| The trigger | Ram's `character_weapon_ctrl` state 0x60DA20 | quadtree sphere query 0x4865A0 mask 800h at the pelvis - 0.4 m, radius 1; new entries get `trig(Ram, 1)` (cmd 19Fh) on the prop's AICtrl |
| The pick | prop_ai trig 0x798AB0 -> state 0x7982F0 | PropCtrl Owner = Ram, command_get_picked 0x689B40; Success 1: HUD show, ResourceCtrl -5; Success 0 (slot full): command_shake, back to idle |
| The vanish | prop_ai idle 0x7961E0 | AutoRemoveTimer data +4 > 0: sleep, then ResourceCtrl -5. Reset clears it (0x796AFD), so it is written after the reset. Idle starts over after a shake, so the timer alone is not enough for a time limit |
| The pool | `master_control_resource_ctrl` (MASTER_CTRL data +40h) | -13 request(type) 0x75FA60: first with ResourceStatus 0 and Locked 0, result in +0Ch / +10h; -14 allocate(placeholder, prop) 0x75E540; -5 deprecated 0x75E870; -10 free 0x75E6B0 (any status; hides, reset_to_default) |
| Dangling props | 0x75E9E0 (periodic, each prop) and sub ctrl 0x763D90 | ResourceStatus 1 with Placeholder 0 is freed within a second. Far beyond the model's drawing distance (model +3Eh, 150 m) a prop is given up (2ABh on its placeholder, then freed) or hidden |

### 6.2 What the DLL does

1. **The fixed ability.** `ps::ability_for(uid, moves)`: a hash of the placement's stable id (placeholder node `m_Id` >> 8), so
   one placement has one move in every run, after a reload and in every reinforcement that re-arms it. `pick()` only ever
   starts that move: an enemy on the charge stands still at a distance only the spin allows. The gun in hand is no input
   (at first sight the gun is often not out, so using it would give nearly every enemy the charge). An enemy with no
   gun cannot spin, and still drops the Tornado it was given. `debug_move` overrides for the debug start key only.
2. **The book** (`ps::EliteBook`, 128 entries, keyed by placeholder; a reused shell is another entry). Scanned every 0.5
   game s out to `drop_track_radius` (45 m: a gang war's kill counts, the killer plays no part), during moves too.
   Eligible: alive, hostile to Ram, Health >= 200, not a buddy / VIP / escort / scripted AI state, no boss unless
   `bosses = 1`. So it is `is_elite` without the bosses; `elite_only = 0` grunts use moves and drop nothing.
3. **The death** is read every tick from the shell that was seen alive: same AI and placeholder, state 28 or DamageTaken
   >= Health. A shell never seen alive cannot die (born-dead shells), a shell handed to another placement says
   nothing. No drop when the death falls in a cut-scene, a rewind or on the first tick after a gap of game time without
   ticks (the input tick does not run in a cut-scene).
4. **The body.** The pelvis is followed until it moved less than 2 cm for 0.25 s, at most `drop_settle_seconds` (1.5 s, well
   inside the ragdoll's 4 s corpse delay). The place: the body, unless there is no ground under it, a wall between it and
   where the enemy stood, or it lies more than 3 m away / 1.5 m lower: then where the enemy stood. Ground: bisection with the
   line test straight down (2.5 cm). Away from walls by 0.4 m.
5. **The prop.** -13 request(5); ResourceStatus = 2 with the game's touch; the show call; +5Ch = 10 / 11 with the touch;
   prop_root -4 command_reset; set_pos; AutoRemoveTimer. Status 2 is the trick: status 1 with no placeholder is freed as
   dangling, 2 is used by nothing in the game, requests want 0 and every free path takes any status. Everything from there
   on is the game's: model, glow, rotation, trigger, slot maximum, sound, HUD.
6. **Empty pool.** The level's goodies take the five props first. No free prop: the level goodie furthest from Ram, at least
   `drop_steal_distance` (20 m) away, gives its prop back the way the pool itself does it for a prop out of range (2ABh +
   free); its placeholder gets one again when a drop ends. None that far: the drop waits, up to `drop_give_up_seconds`.
7. **End.** The DLL frees the prop when `drop_seconds` are up (the prop's own timer restarts after a refused pickup).
   Collected = the prop left status 2 and Ram's count rose.
8. **Rewind.** A rewind takes Ram's counts and the props' members back with it (seen in game, section 6.3). The tick does not
   run while it plays, so the DLL compares the clock on the first tick after it. A death later than the clock never happened:
   the enemy lives, its drop goes, it can drop again. A drop made later than the clock whose enemy stays dead is made again. A
   pickup later than the clock did not happen either: the prop lies there again in status 2 and Ram has his old count, so a
   collected drop is kept 15 s for this and watched again. A jump of more than 10 s back is a load or retry: the book starts
   over. In every case one placement has one drop.
9. **Saves.** No new nodes, nothing added to a save. A save made with a drop on the ground stores a status 2 prop; once a
   second the pool is swept and a status 2 prop that is no live drop goes back. Level change: book and drops forgotten, the
   same sweep frees what lay there.

Reinforcements: **one drop per placement per mission**. The difficulty's reinforcements re-arm placements for as
long as a fight runs, so a drop for each comeback would be a charge farm.

With Bigger Fights: a dormant copy that wakes is a placement of its own, in any shell: it can be an elite and drops once
like any other. It is never a kill target (the data clears MissionObjectiveKill, IsVIP; `tests/data/test_bigger_fights.py` checks
every block). The gunmen of a heat car keep the police assault's IsVIP = 1, so `scan` marks them special: no moves, no drop, on any
trip. Three things were tightened for placements that come back: `EliteBook::add` throws out only entries that did not drop,
`undo_death` undoes only a drop made after that death (`t_drop`), and `clock_back` frees only that death's drop. Bigger Fights raises
the weapon and wear caps only, the five goodie props of `DefaultGoodieRessources` and `ResourceList[5]` stay as they are.

### 6.3 Testing

Same save and harness as section 4. Kills are the difficulty's debug kills, Ram's
walk is `debug_drop_walk`, the lethal hit in the rewind tests is `debug_ram_dies`; charges and the death prompt are the
game's. Checked in game:

* **Tornado drop**: an elite dies, the goodie lies at the body, Ram walks over it, the game's Tornado pickup banner
  shows, the HUD row goes 1 -> 2, LCONTROL runs the spin from the collected charge, and the next elite's drop lies
  beside him.
* **El Toro drop**: the same for El Toro (a charge spent first, since the save is at 2 of 2), with a kill that is not
  Ram's (`debug_kill_as_ram = 0`).
* **Rewind before the death**: a drop lies there, Ram dies, TAB: the rewind passes the elite's death and the drop is
  gone; killed again, the elite drops once.
* **Rewind past the pickup**: Ram collects, dies, TAB: the count goes back to 1, the drop lies there again and is
  collected again (1 -> 2); no second drop.

Measured on the rewinds: about 1.5 to 4 s of game time went back. Ram's counts follow the clock (tornado 2 -> 1), one Rewind charge
is spent (6 -> 5); the prop comes back in status 2 at its place. Other results: a full slot refuses the drop (PropCtrl
Success 0), it stays and goes at exactly 30 s; a 150 hp grunt drops nothing; with the pool empty the level goodie 46 m from Ram
gave its prop back; ragdolls: the drop falls 1.2 to 1.5 s after the death, 0.3 to 2.9 m from where the enemy stood.
Cost 0.007-0.031 ms per tick on average, 0.4-4.2 ms for the worst tick (the 0.5 s scan, `log_verbose = 1` and the trace on).

Not tried in game: a save made with a drop on the ground and loaded (the sweep is written for it and ran in the rewind runs);
a death inside a cut-scene (the rule is the 1 s gap of game time); a real gang-war kill (the debug kill that is not Ram's stands
in, the code has no killer input); drops on stairs and roofs (a drop on a roof Ram cannot reach just ends after 30 s).
