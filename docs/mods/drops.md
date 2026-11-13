# Drops: more ammo, health and the odd power-up from dead enemies (`[drops]`)

## What it does

On every difficulty a killed enemy leaves more than in the original game:

- **Ammo.** Its gun carries `ammo_scale` (1.75) x the vanilla bullets. This replaces the difficulty's ammo factor
  (HARD x0.75, BRUTAL and PSYCHO x0.5), so PSYCHO goes from half the vanilla ammo to 1.75 x. With
  `keep_difficulty_ammo = 1` the two multiply instead.
- **Health.** A kill leaves a medium (150) or large (400) health goodie with chance `health_chance` (0.2). The chance
  rises by `health_hurt_bonus` (0.3) x the share of Ram's health that is missing, so it is 0.5 when he is nearly dead.
  A quarter of the health drops are large.
- **Power-ups.** A kill leaves a power-up with chance `powerup_chance` (0.05): rewind, golden gun, tornado, El Toro,
  sombrero, wrestler, mariachi or Piñata, by the `weight_*` keys. Only slots that are not full come: a full or locked
  slot never drops. The Piñata never drops while Suave takes its slot (`[suave] slot = 10`). A PSYCHO elite that already
  leaves its loco move ([psycho] elite drops) gets no extra power-up.

The pickups are the game's own goodies. One of the five pooled goodie props of `DefaultGoodieRessources` brings its
model, glow, rotation, trigger, pickup, sound and HUD effect. It lies beside the body, 0.7 m toward Ram, because the
dropped gun lies on the body. Each pickup stays `drop_seconds` (30), and at most `max_on_ground` (3) are out at once.
There are none in cut-scenes. A rewind takes a pickup back, and a load forgets them.

## Vanilla: what a dead enemy drops (GOG TOD.exe)

- **The gun, with ammo, and the hat.** That is all.
  - The kill (`character_placeholder command_kill` 0x5A8950) and the sentry's dying state (0x5D6B70) request no goodie.
  - `character_placeholder.PlaceholderPropList` is empty in all 2,830 placeholders of the data (filled at run time).
- **The gun's bullets.** `default_weapon` init 0x666530 (REF `DR_WPN_BULLETS`) sets:
  - BulletsToDrop (data +80h) = max(1, int(pct / 100 x MaxBulletsToDrop (+7Ch))).
  - pct is random within the band of the gun's drop level, from BulletDrop (cache +2C0h, data +0Ch..+30h): VeryFew 10-30,
    Few 30-80, Average 80-100, Many 100-150, VeryMany 150-250.
  - MaxBulletsToDrop is the clip size: x2 for the Hudson, x5 for throwables, launchers and the Piñata.
  - `[difficulty]` multiplies the ten band values by its ammo factor.
- **Health and power-ups** come only from goodies placed in the levels (`goodie_placeholder`) and from combo kill awards
  (ComboAwardDict).

## The Piñata (power-up slot 10, goodie 16)

- **It is never placed in a level.** No `goodie_placeholder` in any of the 696 blocks of blocks.naz has
  GoodieSubType 16.
- **Nothing unlocks it.** Only tutorial_01 has `remove_move` triggers, and those are for another move.
- **The only source in the original game is a kill combo.** ComboAwardDict:
  - 12 kills: Mariachi, El Toro or Piñata (one of those not full, at random);
  - 30 and 40 kills: Mariachi, Piñata, Sombrero, El Toro or Wrestler;
  - 50 and 60 kills: Golden Gun, Tornado, Mariachi, Piñata, Sombrero, El Toro or Wrestler.
  - The other awards: 3 kills medium health, 6 Tornado or Golden Gun, 9 Rewind, 18 Wrestler or Sombrero, 24 Rewind or
    health.
- **Some missions count no combos at all.** The combo kill handler (0x69CB20, REF `DR_COMBO_BLOCK`) ignores the kill when
  the mission data (cache +25Ch -> data) +78h is set. `[drops]` logs this flag at every mission start; MM010 and MM013
  both log "combo kill awards on".
- **MM013 (Tommy Cruz) has no goodie placements at all**, only slow-motion camera triggers. The farm map has none either.
- **No other part of this mod hides it.**
  - Suave defaults to slot 11, not the Piñata's slot 10.
  - Elite drops only take props from level goodies, and no level goodie is a Piñata.
  - Bigger Fights does not touch the goodie pool or the combo awards.
- With `[drops]` on, the Piñata can also drop from any kill (weight 2 of 13 of the 5 % power-up chance).

## Where a pickup rests

A level goodie is not on the floor. Its prop sits exactly at the placeholder position, and the placeholders float
above the ground. The probe in MM010 measured three level goodies at 0.64, 0.73 and 0.89 m above the floor found by
the game's own line test. The same line test under Ram reads -139.86 against his root at -139.88, so the test itself is
right.

A drop placed at the floor itself (-140.00 in that MM010 spot) would sink half its model into the ground. So elite
drops and `[drops]` use the same rule:

1. Line test down at the spot and at four points 0.25 m around it.
2. Start from the centre hit (or the lowest hit if the centre misses). Take the highest hit that is at most 0.35 m
   above it, so a step or a slope edge does not swallow the model, but a table or a wall top is ignored.
3. Add the lift, 0.75 m by default (`lift` in `[drops]`, `drop_lift` in `[psycho]`).

The goodie reset (0x689F50) also copies the node position into OrgPos (+38h). Level goodies read (0,0,0) there, so it is
not the position source; the drops leave it alone.

Checked in game: a test health drop rests at the same height as the level's rewind goodie next to it.

## Files and keys

- Code: `drops/code/drops_game.cpp` (glue), `drops_logic.cpp` (pure, unit tested by `test_drops.cpp`).
- `gp_difficulty.cpp` writes the BulletDrop bands with `loot_ammo_factor()`. Without `[difficulty]` tuning, `[drops]`
  writes them itself, after the same vanilla check (10 / 250).
- REF rows: `DR_WPN_BULLETS`, `DR_COMBO_BLOCK`, `DR_GOODIE_ORGPOS`, plus the shared goodie-pool rows of elite drops
  (`PS_RES_*`, `PS_PROP_*`, `PS_SHOW`, `PS_SETFLAGS`, `PS_POWERUP_COUNT`). One mismatch turns the feature off.
- No code site. It runs in the shared input tick and the post-input hook.
- Test-copy keys (never in the installer): `debug_probe_key`, `debug_probe_seconds`, `debug_drop_key`,
  `debug_drop_goodie`, `debug_walk_to_goodie`, `debug_walk_stop`, `debug_kill_seconds`, `debug_kill_range`,
  `debug_kill_stop`, `debug_marks`.
