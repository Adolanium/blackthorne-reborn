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

## Suggested shape for enhanced art

- **Keep the classic path.** Leave `Renderer` as the reference.
  `screencheck.py` keeps it identical to the original. Add an enhanced
  renderer next to it and choose between them in settings, the way
  widescreen and the filters work.
- **Key replacements by content.** Key each image by a hash of the original
  pixels: an 8x8 tile's 64 bytes plus its mask, or a sprite frame's pixels
  and size. Then a replacement follows the artwork wherever the game uses
  it, across levels, flips and palette rows. Fall back to the original
  pixels when no replacement exists, so packs can be partial.
- **Draw at a higher scale.** Draw replacements at k times the resolution
  (for example 4x: 1280x960) into the same layer order the classic renderer
  uses: background, sprite priorities, bit-3 tiles, text, HUD. Sprites are
  positioned from the same part x/y, so motion stays identical.
- **Store packs outside the repository.** Packs are derived from the
  original art: never commit them. Load them from the settings folder (for
  example `%APPDATA%/Adolanium/BlackthorneReborn/packs/<name>/`) or a
  git-ignored `packs/` folder.
- **Getting the originals.** `btviewer` and `tools/sprites.py` can dump the
  original tiles and sprite frames to feed an image model. Keep the dumps in
  `extracted/` or `out/` (git-ignored).

## Checking nothing broke

- Gameplay: `tools/emu/check.py` must still report 0 of 21 levels diverged.
  The renderer cannot change game state, but anything that touches
  `src/engine` must keep this.
- The classic look: `tools/emu/screencheck.py` must stay identical.
- The real game: `tools/playtest.py` runs every level headless, with the
  enhanced path switched on too.
