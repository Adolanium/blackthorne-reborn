#pragma once

#include <cstdint>
#include <vector>

#include "data/archive.h"

namespace bt {

// One frame, one byte per pixel (0 = transparent).
struct SpriteFrame {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
};

// Splits a sprite bank into frames of width x height pixels. Frames are
// stored as Mode X planar square cells; see docs/FORMATS.md "Sprite banks".
std::vector<SpriteFrame> decode_sprite_bank(const Bytes& data, int width, int height);

}  // namespace bt
