#include "data/level.h"

#include <algorithm>

namespace bt {
namespace {

constexpr std::uint16_t kEnd = 0xFFFF;

Palette palette_from_vga(const Bytes& data, std::size_t first_colour, std::size_t count) {
    Palette p{};
    for (std::size_t i = 0; i < count; ++i) {
        auto scale = [](std::uint8_t v) { return static_cast<std::uint8_t>(std::min(255, v * 255 / 63)); };
        p[first_colour + i] = {scale(data[i * 3]), scale(data[i * 3 + 1]), scale(data[i * 3 + 2])};
    }
    return p;
}

MapLayer load_layer(const Archive& archive, const Bytes& header, std::size_t at) {
    MapLayer layer;
    layer.width = read_u16(header, at);
    layer.height = read_u16(header, at + 2);
    const int id = read_u16(header, at + 5);
    if (id == 0 || id == kEnd) return layer;
    const Bytes data = archive.resource(id);
    layer.cells.resize(static_cast<std::size_t>(layer.width) * layer.height);
    for (std::size_t i = 0; i < layer.cells.size(); ++i) layer.cells[i] = read_u16(data, i * 2);
    return layer;
}

// Planar 8x8: 4 planes x 8 rows x 2 bytes, pixel x = col*4 + plane.
Tile decode_tile(const std::uint8_t* src) {
    Tile t{};
    for (int plane = 0; plane < 4; ++plane)
        for (int y = 0; y < 8; ++y)
            for (int col = 0; col < 2; ++col)
                t[y * 8 + col * 4 + plane] = src[plane * 16 + y * 2 + col];
    return t;
}

}  // namespace

Level load_level(const Archive& archive, int descriptor_entry) {
    Level lv;
    lv.descriptor_entry = descriptor_entry;
    const Bytes d = archive.entry(descriptor_entry);

    lv.foreground = load_layer(archive, d, 0x1C);
    lv.background = load_layer(archive, d, 0x27);

    const Bytes tiles = archive.resource(read_u16(d, 0x23));
    for (std::size_t o = 0; o + 64 <= tiles.size(); o += 64) lv.tiles.push_back(decode_tile(&tiles[o]));

    const Bytes meta = archive.resource(read_u16(d, 0x25));
    for (std::size_t o = 0; o + 8 <= meta.size(); o += 8) {
        Metatile m{};
        for (int k = 0; k < 4; ++k) m[k] = {read_u16(meta, o + k * 2)};
        lv.metatiles.push_back(m);
    }

    // Walk the descriptor body in the order of sub_105B4's helpers.
    std::size_t o = 0x36;
    while (read_u16(d, o) != kEnd) o += 14;                       // sub_1074E
    o += 2;
    while (read_u16(d, o) != kEnd) {                              // sub_10760
        const Bytes res = archive.resource(read_u16(d, o));
        const int slot = d[o + 2];
        if (res.size() == 768) {
            lv.palette = palette_from_vga(res, 0, 256);
        } else if (res.size() == 48) {
            const Palette part = palette_from_vga(res, slot, 16);
            std::copy_n(part.begin() + slot, 16, lv.palette.begin() + slot);
        }
        o += 3;
    }
    o += 2 + 2;                                                   // sub_107BC
    while (d[o] != 0) {
        o += 3;
        while (read_u16(d, o) != kEnd) o += 2;
        o += 2;
    }
    o += 1 + 2;                                                   // sub_10802
    while (d[o] != 0) o += 8;
    o += 1;
    while (read_u16(d, o) != kEnd) o += 6;                        // sub_10876
    o += 2;
    while (read_u16(d, o) != kEnd) {                              // sub_108A5
        lv.sprite_banks.push_back({read_u16(d, o) - 1, d[o + 2] * 8, d[o + 3] * 8, d[o + 4]});
        o += 5;
    }
    return lv;
}

std::vector<int> render_level(const Level& level, bool background, bool foreground) {
    const MapLayer& ref = level.foreground.cells.empty() ? level.background : level.foreground;
    const int w = ref.width * 16, h = ref.height * 16;
    std::vector<int> out(static_cast<std::size_t>(w) * h, -1);

    auto paint = [&](const MapLayer& layer) {
        for (int my = 0; my < layer.height; ++my)
            for (int mx = 0; mx < layer.width; ++mx) {
                const int mi = layer.at(mx, my) & 0xFFF;
                if (mi >= static_cast<int>(level.metatiles.size())) continue;
                for (int k = 0; k < 4; ++k) {
                    const TileRef ref = level.metatiles[mi][k];
                    if (ref.tile() >= static_cast<int>(level.tiles.size())) continue;
                    const Tile& t = level.tiles[ref.tile()];
                    const int ox = mx * 16 + (k & 1) * 8, oy = my * 16 + (k >> 1) * 8;
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x) {
                            const int sx = ref.flip_x() ? 7 - x : x;
                            const int sy = ref.flip_y() ? 7 - y : y;
                            const int v = t[sy * 8 + sx];
                            if (v) out[static_cast<std::size_t>(oy + y) * w + ox + x] = v + ref.palette_row() * 16;
                        }
                }
            }
    };
    if (background && !level.background.cells.empty()) paint(level.background);
    if (foreground && !level.foreground.cells.empty()) paint(level.foreground);
    return out;
}

}  // namespace bt
