#include "render/filters.h"

#include <algorithm>

namespace bt {

void scale2x(const Image& src, Image& dst) {
    const int w = src.width, h = src.height;
    dst.resize(w * 2, h * 2);
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return src.pixels[static_cast<std::size_t>(y) * w + x];
    };
    for (int y = 0; y < h; ++y) {
        std::uint32_t* out0 = &dst.pixels[static_cast<std::size_t>(2 * y) * dst.width];
        std::uint32_t* out1 = out0 + dst.width;
        for (int x = 0; x < w; ++x) {
            const std::uint32_t p = at(x, y), a = at(x, y - 1), b = at(x + 1, y), c = at(x - 1, y), d = at(x, y + 1);
            std::uint32_t e0 = p, e1 = p, e2 = p, e3 = p;
            if (a != d && c != b) {
                if (c == a) e0 = a;
                if (a == b) e1 = b;
                if (d == c) e2 = c;
                if (b == d) e3 = d;
            }
            out0[2 * x] = e0;
            out0[2 * x + 1] = e1;
            out1[2 * x] = e2;
            out1[2 * x + 1] = e3;
        }
    }
}

}  // namespace bt
