#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "data/archive.h"
#include "engine/data_segment.h"

namespace bt {

// The memory the original game's logic works on: the data segment, the map
// segment (foreground layer, background layer, metatile table; ds:259Ch) and
// the actor script blob (DATA.DAT entry 64). Offsets are 16-bit and wrap the
// same way the original segment arithmetic does.
struct Machine {
    DataSegment ds;
    std::array<std::uint8_t, 0x10000> map{};
    Bytes scripts;
    // seg001 and seg002 as they sit in memory below the data segment: the
    // language's text table (DATA.DAT entry 1, 2 or 3) loaded over the EXE
    // image. Text reads use 16-bit offsets that can run past the table, into
    // seg002 and then the data segment (at kTextImageSize), as in the original.
    // The VGA DAC: the palette as last uploaded (sub_127C5), which is what
    // the screen shows; ds:4D12h can be ahead of it.
    std::array<std::uint8_t, 768> dac{};
    // The 32x28 screen cells repainted since the last full redraw (sprites
    // or text went over them). The original's full redraw paints all
    // background tiles then all foreground tiles; only repainted cells get
    // the tile bit 3 ordering (sub_15A55). Kept for the display.
    std::array<std::uint8_t, 32 * 28> repainted{};
    // The map segment as the level loaded it: cells the game has changed
    // since (doors, bridges) differ from it. Art packs paint the loaded map.
    std::array<std::uint8_t, 0x10000> map_at_load{};
    // The tileset and masks as the level loaded them: tiles that tile
    // animations rewrote differ from these.
    std::array<std::uint8_t, 0x10000> tiles_at_load{};
    std::array<std::uint8_t, 0x2000> tile_masks_at_load{};
    // The level's palette (ds:4A12h) as it loaded; the game can change it later.
    std::array<std::uint8_t, 768> palette_at_load{};
    Bytes text;
    static constexpr std::size_t kTextImageSize = 0x4240;
    // Past the data segment's own image and stack (8110h bytes) come the
    // buffers DOS allocated next: the tile animation frames (2000h bytes,
    // ds:2594h) and then the tile graphics (ds:2596h).
    static constexpr std::size_t kHeapStart = 0x8110, kTileAnimBytes = 0x2000;
    std::uint8_t text_u8(std::uint16_t off) const {
        if (off < kTextImageSize) return text[off];
        const std::size_t d = off - kTextImageSize;
        if (d < kHeapStart) return ds.u8(static_cast<std::uint16_t>(d));
        const std::size_t h = d - kHeapStart;
        return h < kTileAnimBytes ? tile_anims[h] : tiles[h - kTileAnimBytes];
    }

    // Sprite memory. Segment numbers stored in the data segment are these
    // symbolic values; sprite_data() maps them back to buffers.
    static constexpr std::uint16_t kFrameSeg = 0x5000;   // ds:259Eh: actor frame buffers
    static constexpr std::uint16_t kStaticSeg = 0x6000;  // ds:25A0h: preloaded single-frame sprites
    static constexpr std::uint16_t kDataSeg = 0x7000;    // "ds" itself (HUD icons live in the data segment)
    static constexpr std::uint16_t kTileAnimSeg = 0x8000;  // ds:2594h: tile animation frames
    std::array<std::uint8_t, 0x10000> frames{};
    std::array<std::uint8_t, 0x10000> statics{};
    // Tileset (ds:2596h: 64 planar bytes per 8x8 tile), its masks (ds:2598h:
    // 8 bytes per tile) and tile animation frames (ds:2594h). Tile animations
    // rewrite tiles in place, so the renderer reads tiles from here.
    std::array<std::uint8_t, 0x10000> tiles{};
    std::array<std::uint8_t, 0x2000> tile_masks{};
    std::array<std::uint8_t, 0x10000> tile_anims{};

    // Sprite banks by slot (the byte index into ds:1152h divided by 2), raw
    // frames as stored in DATA.DAT; the game keeps these in EMS.
    std::vector<Bytes> banks = std::vector<Bytes>(16);

    // Pixel data for a (segment, offset) pair as stored in part fields 852h/752h.
    const std::uint8_t* sprite_data(std::uint16_t seg, std::uint16_t off) const {
        if (seg == kFrameSeg) return frames.data() + off;
        if (seg == kStaticSeg) return statics.data() + off;
        if (seg == kDataSeg) return ds.raw() + off;
        return nullptr;
    }

    std::uint16_t map_u16(std::uint16_t off) const {
        return static_cast<std::uint16_t>(map[off] | (map[static_cast<std::uint16_t>(off + 1)] << 8));
    }
    void set_map_u16(std::uint16_t off, std::uint16_t v) {
        map[off] = static_cast<std::uint8_t>(v);
        map[static_cast<std::uint16_t>(off + 1)] = static_cast<std::uint8_t>(v >> 8);
    }

    // Map geometry lives in the level descriptor header, which sits at ds:1CEEh.
    static constexpr std::uint16_t kMapWidth = 0x1D0A;    // header +0x1C, in cells
    static constexpr std::uint16_t kMapHeight = 0x1D0C;   // header +0x1E
    static constexpr std::uint16_t kMapRowBytes = 0x1C96;
    static constexpr std::uint16_t kMetatileTable = 0x25B4;  // offset of metatiles in the map segment

    // sub_189A5: the foreground cell at (tx, ty); 0x400 (class 1) outside the map.
    std::uint16_t cell(std::uint16_t tx, std::uint16_t ty) const {
        if (tx >= ds.u16(kMapWidth) || ty >= ds.u16(kMapHeight)) return 0x400;
        return map_u16(static_cast<std::uint16_t>(ty * ds.u16(kMapRowBytes) + tx * 2));
    }
    // sub_189CD: no bounds check in the original.
    void set_cell(std::uint16_t tx, std::uint16_t ty, std::uint16_t v) {
        set_map_u16(static_cast<std::uint16_t>(ty * ds.u16(kMapRowBytes) + tx * 2), v);
    }
    // sub_18992 / sub_18984: collision class of the cell containing pixel (x, y).
    std::uint8_t pixel_class(std::uint16_t x, std::uint16_t y) const {
        return static_cast<std::uint8_t>(cell(static_cast<std::uint16_t>(x >> 4), static_cast<std::uint16_t>(y >> 4)) >> 10);
    }
};

}  // namespace bt
