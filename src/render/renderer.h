#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "data/archive.h"
#include "data/sprites.h"
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

    // The whole 320x240 screen: playfield, right inventory panel and bottom
    // bar (for levels with a HUD; other levels show the playfield centred).
    static constexpr int kScreenWidth = 320, kScreenHeight = 240;
    void render_screen(const Game& game, Image& out, const RenderOptions& opt = {}) const;

private:
    // A map layer: cells at `layer_offset` in the map segment, `row_bytes` per row,
    // `width` x `height` cells, viewed from (cam_x, cam_y).
    void draw_layer(const Game& game, std::uint16_t layer_offset, int row_bytes, int width, int height, int cam_x, int cam_y,
                    Image& out) const;
    void draw_parts(const Game& game, Image& out) const;
    void draw_text(const Game& game, Image& out) const;

    void draw_hud(const Game& game, Image& out) const;
    void stamp(const Game& game, Image& out, const std::vector<std::uint8_t>& pixels, int size, int x, int y,
               std::uint8_t colour_base) const;

    struct Panel {
        int width = 0, height = 0;
        std::vector<std::uint8_t> pixels;
    };
    Panel side_panel_, bottom_bar_;
    std::vector<std::vector<std::uint8_t>> item_icons_;  // 32x32, resource 0x4C
    std::vector<std::uint8_t> selection_frame_;         // 32x32, resource 0x4D frame 0

    struct Glyph {
        std::array<std::uint8_t, 64> pixels;  // values 0-3 = palette entries 0-3
        std::array<std::uint8_t, 8> mask;     // one bit per data byte, planar order
    };
    std::vector<Glyph> fonts_[2];  // entries 288/289 and the alternate 290/291
};

}  // namespace bt
