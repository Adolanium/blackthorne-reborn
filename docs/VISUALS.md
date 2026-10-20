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
  painting covers exactly what the original covered, and whatever the model
  painted over the magenta is dropped. Tiles without a repaint keep the
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
- **Everything else** (sprites, text, HUD) is the original, upscaled.

A pack of the exports upscaled 4x with nearest neighbour must give exactly
the classic picture upscaled 4x. That is the round-trip check for
`HdRenderer` (see below).

The game keeps the load-time map, tiles and palette in `Machine`
(`map_at_load`, `tiles_at_load`, `tile_masks_at_load`, `palette_at_load`),
and save states store them.

## Next: sprites

- **Key replacements by content.** Key each sprite frame by a hash of its
  original pixels and size, plus the colour base. Then a replacement follows
  the artwork wherever the game uses it. Fall back to the original pixels
  when no replacement exists.
- **Positions stay the same.** Sprites are positioned from the same part
  x/y, so motion stays identical.
- **Getting the originals.** `btviewer` and `tools/sprites.py` can dump the
  original sprite frames. Keep the dumps in `extracted/` or `out/`
  (git-ignored).

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
