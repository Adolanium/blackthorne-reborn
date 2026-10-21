#include "render/effects.h"

#include <algorithm>
#include <vector>

namespace bt {
namespace {

// Averages `src` (premultiplied RGB in floats) down by `factor`.
void downsample(const Image& src, int factor, int& w, int& h, std::vector<float>& out) {
    w = std::max(1, src.width / factor);
    h = std::max(1, src.height / factor);
    out.assign(static_cast<std::size_t>(w) * h * 3, 0.0f);
    const float n = 1.0f / static_cast<float>(factor * factor);
    for (int y = 0; y < h * factor && y < src.height; ++y)
        for (int x = 0; x < w * factor && x < src.width; ++x) {
            const std::uint32_t p = src.pixels[static_cast<std::size_t>(y) * src.width + x];
            const float a = static_cast<float>(p >> 24) / 255.0f;
            if (a <= 0.0f) continue;
            float* o = &out[(static_cast<std::size_t>(y / factor) * w + x / factor) * 3];
            o[0] += static_cast<float>((p >> 16) & 0xFF) * a * n;
            o[1] += static_cast<float>((p >> 8) & 0xFF) * a * n;
            o[2] += static_cast<float>(p & 0xFF) * a * n;
        }
}

// Three box blurs of radius r (close to a Gaussian), edges clamped.
void blur(std::vector<float>& img, int w, int h, int r) {
    std::vector<float> tmp(img.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int k = -r; k <= r; ++k) sum += img[(static_cast<std::size_t>(y) * w + std::clamp(k, 0, w - 1)) * 3 + c];
                for (int x = 0; x < w; ++x) {
                    tmp[(static_cast<std::size_t>(y) * w + x) * 3 + c] = sum / static_cast<float>(2 * r + 1);
                    sum += img[(static_cast<std::size_t>(y) * w + std::clamp(x + r + 1, 0, w - 1)) * 3 + c];
                    sum -= img[(static_cast<std::size_t>(y) * w + std::clamp(x - r, 0, w - 1)) * 3 + c];
                }
            }
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int k = -r; k <= r; ++k) sum += tmp[(static_cast<std::size_t>(std::clamp(k, 0, h - 1)) * w + x) * 3 + c];
                for (int y = 0; y < h; ++y) {
                    img[(static_cast<std::size_t>(y) * w + x) * 3 + c] = sum / static_cast<float>(2 * r + 1);
                    sum += tmp[(static_cast<std::size_t>(std::clamp(y + r + 1, 0, h - 1)) * w + x) * 3 + c];
                    sum -= tmp[(static_cast<std::size_t>(std::clamp(y - r, 0, h - 1)) * w + x) * 3 + c];
                }
            }
    }
}

// Float RGB to an opaque image; `gain` lifts the spread-out light back up,
// and values are compressed so strong light doesn't clip.
void to_image(const std::vector<float>& img, int w, int h, float gain, Image& out) {
    out.resize(w, h);
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        auto ch = [&](int c) {
            const float v = img[i * 3 + static_cast<std::size_t>(c)] * gain / 255.0f;
            return static_cast<std::uint32_t>(std::clamp(v / (1.0f + v) * 2.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        out.pixels[i] = 0xFF000000u | (ch(0) << 16) | (ch(1) << 8) | ch(2);
    }
}

}  // namespace

void build_light_maps(const Image& emission, LightMaps& out) {
    int w = 0, h = 0;
    std::vector<float> img;
    downsample(emission, 4, w, h, img);
    blur(img, w, h, 9);  // about 36 game pixels each pass
    to_image(img, w, h, 40.0f, out.light);
    downsample(emission, 2, w, h, img);
    blur(img, w, h, 3);
    to_image(img, w, h, 9.0f, out.glow);
}

}  // namespace bt
