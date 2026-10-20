// High-resolution rendering with art packs (docs/VISUALS.md).
#pragma once

#include <filesystem>
#include <map>
#include <memory>

#include "render/renderer.h"

namespace bt {

// A folder with repainted level layers:
//   <pack>/pack.json                 {"scale": 4}
//   <pack>/levels/<ID>/fg.png        the foreground, scale times the 1x export
//   <pack>/levels/<ID>/bg.png        the background, likewise
// Missing files (or files of the wrong size) fall back to the original art.
class ArtPack {
public:
    explicit ArtPack(std::filesystem::path dir);
    int scale() const { return scale_; }
    const std::filesystem::path& dir() const { return dir_; }
    // The repainted layer for a level, or nullptr. `width` / `height` are the
    // layer's size at 1x: the image must be exactly scale times that.
    const Image* layer(int level, bool background, int width, int height) const;

private:
    std::filesystem::path dir_;
    int scale_ = 4;
    mutable std::map<std::pair<int, bool>, std::unique_ptr<Image>> cache_;  // nullptr = not in the pack
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
    const Renderer& base_;
    const ArtPack* pack_;
    int scale_;
    // Buffers kept between frames.
    mutable Image classic_, play_, page_;
    mutable Renderer::Layers layers_;
    mutable std::vector<std::uint8_t> cells_;
};

}  // namespace bt
