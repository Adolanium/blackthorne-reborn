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
    // Widescreen: extra columns on each side of the 256-wide playfield. They
    // show the neighbouring screens (dimmed when dim_sides is set).
    int extra = 0;
    bool dim_sides = true;
    bool no_shake = false;  // render_screen without the screen shake
};

// The 432x240 widescreen layout: 56 extra columns per side.
constexpr int kWideExtra = 56;

// What one frame shows: the machine state the picture is drawn from and the
// palette it is shown through (6-bit RGB triples). Game::display() is what
// the original's screen shows; live_scene() is the current state through the
// shown palette (ds:4D12h), for tools.
Scene live_scene(const Game& game);

// Draws the game's 256x224 playfield the way the original composes it: map
// layers from the live map segment, sprite parts from the part pool, the text
// layer on top, all through the current shown palette (ds:4D12h).
class Renderer {
public:
    static constexpr int kWidth = 256, kHeight = 224;

    explicit Renderer(const Archive& archive);

    // The playfield, kWidth + 2 * opt.extra wide.
    void render(const Scene& scene, Image& out, const RenderOptions& opt = {}) const;

    // The whole 320x240 screen: playfield, right inventory panel and bottom
    // bar (for levels with a HUD; other levels show the playfield centred).
    // opt.extra widens it by 2 * extra.
    static constexpr int kScreenWidth = 320, kScreenHeight = 240;
    void render_screen(const Scene& scene, Image& out, const RenderOptions& opt = {}) const;
    void render_screen(const Game& game, Image& out, const RenderOptions& opt = {}) const;  // game.display()

    // A whole map layer (the background or the foreground) at 1x, transparent
    // where no tile pixel is set. pass: 0 = tiles without bit 3, 8 = tiles
    // with it (drawn in front of sprites), -1 = both.
    void render_layer(const Scene& scene, bool background, int pass, Image& out) const;

    // The playfield split into the layers the original composes, at 1x, for
    // renderers that compose them differently (src/render/hd.h). The map
    // layers hold palette indices (decode with layer_index()); sprites and
    // text hold colours. Transparent pixels have alpha 0.
    struct Layers {
        Image background_back, background_front, foreground_back, foreground_front;  // indices
        Image sprites[4];  // by priority
        Image text;
        int view_x = 0, cam_y = 0, bg_x = 0, bg_y = 0;  // map coordinates of the top left
        int shift = 0;  // where the 256-wide game screen starts in the view
    };
    void render_layers(const Scene& scene, const RenderOptions& opt, Layers& out) const;
    static int layer_index(std::uint32_t pixel);  // -1 if transparent
    static std::uint32_t index_pixel(int index);  // the encoding layer_index() reads

    // A sprite part as the original draws it: its square's top left in map
    // coordinates, size, horizontal mirroring, colour base and pixel data
    // (size * size / 8 runs of [mask][8 pixels], Mode X planar).
    struct Part {
        int x = 0, y = 0, size = 0;
        bool mirror = false;
        std::uint8_t base = 0;
        const std::uint8_t* data = nullptr;
    };
    // The parts of one priority, in drawing order (later ones on top).
    static void parts(const Scene& scene, int priority, std::vector<Part>& out);
    // The palette index of a part's pixel (x, y) as shown, mirroring
    // applied; -1 where transparent.
    static int part_pixel(const Part& part, int x, int y);
    // The key for replacement art: a hash of the pixel data and colour base.
    static std::uint64_t part_key(const Part& part);

    // The HUD's artwork as palette indices: the right panel (64x240 at
    // x = 256), the bottom bar (256x16 at y = 224), the 32x32 item icons
    // (0 is the empty slot, stamped with colour base 0x80, the others with
    // 0xF0) and the selection frame (0xF0). Icon pixels below 16 take the
    // colour base; 0 is transparent.
    struct Panel {
        int width = 0, height = 0;
        std::vector<std::uint8_t> pixels;
    };
    const Panel& side_panel() const { return side_panel_; }
    const Panel& bottom_bar() const { return bottom_bar_; }
    const std::vector<std::vector<std::uint8_t>>& item_icons() const { return item_icons_; }
    const std::vector<std::uint8_t>& selection_frame() const { return selection_frame_; }

private:
    // A map layer: cells at `layer_offset` in the map segment, `row_bytes` per row,
    // `width` x `height` cells, viewed from (cam_x, cam_y).
    void draw_layer(const Scene& scene, std::uint16_t layer_offset, int row_bytes, int width, int height, int cam_x, int cam_y,
                    int pass, Image& out) const;
    void draw_parts(const Scene& scene, int cam_x, int cam_y, int priority, Image& out) const;
    void draw_text(const Scene& scene, int x0, const Image* tiles, Image& out) const;

    void draw_hud(const Scene& scene, Image& out) const;
    void stamp(const Scene& scene, Image& out, const std::vector<std::uint8_t>& pixels, int size, int x, int y,
               std::uint8_t colour_base) const;

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
