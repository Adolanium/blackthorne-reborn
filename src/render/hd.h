// High-resolution rendering with art packs (docs/VISUALS.md).
#pragma once

#include <array>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>

#include "render/renderer.h"

namespace bt {

// A folder with repainted level layers:
//   <pack>/pack.json                 {"scale": 4, "smooth_text": true}
//                                    (smooth_text: text in the original font
//                                    is rounded off with Scale2x twice;
//                                    smooth_fallback: so is everything drawn
//                                    from the original pixels)
//   <pack>/levels/<ID>/fg.png        the foreground, scale times the 1x export
//   <pack>/levels/<ID>/bg.png        the background, likewise
//   <pack>/sprites/<key>.png         a sprite part, scale times its size
//                                    (key: Renderer::part_key, 16 hex digits)
//   <pack>/sprites/<key>.pal         the palette it was painted against (768
//                                    bytes of 6-bit RGB, from blackthorne --parts)
//   <pack>/sprites/alias.tsv         "<key> <painted key>": parts with the same
//                                    pixel data drawn with another colour base;
//                                    drawn from that painting, recoloured
//   <pack>/hud/side.png, bottom.png  the HUD's right panel and bottom bar
//   <pack>/hud/icon_<nn>.png         item icon nn (hex; 00 is the empty slot)
//   <pack>/hud/selection.png         the selection frame
//   <pack>/hud/hud.pal               the palette the HUD was painted against
//   <pack>/font/<f>_<nnn>.png        glyph nnn (hex) of font f (0 normal,
//                                    1 captions), scale times 8x8
//   <pack>/font/font.pal             the palette the glyphs were painted against
//   <pack>/cells/<ID>_<mmm>.png      metatile mmm (hex) of level ID painted
//                                    alone (scale times 16x16), drawn where
//                                    the game changed a map cell to it
//   <pack>/cells/<ID>_k<key>.png     a cell showing animated tiles, by
//                                    Renderer::metatile_key (16 hex digits)
// Missing files (or files of the wrong size) fall back to the original art.
class ArtPack {
public:
    // background_loading: level layers decode on another thread; until one is
    // ready, layer() returns nullptr (the original art is drawn). Only the
    // current level's layers stay in memory either way.
    explicit ArtPack(std::filesystem::path dir, bool background_loading = false);
    int scale() const { return scale_; }
    bool smooth_text() const { return smooth_text_; }
    bool smooth_fallback() const { return smooth_fallback_; }
    const std::filesystem::path& dir() const { return dir_; }
    // The repainted layer for a level, or nullptr. `width` / `height` are the
    // layer's size at 1x: the image must be exactly scale times that.
    const Image* layer(int level, bool background, int width, int height) const;
    // The replacement for a sprite part of `size` pixels square, or nullptr.
    struct Sprite {
        Image image;
        std::array<std::uint8_t, 768> palette{};  // what it was painted against
    };
    const Sprite* sprite(std::uint64_t key, int size) const;
    // The painted part a key is an alias of (same pixels, other colour base), or 0.
    std::uint64_t alias(std::uint64_t key) const;
    // A HUD image ("side", "bottom", "icon_05", "selection") of w x h game
    // pixels, or nullptr; hud_palette() is what it was painted against.
    const Image* hud(const std::string& name, int width, int height) const;
    bool has_hud() const { return has_hud_; }
    // A painted metatile of a level, or nullptr; anim_cell: a painted state
    // of an animated cell, by Renderer::metatile_key.
    const Image* cell(int level, int metatile) const;
    const Image* anim_cell(int level, std::uint64_t key) const;
    // A painted glyph of font 0 or 1, or nullptr.
    const Image* glyph(int font, int index) const;
    bool has_font() const { return has_font_; }
    const std::array<std::uint8_t, 768>& font_palette() const { return font_palette_; }
    const std::array<std::uint8_t, 768>& hud_palette() const { return hud_palette_; }

private:
    std::filesystem::path dir_;
    int scale_ = 4;
    bool smooth_text_ = false, smooth_fallback_ = false;
    bool background_loading_ = false;
    mutable int cached_level_ = -1;
    mutable std::map<std::pair<int, bool>, std::shared_future<std::shared_ptr<Image>>> cache_;  // null = not in the pack
    bool has_sprites_ = false;
    std::map<std::uint64_t, std::uint64_t> aliases_;
    bool has_hud_ = false, has_font_ = false, has_cells_ = false;
    mutable std::map<int, std::unique_ptr<Image>> cells_;  // level * 1024 + metatile; nullptr = not in the pack
    mutable std::map<std::pair<int, std::uint64_t>, std::unique_ptr<Image>> anim_cells_;
    std::array<std::uint8_t, 768> font_palette_{};
    mutable std::map<int, std::unique_ptr<Image>> glyphs_;  // font * 4096 + index; nullptr = not in the pack
    std::array<std::uint8_t, 768> hud_palette_{};
    mutable std::map<std::string, std::unique_ptr<Image>> hud_;  // nullptr = not in the pack
    mutable std::map<std::uint64_t, std::unique_ptr<Sprite>> sprites_;  // nullptr = not in the pack
};

// Draws the screen at `scale` times 320x240 (wider in widescreen): the
// original's layers in its order, with the map layers from the pack where it
// has them, and everything else upscaled from the original pixels.
class HdRenderer {
public:
    HdRenderer(const Renderer& base, const ArtPack* pack, int scale) : base_(base), pack_(pack), scale_(scale) {}
    int scale() const { return scale_; }
    void render_screen(const Scene& scene, Image& out, const RenderOptions& opt = {}) const;

    // Where the last frame's game pixels came from: the pack's paintings or
    // the original art (map, sprites, text, HUD; empty screen not counted).
    struct Coverage {
        long painted = 0, original = 0;
        long original_map = 0, original_sprites = 0, original_text = 0, original_hud = 0;  // original, by kind
    };
    const Coverage& coverage() const { return cov_; }

    // What gives off light in the last frame, 1 pixel per game pixel of the
    // screen (fire and lava from palette cycles, bright effect sprites);
    // transparent elsewhere. And the playfield's rect on the screen.
    const Image& emission() const { return emission_; }
    // Where text and dialogue boxes are (opaque white), same size: kept unlit.
    const Image& ui_mask() const { return ui_; }
    int playfield_x() const { return play_x_; }
    int view_x() const { return layers_.view_x; }  // the world position the playfield shows (game pixels)
    int view_y() const { return layers_.cam_y; }
    int playfield_w() const { return play_w_; }

private:
    void draw_hud(const Scene& scene, const Image& classic, const std::array<bool, 256>& cycled, Image& page) const;
    // A painted glyph at (x, y) in game pixels of `out` (the playfield or the
    // page), tinted against the font palette; false if the pack has none.
    bool draw_glyph(const Scene& scene, int font, int index, int x, int y, Image& out) const;
    const Renderer& base_;
    const ArtPack* pack_;
    int scale_;
    // Buffers kept between frames.
    mutable Image classic_, play_, page_, text1_, text2_, text4_;
    mutable Image smooth2_, smooth4_[4], part1_, part2_, part4_;  // smooth_fallback
    mutable Renderer::Layers layers_;
    mutable std::vector<std::uint8_t> cells_;
    mutable std::vector<Renderer::Part> parts_;
    mutable Coverage cov_;
    mutable Image emission_, ui_;
    mutable int play_x_ = 0, play_w_ = 0;
    mutable std::map<int, std::uint64_t> anim_keys_;  // metatile -> key, this frame
    mutable std::map<int, std::array<bool, 256>> anim_masks_;  // metatile -> pixels its animation changed
    // Where each metatile first appears in each layer at load (for cells
    // changed to a metatile the painting already shows elsewhere).
    mutable int where_level_ = -1;
    mutable std::map<int, std::pair<int, int>> where_[2];  // [background] metatile -> cell
};

}  // namespace bt
