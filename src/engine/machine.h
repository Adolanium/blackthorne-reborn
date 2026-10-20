#pragma once

#include <array>
#include <cstdint>

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
