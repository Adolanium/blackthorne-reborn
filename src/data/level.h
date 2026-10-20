#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "data/archive.h"

namespace bt {

struct Rgb {
    std::uint8_t r, g, b;
};
using Palette = std::array<Rgb, 256>;

// 8x8 tile, one byte per pixel (0 = transparent). Values 1-15 are offset by
// the tile word's palette row; values >= 16 are absolute colours (sub_1438A).
using Tile = std::array<std::uint8_t, 64>;

// Tile reference word (sub_1438A): bits 6-15 tile, 4-5 flip, 3 unknown, 0-2 palette row.
struct TileRef {
    std::uint16_t raw;
    int tile() const { return raw >> 6; }
    bool flip_x() const { return raw & 0x10; }
    bool flip_y() const { return raw & 0x20; }
    int palette_row() const { return raw & 7; }
};

// 16x16 block of four tiles: top-left, top-right, bottom-left, bottom-right.
using Metatile = std::array<TileRef, 4>;

// Map cell (sub_189A5): bits 0-9 metatile, bits 10-15 collision code.
struct MapLayer {
    int width = 0, height = 0;          // in metatiles
    std::vector<std::uint16_t> cells;
    std::uint16_t at(int x, int y) const { return cells[y * width + x]; }
    int metatile(int x, int y) const { return at(x, y) & 0x3FF; }
    int collision(int x, int y) const { return at(x, y) >> 10; }
};

// Object spawn record (14 bytes at descriptor 0x36, read by sub_11380 and
// sub_11458). Spawns when the camera rectangle overlaps x +- rx, y +- ry;
// flag 0x800 spawns at level start.
struct ObjectSpawn {
    int x, y;           // pixels
    int rx, ry;         // activation half extents
    int type;           // actor type, index into the actor table (entry 64)
    int flags;
    int param;
};

struct SpriteBankRef {
    int entry;      // 0-based DATA.DAT entry
    int width;      // pixels
    int height;     // pixels
    int flag;
};

struct Level {
    int descriptor_entry = 0;
    Palette palette{};
    std::vector<Tile> tiles;
    std::vector<Metatile> metatiles;
    MapLayer foreground;   // header 0x1C/0x1E/0x21; carries flag bits
    MapLayer background;   // header 0x27/0x29/0x2C
    std::vector<SpriteBankRef> sprite_banks;
    std::vector<ObjectSpawn> objects;
};

// The 18 levels in play order (descriptor DATA.DAT entries).
constexpr std::array<int, 18> kLevelDescriptors = {
    191, 192, 193, 194, 195, 203, 204, 205, 206,
    217, 218, 219, 220, 235, 236, 237, 238, 239,
};

Level load_level(const Archive& archive, int descriptor_entry);

// Paints both layers (background then foreground) into an indexed buffer of
// width*16 x height*16 pixels; returns colour indices, -1 where transparent.
std::vector<int> render_level(const Level& level, bool background, bool foreground);

}  // namespace bt
