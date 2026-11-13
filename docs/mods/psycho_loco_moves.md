# PSYCHO: enemies use loco moves (`[psycho]`)

## What it does

On the PSYCHO difficulty, elite enemies and bosses can use two of Ram's own loco moves against him: the **Tornado spin**
(guns blazing all round) and the **El Toro charge**. Each move is telegraphed for one second with a red flash, an
orange glow and the loco-move sting. Shooting the enemy in the wind-up cancels it; a shoot-dodge avoids the hits.

## How it is implemented

1. **Engine facts** (full table in `psycho/README.md` section 1):
   - Every loco move runs through Ram's own `character_powerup_ctrl` (use 0x5C0CB0, coroutine 0x5BF2F0), then Ram's
     WeaponCtrl or Combat_Move_ctrl. Each body also takes something global: the Tornado slows the world clock and
     moves the camera; El Toro changes the camera fov, motion blur, HUD countdown and god mode. The Tornado's targets
     are filtered by relations row 0 (the player's). So no move body can run on an enemy.
   - What can be reused is the content: the cache's anim slots (+73Ch ram_locomove, +740h ram_rammove_idle, +744h
     ram_rammove_run), the gun's blank shot (`command_fire_blank_shot` 1266 on its EffectCtrl), the power-up particles
     (ParticleDict 30 and 180), the sting (SoundDict 107), and `character_root.take_a_hit`, whose owner argument decides
     whose damage factor applies.
   - The TIMESTAMP clock node (cache +274h) runs at a tenth of game time; timing uses the game-ms global 0xA3DCD4.
2. **No code site of its own.** It runs in the shared input tick and checks its REF rows (`PS_*`) first.
3. **Pick** (every 0.5 game s, within 20 m): alive, hostile (the Tornado's faction test), not a buddy, in a fight
   (sentry state 13, 17-22 or 25), elite (Health >= 200) or boss (>= 1000), in the fight 6 s, own cooldown over
   (20-40 s), distance fits the move, a 0.35 roll, a clear chest-to-chest line (the charge also at knee height). One
   enemy move at a time, then a 5 s gap. Never while Ram is in a vehicle, a cut-scene, a rewind or pause, his own loco
   move, or frozen time.
4. **Telegraph (1 s).** The enemy's AI is switched off (`no_ai_control`), its burst stopped, its MoveCtrl parked in the
   Victim controller (`Request_new_State {3, 22}`), turned to Ram. Red power-up flash and orange glow on the chest, the
   sting plays. Tornado: ram_locomove plays at the speed that lands its shoot-start frame at the telegraph's end.
   El Toro: the bull stance. Any damage in the wind-up cancels the move, gives the AI back and halves the cooldown.
5. **Active.**
   - Tornado: firing frames at speed 0.4; the gun fires blanks through its own effects; every 0.35 s Ram is hit if he
     is within 12 m, in sight and not diving (at most 4 hits of 25 raw).
   - El Toro: ram_rammove_run along the line to where Ram stood, 9 m/s, up to 3 m past it, collision off, a wall line
     test ends it. Within 1.3 m of a Ram who is not diving: one hit of 60 raw and a push.
6. **Damage.** `take_a_hit` on Ram's root with the enemy as owner: the enemy's Damage_mod applies, then the player's
   difficulty factor (25 raw = 70 hp on PSYCHO). Nobody else is ever hit.
7. **Recover** 0.5 s, then the MoveCtrl goes back to its default and the AI regains control. No slow motion, camera,
   HUD, meter or inventory is touched.

## Safety rules

- **No Ram voice.** SoundDict 107 (`loco move_02.wav`) is Ram's own shout, so `sound` defaults to -1 (no sting) and
  Ram's voice lines 106-109 are refused even when the ini asks for them.
- **Never bosses.** `bosses = 0` (default): bosses are mission actors driven by the mission script. VIP placements,
  escorts / special sub types and scripted AI states (2, 3, 12, 31) never use a move either.
- **A new enemy waits again.** The pool hands a shell (root, AI) to the next placement, so cooldown memory keyed by the
  shell would let a new enemy inherit the old one's fight time (a boss in MM032 could move 1.2 s after it appeared).
  The memory starts over when the shell's placeholder or max Health changes.
- **Hand-back watchdog.** A rewind can put the world back to the middle of a move: the enemy's AI is off again
  (state 2) and nothing would hand it back, so the enemy would freeze. Every enemy a move touched is watched for
  `handback_watch_seconds` (30) and handed back again when its AI is in state 2 (never during the rewind or a pause).
  Log: `had no AI control again ... handed back again`.
- **Ram dead.** Any hp <= 0 ends the move (a test for exactly 0 hp missed overkill, and a spin kept hitting at -46 hp).
  A lethal hit during a move gives the AI back so the game lets the enemy die, and restores the clip speed.

## Elite drops

Every elite (200+ hp, no boss, VIP, escort or scripted enemy) has one fixed loco ability, Tornado or El Toro, worked out from
its placement: the same enemy has the same move in every run. It uses only that move. When it dies it leaves that move as a
pickup on the ground, whether it used the move or not and whoever killed it: Ram walks over it and has one charge more.

- **It is the game's own goodie**: one of the five pooled goodie props, with its model, glow, trigger, slot maximum (a full
  slot refuses it and leaves it lying), sound and HUD. Nothing new in the data, the saves or the menus.
- **Rules**: one drop per placement per mission (a re-armed reinforcement drops nothing again); 30 s on the ground; on the
  ground at the body, or where the enemy stood when the body fell away or lies behind a wall; at most 3 at once; none in a
  cut-scene; a rewind before the death takes the drop back and the enemy can drop it again; a rewind past a pickup
  takes the charge back and the drop lies there again.
- **With Bigger Fights**: an extra enemy is a placement like any other, so an elite copy has its own move and its own one
  drop. The gunmen of a heat car are VIPs in the game's own police assault data, and elites exclude VIPs: they use no moves and
  leave nothing, on any trip. The rule holds without that too: the book is keyed by placement, a second life in another shell
  finds the same entry, a full book never forgets a placement that dropped, and a rewind past a later death does not undo
  an earlier life's drop.
- **The pool**: the level's goodies come first. With no free prop the level goodie furthest from Ram (20 m or more) gives its
  prop back, as the pool does for a prop out of range, and comes back when a drop ends.
- How it is built: `psycho/README.md` section 6, calls in `dll/HOOKS.md`.

## Ini keys (`[psycho]`, active only on PSYCHO)

| Key | Default | Meaning |
|---|---|---|
| `enemy_loco_moves` | 0 | 1 = on (needs `[difficulty] enabled = 1` and PSYCHO picked); 0 = PSYCHO plays as BRUTAL |
| `moves` | tornado, toro | which moves enemies may use |
| `cooldown` | 20-40 | game seconds between two moves of one enemy |
| `telegraph_seconds` | 1.0 | the wind-up |
| `elite_only` / `elite_health` | 1 / 200 | elites only; elite threshold |
| `bosses` | 0 | 1 = bosses (>= `boss_health`) use moves too |
| `sound` | -1 | a SoundDict sting at the telegraph; Ram's voice lines 106-109 are refused |
| `handback_watch_seconds` | 30 | how long a moved enemy is watched for a rewind's AI-off state |
| `elite_drops` | 1 | a killed elite leaves its ability as a pickup |
| `drop_seconds` | 30 | how long a drop stays |
| `drop_max` | 3 | drops on the ground at once (the pool has 5 goodie props) |

More keys with code defaults (distances, damage, speeds, effect ids) are in `psycho/README.md` section 3. Test-only:
`debug_force_psycho`, `debug_start_key`, `debug_move`, `debug_dodge`, `debug_dodge_lead`, `debug_heal_below`,
`debug_face_enemy`, `debug_marks`, `debug_interrupt`, `debug_rewind_sim` (s after a hand-back the AI is switched off again, as a rewind restores it),
`debug_drop_key`, `debug_drop_walk`, `debug_drop_walk_wait`, `debug_drop_trace`, `debug_rewinds`, `debug_ram_dies`, `debug_ram_dies_on`.

## Testing and limits

- Unit tests `psycho/code/test_psycho.cpp`; host test 18.
- Checked in game on a test copy of the game with The Ambush save on PSYCHO: 4 moves by two
  elites in 106 s under the shipped rules (Tornado dodged, El Toro hit, El Toro dodged, Tornado 4 hits); a fully
  dodged spin; wind-ups cancelled by a scripted shot.
- Cost 0.012 to 0.039 ms per tick on average.
- Not verified: a human player in the loop (dodges and the interrupt were scripted), bosses, El Toro on stairs, slopes
  or near ledges. Damage numbers are first guesses. In the combined test no move triggered (debug kills removed the
  elites first).
- Not built: Pinata, Sombrero and Wrestler (they spawn helpers), Golden Gun and Mariachi.

## Files

`psycho/code/psycho_game.cpp`, `psycho_drops.inl`, `psycho_logic.cpp/.h`, `test_psycho.cpp`, `psycho/README.md`, `dll/HOOKS.md` "PSYCHO enemy loco moves".
