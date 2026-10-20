#include "render/renderer.h"

#include <algorithm>

namespace bt {
namespace {

constexpr std::uint16_t kShownPalette = 0x4D12;

std::uint32_t colour(const DataSegment& ds, int index) {
    auto c = [&](int k) {
        const int v = ds.u8(static_cast<std::uint16_t>(kShownPalette + index * 3 + k)) & 0x3F;
        return static_cast<std::uint32_t>(v * 255 / 63);
    };
    return 0xFF000000u | (c(0) << 16) | (c(1) << 8) | c(2);
}

}  // namespace

Renderer::Renderer(const Archive& archive) {
    for (int f = 0; f < 2; ++f) {
        const Bytes font = archive.entry(288 + 2 * f);
        const Bytes masks = archive.entry(289 + 2 * f);
        for (std::size_t o = 0, i = 0; o + 64 <= font.size(); o += 64, ++i) {
            Glyph g{};
            for (int plane = 0; plane < 4; ++plane)
                for (int y = 0; y < 8; ++y)
                    for (int col = 0; col < 2; ++col) g.pixels[y * 8 + col * 4 + plane] = font[o + plane * 16 + y * 2 + col];
            for (int k = 0; k < 8 && i * 8 + k < masks.size(); ++k) g.mask[k] = masks[i * 8 + k];
            fonts_[f].push_back(g);
        }
    }
}

void Renderer::render(const Game& game, Image& out, const RenderOptions& opt) const {
    out.resize(kWidth, kHeight);
    if (opt.background && game.has_background()) draw_layer(game, game.background_offset(), out);
    if (opt.foreground) draw_layer(game, game.foreground_offset(), out);
    if (opt.sprites) draw_parts(game, out);
    if (opt.text) draw_text(game, out);
}

// One map layer (cells: bits 0-9 metatile) over the camera window.
void Renderer::draw_layer(const Game& game, std::uint16_t layer_offset, Image& out) const {
    const Machine& m = game.machine();
    const DataSegment& ds = m.ds;
    const Level& lv = game.level();
    const int cam_x = static_cast<std::int16_t>(ds.u16(0x1B4E)), cam_y = static_cast<std::int16_t>(ds.u16(0x1B50));
    const int map_w = ds.u16(Machine::kMapWidth), map_h = ds.u16(Machine::kMapHeight);
    const std::uint16_t row_bytes = ds.u16(Machine::kMapRowBytes);
    const std::uint16_t metatiles = ds.u16(Machine::kMetatileTable);
    for (int my = cam_y >> 4; my <= (cam_y + kHeight) >> 4; ++my) {
        for (int mx = cam_x >> 4; mx <= (cam_x + kWidth) >> 4; ++mx) {
            if (mx < 0 || my < 0 || mx >= map_w || my >= map_h) continue;
            const std::uint16_t cell = m.map_u16(static_cast<std::uint16_t>(layer_offset + my * row_bytes + mx * 2));
            const std::uint16_t mt = static_cast<std::uint16_t>(metatiles + (cell & 0x3FF) * 8);
            for (int k = 0; k < 4; ++k) {
                const TileRef ref{m.map_u16(static_cast<std::uint16_t>(mt + 2 * k))};
                if (ref.tile() >= static_cast<int>(lv.tiles.size())) continue;
                const Tile& t = lv.tiles[ref.tile()];
                const int ox = mx * 16 + (k & 1) * 8 - cam_x, oy = my * 16 + (k >> 1) * 8 - cam_y;
                for (int y = 0; y < 8; ++y) {
                    const int sy = oy + y;
                    if (sy < 0 || sy >= kHeight) continue;
                    for (int x = 0; x < 8; ++x) {
                        const int sx = ox + x;
                        if (sx < 0 || sx >= kWidth) continue;
                        const int v = t[(ref.flip_y() ? 7 - y : y) * 8 + (ref.flip_x() ? 7 - x : x)];
                        if (!v) continue;
                        out.pixels[static_cast<std::size_t>(sy) * kWidth + sx] = colour(ds, v >= 16 ? v : v + ref.palette_row() * 16);
                    }
                }
            }
        }
    }
}

// Sprite parts: pool entries with attribute bit 0x80 (allocated) and without
// 0x40 (hidden). Pixel data is rows of [mask][8 pixels]; the pixel bytes form
// the same Mode X planar square as the sprite banks.
void Renderer::draw_parts(const Game& game, Image& out) const {
    const Machine& m = game.machine();
    const DataSegment& ds = m.ds;
    const int cam_x = static_cast<std::int16_t>(ds.u16(0x1B4E)), cam_y = static_cast<std::int16_t>(ds.u16(0x1B50));
    struct Item { int priority, p; };
    std::vector<Item> items;
    for (int p = 0; p < 0x100; p += 2) {
        const std::uint16_t fl = ds.field(0x452, p);
        if ((fl & 0x80) && !(fl & 0x40)) items.push_back({(fl >> 8) & 3, p});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.priority > b.priority; });
    std::vector<std::uint8_t> raw;
    for (const Item& it : items) {
        const int p = it.p;
        const std::uint16_t fl = ds.field(0x452, p);
        const int size = ds.field(0x952, p);
        if (size != 8 && size != 16 && size != 32 && size != 64) continue;
        const std::uint8_t* data = m.sprite_data(ds.field(0x852, p), ds.field(0x752, p));
        if (!data) continue;
        const int px = static_cast<std::int16_t>(ds.field(0x552, p)) - cam_x;
        const int py = static_cast<std::int16_t>(ds.field(0x652, p)) - cam_y;
        if (px >= kWidth || py >= kHeight || px + size <= 0 || py + size <= 0) continue;
        const std::uint8_t base = static_cast<std::uint8_t>(ds.field(0xF52, p));
        const int n = size * size;
        raw.resize(static_cast<std::size_t>(n));
        for (int g = 0; g < n / 8; ++g)
            for (int k = 0; k < 8; ++k) raw[static_cast<std::size_t>(g * 8 + k)] = data[g * 9 + 1 + k];
        const int cols = size / 4;
        for (int y = 0; y < size; ++y) {
            const int sy = py + y;
            if (sy < 0 || sy >= kHeight) continue;
            const int srow = (fl & 0x10) ? size - 1 - y : y;
            for (int x = 0; x < size; ++x) {
                const int sx = px + x;
                if (sx < 0 || sx >= kWidth) continue;
                const int scol = (fl & 0x20) ? size - 1 - x : x;
                const int plane = scol & 3;
                const std::uint8_t v = raw[static_cast<std::size_t>((plane * size + srow) * cols + (scol >> 2))];
                if (!v) continue;
                out.pixels[static_cast<std::size_t>(sy) * kWidth + sx] = colour(ds, v | base);
            }
        }
    }
}

// The 32x28 text layer at ds:2D08h, drawn with the tile blitter from the font
// (sub_146BA): glyph = character - 0x20, pixels are palette entries 0-3,
// opaque where the glyph mask is set. 0x20 is an empty cell.
void Renderer::draw_text(const Game& game, Image& out) const {
    const DataSegment& ds = game.machine().ds;
    const auto& font = fonts_[(ds.u16(0x1CF6) & 0x100) ? 1 : 0];
    for (int row = 0; row < 28; ++row) {
        for (int col = 0; col < 32; ++col) {
            const std::uint16_t v = ds.u16(static_cast<std::uint16_t>(0x2D08 + (row * 32 + col) * 2));
            if (v == 0x20 || v >= font.size()) continue;
            const Glyph& g = font[v];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    // Mask bytes follow the planar data: byte = plane*2 + (row >= 4),
                    // bit 7..0 = the 8 data bytes (4 rows x 2 columns) of that run.
                    const int plane = x & 3, data_col = x >> 2;
                    const int byte = plane * 2 + (y >> 2), bit = (y & 3) * 2 + data_col;
                    if (g.mask[byte] & (0x80 >> bit))
                        out.pixels[static_cast<std::size_t>(row * 8 + y) * kWidth + col * 8 + x] = colour(ds, g.pixels[y * 8 + x]);
                }
        }
    }
}

}  // namespace bt
