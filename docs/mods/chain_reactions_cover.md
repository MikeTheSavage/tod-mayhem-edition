# Chain reactions and breakable cover (`[mayhem]`)

## What it does

**Chain reactions:** an explosion sets off the explosive objects and the cars around it, each after a short delay
(nearer ones first), so a cluster of tanks ripples instead of popping in one frame. **Breakable cover:** crates,
tables, stalls, carts, benches and bins break sooner under fire and burst into pieces that fly; enemies crouched behind
a broken prop stand up.

## How it is implemented

1. **Engine facts.**
   - Destroyable_Object (handler 0x6691A0): `take_a_hit` (215) -> 0x670C70 -> Destroyable_Objects_AI_Ctrl 0x678050
     (DamageTaken +BCh vs Health +114h) -> IsDead +144h -> `command_destroy_object` 0x671410 (explosion call 0x668BA0
     when IsExplodeable +7Ch) -> dying thread 0x66C1C0 -> `do_explosion` 0x66A600.
   - `do_explosion` queries the quadtree (mask 440h) within ExplosiveRange (+118h, 4-5 m) and sends `take_a_hit`
     (ExplosiveDamage +11Ch, type 1) to every entity at 0x66BCDF, all in the same frame.
   - Cars: vehicle_ai's explode state 0x809910 calls its explosion 0x806860 at 0x80A756 and 0x80A8EC.
   - Destroyables are a pool of about 11 resources handed to the nearest placeholders; far objects do not exist yet.
     Exploded barrels keep IsDead 0 and Health 0, so a chain must remember what already blew up.
   - TOD has no cover points. Enemies crouch by their type's odds at their placements; "cover" is just a prop between
     Ram and an enemy, made of collision.
2. **Sites** (all CALL retargets):
   - MH_DO_QUERY 0x66A6EE: do_explosion's range query; the reliable "a destroyable exploded" event.
   - MH_DO_HIT 0x66BCDF: do_explosion's instant take_a_hit on an explosive neighbour is held back and scheduled as a
     chain link; everything else gets the original call.
   - MH_CAR_A 0x80A756, MH_CAR_B 0x80A8EC: a car's explosion is queued as a blast.
   - MH_DRAW 0x421C71: the frame-end motion blur call in GfxInternal::Render; the debris is drawn right before it.
   - PH_SHELL / PH_DESTROY (from [physics.md](physics.md)) also report their blasts.
3. **Chain schedule** (input tick). For each queued blast, query `chain_radius` (mask 440h) and schedule every live
   explosive destroyable and every car (not the one Ram is in) at now + delay, delay = min + (max - min) x
   (0.6 x distance / radius + 0.4 x random), in game time. A due link is checked again (same node, class, place, still
   alive) and gets `take_a_hit` {`chain_damage`, the first blast's owner, type 1}. The game's own explosion, fire,
   sound, damage and score follow, and its blast is the next link. Limits: `chain_max_depth`, `chain_max_per_second`,
   `chain_max_per_frame`, 32 pending, no node twice (60 s memory). Rewind drops pending links.
4. **Breakable cover.** Every 0.5 s the destroyables within 45 m whose names match `cover_props` get Health x
   `cover_health_scale` (no notify: never saved). When one dies: debris pieces from its collision box, its broken model
   hidden for 2 s (node flag 1 = no draw, no collision, so bullets pass), and enemies within `expose_radius` get
   `command_crouch_out` (1054). Blasts also hit cover props within `cover_blast_radius`.
5. **Debris.** Box3D bodies in the physics world (category 2). At most `debris_max`, each lives `debris_life` s and
   shrinks away. Drawn by `mh_draw.cpp` as textured boxes with the game's device, depth-tested in the world pass,
   under the HUD, with every touched state restored.
6. **Debris look, style 1** (the default). The first version threw blast shards that read as large red bricks (6 per
   exploding destroyable, 0.35 x 0.5 m boxes cut into slats), and every chained or unnamed explosive got a procedural
   red paint. Style 1 replaces them:
   - **Chunks**: `debris_pieces` (cover) / `blast_debris` (blasts) Box3D bodies, longest edge 5..16 cm for cover and
     3..11 cm for blasts (x `debris_size`), most near the small end, never longer than the prop. Wood breaks into
     splinters, metal into plates, the rest into lumps. No body is thinner than 2.4 cm (Box3D). Pieces shrink away
     inside 1.8 m of the camera (gone at 0.5 m), so a chunk of the cover the player hides behind never fills the screen.
   - **Grit**: `debris_bits` tiny bits per break (blasts 1.3x), plain particles without a physics body: they fly,
     bounce on the ground found by `raycast_static` at their predicted landing point, settle and shrink away.
   - **Dust**: with `debris_dust`, 5 soft camera-facing puffs per broken cover prop in the prop's mean colour mixed with
     a dust beige; alpha blended, no depth write, drawn after the pieces, each card pulled 0.6 radius toward the camera
     so it does not cut a hard edge into the prop it came from. Blasts get none (the game's own fire and smoke).
   - **The prop's own texture**: the Model node's ModelAsset (+50h) TextureSets (+20h, 8-byte {TextureAsset*, index},
     count +24h) -> TextureAsset +28h & ~3 = Texture -> +4 & ~3 = IDirect3DTexture9 (REF `MH_TEXSETS` 0x856F42 in
     ModelAsset::GetPivotMeshTexture 0x856F10, REF `MH_D3DTEX` 0x4634A0 Texture::GetDirect3DTexture). The largest set
     is taken; it must have the vtable of a texture this module made on the same device, so a wrapped or foreign
     object is never called. Its smallest mip <= 64 x 64 is read back once (DXT1-5, A8R8G8B8, X8R8G8B8, R8G8B8,
     R5G6B5, X1R5G5B5 / A1R5G5B5, A4R4G4B4) for the mean colour and a 16 x 16 map of the used (opaque, not black)
     cells. Every piece shows its own patch of the texture from a used cell, sized by the piece over the prop, and holds
     one reference (released when it goes, at a rescan or a level change). No texture found: procedural wood / metal,
     and charred metal instead of red for explosives.
   - `debris_style = 0` keeps the first version (slats, red shards) for comparison.
7. Needs `[physics] enabled = 1` and TODPhysics.dll; without them nothing is patched.

## Mission safety

- A chain link (barrel, tank or car) is never set off with a buddy / escort / friend within its blast (6 m, for a car
  `car_blast_radius` + 2 m). Without this rule a chained car in MM055 exploded and killed two DEA buddies at once. Log:
  `chain: ... not set off: a buddy / escort / friend ... is within its blast`.
- `chain_cars` defaults to 0 (code, sample ini, installer): a chained car takes a 5000-damage hit, and several missions
  fail when one of their cars is destroyed (MM032 the getaway car, MM040 / MM050 the trucks).

## Ini keys (`[mayhem]`)

| Key | Default | Meaning |
|---|---|---|
| `chain_reactions` | 0 | the chain feature |
| `chain_radius` | 9 | m around an explosion |
| `chain_delay_min` / `chain_delay_max` | 0.15 / 0.6 | link delay (game seconds) |
| `chain_max_per_second` / `chain_max_per_frame` / `chain_max_depth` | 6 / 2 / 12 | limits |
| `chain_cars` | 0 | cars in reach take the hit too (off by default: mission cars) |
| `car_blast_radius` / `car_blast_impulse` | 8 / 450 | Box3D blast of an exploding car |
| `blast_debris` | 6 | shards per exploding barrel or tank |
| `breakable_cover` | 0 | the cover feature |
| `cover_health_scale` | 0.35 | about three times fewer hits to break |
| `debris_max` / `debris_life` / `debris_pieces` / `debris_impulse` | 48 / 12 / 8 / 1.0 | debris |
| `debris_style` | 1 | 1 = small chunks, grit and dust in the prop's own texture; 0 = the first version (slats, red shards) |
| `debris_size` / `debris_bits` / `debris_dust` | 1.0 / 14 / 1 | style 1: chunk size factor, grit per break, dust puffs |
| `expose_radius` / `cover_blast_radius` | 2.6 / 6 | stand-up reach, blast reach on cover |
| `cover_props` / `cover_exclude` | lists | which destroyables count as cover |
| `log_seconds` | 5 | statistics |

Test-only: `debug_survey`, `debug_survey_key`, `debug_ignite_key`, `debug_hit_key`, `debug_place_ph`,
`debug_place_cover`, `debug_shots`.

## Testing and limits

- Unit tests `mayhem/code/test_mh.cpp`; host test 22.
- Checked in game on a test copy of the game with The Ambush save: three pressure tanks 6 m
  apart (beyond their 4 m range). Vanilla: one blast. With chains: the next two go 0.69 s and 0.68 s later. Crates
  break into planks.
- Cost: tick 0.004 to 0.02 ms per frame when idle, worst frame 6.2 ms (a blast spawning about 22 debris bodies).
- Debris style 1 was compared with style 0 in the same Ambush runs. Style 0 throws 6 shards per tank, slats and grey
  blocks up to the 0.35 x 0.5 m box. Style 1 throws 6 chunks of 3 to 11 cm in the tank's own texture
  (`SteelRusty_Gradient.bmp`, `PropaneGreen.BMP`) plus 18 grit bits, and the crates that break next to them give
  8 chunks of 5 to 16 cm in `SteelBox.bmp`, 14 grit and a grey dust puff. Every model was found by the quadtree query
  and every texture was read back (256/256 cells used). Drawing costs 0.04 to 0.07 ms per frame on average, 0.5 ms
  worst, with up to 116 pieces plus grit alive. A burst costs 0.12 to 0.18 ms. The debris textures are made at the
  first world frame, so the first burst does not pay for them (7 ms when it did). The big flat panels that fly past
  the camera when a crate breaks are the game's own wreck pieces (they appear with style 0 too); style 1 pieces shrink
  away inside 1.8 m of the camera.
- **Not verified:** debris style 1 on other maps and props (only the Ambush tanks, crates and the debug cover were
  seen; a prop whose texture cannot be read falls back to procedural wood / metal / charred, logged); an enemy
  standing up behind broken cover (no enemy was within 2.6 m in the runs); cars as chain links (host-tested only);
  long sessions; save and load with debris alive (a wreck saved within 2 s of a break stays hidden).
- A chain cannot reach an object the pool has not allocated yet.
- EXPERIMENTAL in the installer.

## Files

`mayhem/code/mh_game.cpp`, `mh_logic.cpp/.h`, `mh_draw.cpp`, `test_mh.cpp`, `mayhem/README.md`,
`dll/HOOKS.md` "Mayhem props".
