#include "data/sprites.h"

namespace bt {
namespace {

struct Cell {
    int size, x, y;
};

// Storage order of the square cells making up a width x height frame.
std::vector<Cell> cell_layout(int width, int height) {
    std::vector<Cell> cells;
    int y = 0;
    if (height % 32) {
        for (int x = 0; x < width; x += 16) cells.push_back({16, x, 0});
        y = 16;
    }
    for (; y < height; y += 32) {
        int x = 0;
        for (; x + 32 <= width; x += 32) cells.push_back({32, x, y});
        if (x < width) {
            cells.push_back({16, x, y});
            cells.push_back({16, x, y + 16});
        }
    }
    return cells;
}

}  // namespace

std::vector<SpriteFrame> decode_sprite_bank(const Bytes& data, int width, int height) {
    const std::size_t frame_size = static_cast<std::size_t>(width) * height;
    const std::vector<Cell> layout = cell_layout(width, height);
    std::vector<SpriteFrame> frames;
    for (std::size_t base = 0; base + frame_size <= data.size(); base += frame_size) {
        SpriteFrame f{width, height, std::vector<std::uint8_t>(frame_size)};
        std::size_t o = base;
        for (const Cell& c : layout) {
            const int cols = c.size / 4;
            for (int plane = 0; plane < 4; ++plane)
                for (int row = 0; row < c.size; ++row)
                    for (int col = 0; col < cols; ++col)
                        f.pixels[static_cast<std::size_t>(c.y + row) * width + c.x + col * 4 + plane] =
                            data[o + (plane * c.size + row) * cols + col];
            o += static_cast<std::size_t>(c.size) * c.size;
        }
        frames.push_back(std::move(f));
    }
    return frames;
}

}  // namespace bt
