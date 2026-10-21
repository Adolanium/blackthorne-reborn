# Visuals: where every pixel comes from

This is the starting point for making the game look better (higher-resolution
art, regenerated graphics, shaders) without breaking the faithful game
underneath.

## What drives the picture

The game logic never looks at pixels. `Game` advances the original's state
one frame at a time. `Game::display()` returns a `Scene`: the machine state
the original drew its last page from, plus the palette its VGA DAC was
showing. `Renderer::render_screen(scene)` turns that into a 320x240 ARGB
image. In widescreen it is 432x240. The display filters in
`src/game/main.cpp` then scale it.

Everything visible comes from these sources, all inside `Scene::machine`:

| What | Where the data is | Drawn by |
|---|---|---|
| Map tiles (background and foreground layers) | `Machine::tiles` (8x8 planar, 64 bytes each) and `tile_masks`. Each 16x16 cell is a metatile of four tile words: bits 6-15 are the tile, 4-5 flip, 3 "in front", 0-2 palette row | `Renderer::draw_layer` |
| Sprites (actors, effects) | Parts in the part pool (ds:452h onwards: flags, x, y, size, frame offset/segment, colour base). Frame pixels are in `Machine::frames` / `statics`, 9-byte rows `[mask][8 px]`, Mode X planar | `Renderer::draw_parts` |
| Text and dialogue boxes | The 32x28 text layer at ds:2D08h. Glyphs come from fonts 288/289 (290/291 for captions) | `Renderer::draw_text` |
| HUD | Side panel (DATA.DAT entry 73), bottom bar (74), item icons (resource 4Ch), selection frame (4Dh), item names from the font | `Renderer::draw_hud` |
| Colours | `Scene::palette`, 256 entries of 6-bit RGB. Pixels 0-15 take the tile row or part colour base; pixels 16-255 are absolute colours | `colour()` |

Palette effects are cycling, fades, tints and the dark menu. They change the
palette only, never the pixel indices. Keep that in mind for replacement
art: an indexed (palette-aware) replacement keeps these effects working for
free. A plain RGB replacement has to reapply them, for example by tinting
with the ratio between the current palette and the base palette at ds:4A12h.

## Art packs: repainted levels

The first enhanced path repaints each level as a whole: one painting for the
foreground layer and one for the background, at 4 times the original
resolution. `HdRenderer` (`src/render/hd.{h,cpp}`) draws them.

### Making a pack

1. Export the layers. `btexport` writes them for every level:

   ```
   btexport "<Blackthorne>/BTHORNE/DATA.DAT" out/export
   ```

   Each `out/export/levels/<ID>/` gets `fg.png` and `bg.png` (1 pixel per game
   pixel, transparent where nothing is drawn), `fg_front.png` and
   `bg_front.png` (the tiles drawn in front of the characters),
   `preview.png` and `level.json` (sizes, parallax, palette).
2. Repaint `fg.png` and `bg.png` at exactly 4x their size, with an image
   model or by hand. Keep every element where it is: ledges, ladders, walls
   and doors must line up with the game's collision. Keep transparency where
   the 1x image is transparent.
3. Put the results in `<pack>/levels/<ID>/fg.png` and `bg.png`, and a
   `<pack>/pack.json` with `{"scale": 4}`. A missing file, or one of the wrong
   size, falls back to the original art, so a pack can be partial.

Exports and packs come from the original art: keep them out of git (`out/`
and `packs/` are git-ignored).

### Repainting with an image model

A whole layer is too large for an image model (C0's foreground is 7168x896
at 4x), and a model won't return exact sizes or transparency.
`tools/artpack.py` handles both:

```
python tools/artpack.py split  out/export out/work --levels C0
# repaint every out/work/<ID>/<fg|bg>/in/<x>_<y>.png into .../out/<x>_<y>.png
python tools/artpack.py stitch out/export out/work packs/<name>
python tools/artpack.py check  out/export packs/<name>
```

- `split` cuts each layer into tiles of 256x224 game pixels (1024x896 at
  4x), overlapping by 32 pixels. Transparent parts are filled with magenta
  (`#ff00ff`). Fully transparent tiles are skipped.
- `stitch` resizes each repainted tile to its exact size, cross-fades the
  overlaps and takes the transparency from the original layer. So the
  painting covers what the original covered, and whatever the model painted
  over the magenta is dropped. Outlines are smoothed to within a fraction of
  a game pixel of the original's (`--edges hard` keeps the 1x shape exactly;
  `--alpha key` takes the outline from the painting itself, everything the
  model didn't leave magenta, for a restyle that changes shapes),
  and magenta the model blended into edges is painted over from neighbouring
  pixels. Tiles without a repaint keep the
  original pixels, so you can repaint a level a few tiles at a time.
- `nearest` writes the round-trip pack (the exports upscaled 4x with nearest
  neighbour).

What to ask of the model for each tile (with Codex, give it this and the
level's `preview.png` for context):

- Repaint the image as detailed, crisp, hand-painted dark-fantasy game art
  at the same size.
- Keep the composition exactly: every ledge, wall, ladder, door, pillar and
  vine in the same place, with the same silhouette and the same overall
  colours and brightness. Characters walk on these edges.
- Add no new objects and remove none. No text, no characters.
- The magenta areas are empty: paint them any way, they are cut out.
- Neighbouring tiles overlap, so keep the style and lighting consistent
  across a level: use the same instructions, and the previous tile as a
  style reference where the model accepts one.

### Using a pack

The game takes the pack from `pack=<folder>` in `settings.ini`, or else the
first folder with a `pack.json` in `packs/` under the settings folder
(`%APPDATA%/Adolanium/BlackthorneReborn/packs/`) or next to the game. The
command line option `--pack <folder>` overrides both. F6 switches between the
pack and the original art (saved as `hd=` in the settings). HD frames always
use sharp scaling: the Smooth and CRT filters are for the 320x240 picture.

### How the paintings are composed

`HdRenderer` keeps the classic renderer as the reference and changes only
where map pixels come from:

- **Layer order.** It draws the original's layer order at 4x: background,
  sprite priority 0, foreground, priorities 1 and 2, the "in front" tiles,
  priority 3, text. The original's 1x layers decide whether a spot belongs to
  the back or the front pass, so a painting needs no separate front layer.
  Cells nothing went over since the last full redraw show the full-redraw
  order, as in the classic renderer.
- **Fades and tints.** A painted pixel is scaled per channel by how much its
  original pixel's palette entry changed from the level's palette at load.
  Where the original is transparent, the overall change applies.
- **What keeps the original pixels.** Map cells the game changed since the
  load (doors, bridges), tiles that tile animations rewrote, colours in
  palette cycles (waterfalls, torches), and palette entries that grew out of
  black. These are upscaled from the original instead.
- **Everything else** (sprites without a replacement, text, HUD) is the
  original, upscaled. Sprite replacements are described below.
- **Smoothing.** Packs made by `stitch` set `smooth_text` and
  `smooth_fallback` in `pack.json`. Text, and everything that keeps the
  original pixels (cycled colours, animated tiles, changed cells, sprites
  without a replacement), is then rounded off with Scale2x twice instead of
  drawn as 4x blocks. The round-trip pack leaves both off.

A pack of the exports upscaled 4x with nearest neighbour must give exactly
the classic picture upscaled 4x. That is the round-trip check for
`HdRenderer` (see below).

The game keeps the load-time map, tiles and palette in `Machine`
(`map_at_load`, `tiles_at_load`, `tile_masks_at_load`, `palette_at_load`),
and save states store them.

## Sprites

Characters, enemies and effects are sprite parts: squares of 8 to 64
pixels. A pack replaces them one part at a time.

- **Key.** A part is keyed by a hash of its pixel data and colour base
  (`Renderer::part_key`). So a replacement follows the artwork wherever the
  game uses it, mirrored or not, in any level.
- **Files.** `<pack>/sprites/<key>.png` is the part at 4x, stored the way
  the data is (not mirrored). `<key>.pal` is the palette it was painted
  against.
- **Drawing.** The replacement is drawn inside the part's original shape.
  Each pixel is tinted by how its colour changed against `<key>.pal`, so
  fades, flashes and palette variants still work. Cycled colours keep the
  original pixels. Positions, priorities and order are the original's.
- **Collecting the parts.** The game only builds sprite frames while it
  plays, so they are collected by playing:

  ```
  python tools/artpack.py collect build/Release/blackthorne.exe "<Blackthorne>/BTHORNE/DATA.DAT" out/export
  python tools/artpack.py split-sprites  out/export out/work
  # repaint out/work/sprites/in/*.png into out/work/sprites/out/
  python tools/artpack.py stitch-sprites out/export out/work packs/<name>
  ```

  `collect` plays each level headless with random input
  (`blackthorne --parts`), and saves each part shown outside fades.
  `split-sprites` puts parts of one size on magenta sheets. Parts the
  random play never showed keep the original pixels.
- **Limits.** Some characters are built from several parts (a body and a
  head). Each part is repainted on its own, so keep the style consistent
  across a sheet.

## The HUD

`btexport` also writes the HUD to `<export>/hud/`: the right panel
(`side.png`), the bottom bar (`bottom.png`), the item icons (`icon_<nn>.png`,
`00` is the empty slot), the selection frame and `hud.pal`, the palette they
are drawn in. A pack's `hud/` holds the same files at 4x, plus `hud.pal`.
`HdRenderer` draws them in the original's places, tinted against
`hud.pal` like sprites. The item name keeps the original font.
`artpack.py split-hud` / `stitch-hud` prepare them for the model.

## Repainting everything with Codex

`tools/codex_repaint.py` runs Codex CLI with its image model over a whole
work folder: every level section, sprite sheet and HUD part without a
result yet. Jobs run in parallel and the run can be resumed. Results whose
painted shape strays too far from the original's are retried. Every job
gets the approved look as a style reference:

```
python tools/artpack.py split out/export out/work --tile 384x224
python tools/artpack.py split-sprites out/export out/work
python tools/artpack.py split-hud out/export out/work
python tools/codex_repaint.py out/work --style out/style --jobs 8
python tools/artpack.py stitch out/export out/work packs/<name> --alpha key
python tools/artpack.py stitch-sprites out/export out/work packs/<name>
python tools/artpack.py stitch-hud out/export out/work packs/<name>
```

`out/style/` holds `fg.png`, `bg.png` and `sprites.png`: repainted examples
that define the style.

## The scene renderer

`src/game/scene.{h,cpp}` draws the game as a scene of whole painted images
on the GPU, not by rebuilding the original's pixels:

- **Levels**: the background and foreground paintings drawn whole, with
  their own soft edges; an "in front" layer (the foreground painting where
  the original draws tiles in front of characters, with a soft edge) after
  the characters.
- **Characters**: whole painted poses (`<pack>/poses/`, from
  `artpack.py export-poses`), matched each frame by the same signature
  `blackthorne --poses` writes, placed from the game's part positions and
  flipped when facing left; their outlines are the painting's own. Parts
  not covered by a pose are drawn as painted parts.
- **Changed cells** (doors, bridges): painted metatiles copied into the
  level texture where the game changes the map.
- **Text and HUD**: painted glyphs and panels.
- Everything is drawn at the window's resolution with smooth scaling; fades
  are a colour modulation of everything.

- **Lighting**: the scene is drawn into a texture and shown through the
  lighting pass. `analyse()` looks at the game state at 1x each frame: fire
  (warm colours in palette cycles) and near-white effect sprites as light
  sources, text boxes as the UI mask.
- **Animated fire and water**: the game's colour cycles (waterfalls,
  torches, lava), softened along the fall of water, drawn over the
  painting, so the static paintings flow and flicker.
- **Smooth motion**: after each game tick the camera's and every actor's
  place are kept; frames between ticks place them part of the way from the
  previous tick (`interp`), so scrolling and movement are fluid at the
  display's rate while the game still steps at its own. Big jumps snap.

It is used when the pack has `poses/` (every character's whole poses);
`--legacy-hd` keeps the pixel-based HD renderer.

## Lighting and atmosphere

On Windows the game draws through SDL's GPU renderer (Direct3D 12) and runs a
lighting pass over the HD picture (F4 toggles it, `effects=` in
`settings.ini`, `--effects 0|1` on the command line, `--no-gpu` for the old
renderer):

- **Light sources come from the game.** While drawing, `HdRenderer` records
  an emission image: warm colours in palette cycles (torches, braziers,
  lava) and bright or cycled sprite pixels (muzzle flashes, explosions,
  glowing effects). `build_light_maps()` (`src/render/effects.cpp`) spreads
  it into a wide light map and a tight glow map.
- **The pass** (`src/render/shaders/light.frag.hlsl`, compiled to DXIL with
  the Windows SDK's `dxc` at build time) darkens each area to its ambient
  level (the mines and the fortress are dark, the forest dim, the desert
  bright), adds the flickering firelight and glow, a slow haze and a
  vignette. Text and dialogue boxes, and the HUD, stay as they are.
- Without Direct3D 12 or `dxc` (or with the classic art) the game draws as
  before.

- **Relief.** The pass reads the painting's shading as a height and the
  light map's slope as the direction light comes from, so surfaces facing
  a fire catch more of its light.
- **Embers** (`Embers`, `src/render/effects.cpp`) rise from every fire
  pixel at a steady rate, in world coordinates so they scroll with the
  level; hot yellow-white when fresh, cooling to the fire's colour. Drawn
  as soft additive glows after the lit frame.
- **Fog**: two noise layers that move with the camera at their own depth.

Next: light shafts, dust on landings, a colour grade per level, and SPIR-V
shaders so the pass runs on Vulkan too.

## Checking nothing broke

- Gameplay: `tools/emu/check.py` must still report 0 of 21 levels diverged.
  The renderer cannot change game state, but anything that touches
  `src/engine` must keep this.
- The classic look: `tools/emu/screencheck.py` must stay identical.
- The real game: `tools/playtest.py` runs every level headless, with the
  enhanced path switched on too.
- The HD path: make a round-trip pack (the exports upscaled 4x with nearest
  neighbour, plus `pack.json`). Then for any level and tick, the frame from
  `blackthorne --level <id> --ticks <n> --shot x.bmp --frame hd.png --pack <pack>`
  must equal the classic frame (`--pack` pointing to a folder without a
  pack) upscaled 4x with nearest neighbour, in normal and widescreen.
  `artpack.py nearest` includes the collected sprites, so the check covers
  sprite replacement too.
