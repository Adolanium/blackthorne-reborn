#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "data/archive.h"
#include "engine/game.h"

namespace bt {

// An ARGB image.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<std::size_t>(w) * h, 0xFF000000u);
    }
};

struct RenderOptions {
    bool background = true;
    bool foreground = true;
    bool sprites = true;
    bool text = true;
};

// Draws the game's 256x224 playfield the way the original composes it: map
// layers from the live map segment, sprite parts from the part pool, the text
// layer on top, all through the current shown palette (ds:4D12h).
class Renderer {
public:
    static constexpr int kWidth = 256, kHeight = 224;

    explicit Renderer(const Archive& archive);

    void render(const Game& game, Image& out, const RenderOptions& opt = {}) const;

private:
    void draw_layer(const Game& game, std::uint16_t layer_offset, Image& out) const;
    void draw_parts(const Game& game, Image& out) const;
    void draw_text(const Game& game, Image& out) const;

    struct Glyph {
        std::array<std::uint8_t, 64> pixels;  // values 0-3 = palette entries 0-3
        std::array<std::uint8_t, 8> mask;     // one bit per data byte, planar order
    };
    std::vector<Glyph> fonts_[2];  // entries 288/289 and the alternate 290/291
};

}  // namespace bt
