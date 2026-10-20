// High-resolution rendering with art packs (docs/VISUALS.md).
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "render/renderer.h"

namespace bt {

// A folder with repainted level layers:
//   <pack>/pack.json                 {"scale": 4}
//   <pack>/levels/<ID>/fg.png        the foreground, scale times the 1x export
//   <pack>/levels/<ID>/bg.png        the background, likewise
//   <pack>/sprites/<key>.png         a sprite part, scale times its size
//                                    (key: Renderer::part_key, 16 hex digits)
//   <pack>/sprites/<key>.pal         the palette it was painted against (768
//                                    bytes of 6-bit RGB, from blackthorne --parts)
//   <pack>/hud/side.png, bottom.png  the HUD's right panel and bottom bar
//   <pack>/hud/icon_<nn>.png         item icon nn (hex; 00 is the empty slot)
//   <pack>/hud/selection.png         the selection frame
//   <pack>/hud/hud.pal               the palette the HUD was painted against
// Missing files (or files of the wrong size) fall back to the original art.
class ArtPack {
public:
    explicit ArtPack(std::filesystem::path dir);
    int scale() const { return scale_; }
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
    // A HUD image ("side", "bottom", "icon_05", "selection") of w x h game
    // pixels, or nullptr; hud_palette() is what it was painted against.
    const Image* hud(const std::string& name, int width, int height) const;
    bool has_hud() const { return has_hud_; }
    const std::array<std::uint8_t, 768>& hud_palette() const { return hud_palette_; }

private:
    std::filesystem::path dir_;
    int scale_ = 4;
    mutable std::map<std::pair<int, bool>, std::unique_ptr<Image>> cache_;  // nullptr = not in the pack
    bool has_sprites_ = false;
    bool has_hud_ = false;
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

private:
    void draw_hud(const Scene& scene, const Image& classic, const std::array<bool, 256>& cycled, Image& page) const;
    const Renderer& base_;
    const ArtPack* pack_;
    int scale_;
    // Buffers kept between frames.
    mutable Image classic_, play_, page_;
    mutable Renderer::Layers layers_;
    mutable std::vector<std::uint8_t> cells_;
    mutable std::vector<Renderer::Part> parts_;
};

}  // namespace bt
