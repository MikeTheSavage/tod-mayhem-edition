# HD HUD icons (data option `hud_textures`)

New high-resolution art for the game's own HUD textures, written into `overdose.main` by the data builder:
15 pickup / power-up coins, 12 ammo icons and the bullet tick, the 28 weapon selector cards and the combo timer strip.
Nothing in the DLL; the game draws the new textures with its own sprites, model and animations.

| group | textures | original | new | format | payload each |
|---|---|---|---|---|---:|
| pickups | LocoMove_* (9), OneShot_* (3), Permanent_* (3) | 32 x 32 | 256 x 256 | RGBA32, 9 mips | 349,524 B |
| ammo | BulletIcon_* (12) | 32 x 32 | 256 x 256 | RGBA32, 9 mips | 349,524 B |
| ammo | BulletLine_Bullet | 8 x 16 | 128 x 256 | RGBA32, 9 mips | 174,764 B |
| weapons | the 28 selector cards | 128 x 64 | 512 x 256 | RGBA32, 10 mips | 699,052 B |
| combo | hud_element_combotimer | 64 x 256 | 128 x 512 | RGBA32, 10 mips | 349,532 B |

Total 29.5 MB more in `overdose.main` (14.3 MB -> 42.4 MB alone; the zip grows by the same). Every texture is at most
the engine's per-texture cap (1,351,384 B), a power-of-two multiple of the original with the same aspect, RGBA32 (soft
alpha would not survive DXT1). The sizes are the ones TOD HD's HUD set already loaded in game; the combo strip is the
largest RGBA32 of its shape under the cap.

## Files

- `textures.json`: the list, one row per game texture: source PNG (relative to the art folder, `work\hud_art_gen\v2` by default; the source art is not in the repo),
  clean-up step, new size, `enabled`. To drop a texture set `enabled` to false and re-run the converter (or flip
  `enabled` in `art/manifest.json`; the builder reads only the manifest).
- `make_hud_textures.py` (developer step, Pillow + numpy): PNG -> `art/<name>.bin.z` (zlib A8R8G8B8 mip chain) +
  `art/manifest.json` (size, levels, sha256, placement). `--sheets` also writes comparison sheets
  (`proof/offline_*.png`, each new texture next to the original, plus the combo checks; not kept in the repo).
- `art/`: what ships. `data/gameplay/hud_textures.py` (stdlib) puts it into the block on the player's machine through
  `data/lib/todtexture.py` (the TOD HD texture writer: the record's size / level fields patched in place, the payload
  swapped, everything else byte-identical and re-checked).

```
python -I hud_textures\make_hud_textures.py --sheets
cd data && python build_gameplay_mod.py --game "C:\GOG Games\Total Overdose" --set hud_textures=on --out ..\work\ht\todgameplay_data.zip
python -m unittest test_hud_textures      (in tests\data)
```

## Placement

- Icons and cards: the art's content box (alpha > 16) is fitted (contain, centred) into the ORIGINAL texture's content
  box scaled to the new size, so every sprite keeps its on-screen size and position. Solid alpha in the generated art is
  249-253; 248 and above becomes 255 (otherwise every icon is 2 % see-through).
- Mips: premultiplied LANCZOS to level 0, premultiplied 2 x 2 box averages below, push-pull fill colour under alpha 0
  (no dark fringes when the game filters or shrinks an icon).
- The combo strip is not a picture of the disc. The model is a disc of 64 wedges whose u runs rim -> centre and whose v
  runs with the angle; the timer animation slides v so the transparent upper half of the strip comes in. The final
  art is drawn in the layout of `work/hud_art_gen/ref/combo_disc_ref.png`, which is the original strip shown as a
  semicircle (v 0.5 -> 1 from 9 to 3 o'clock, u = 1 - r; that mapping reproduces the ref to 1.75 grey levels). The
  converter inverts exactly that mapping (16 samples per texel, the art's alpha-128 rim put on the original's
  alpha-128 column), keeps the upper half transparent black and leaves colour 0 under alpha 0 (the disc is additive).
  `--sheets` checks this: one sheet shows the new strip re-wrapped next to the ref, another the original and the new
  strip through the model's own UVs (a skull).
