#include "render/png.h"

#include <stdexcept>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4244 4456 4459 4701 4996)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace bt {

void write_png(const std::filesystem::path& path, const Image& image) {
    std::vector<unsigned char> rgba(image.pixels.size() * 4);
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        const std::uint32_t p = image.pixels[i];
        rgba[i * 4] = static_cast<unsigned char>(p >> 16);
        rgba[i * 4 + 1] = static_cast<unsigned char>(p >> 8);
        rgba[i * 4 + 2] = static_cast<unsigned char>(p);
        rgba[i * 4 + 3] = static_cast<unsigned char>(p >> 24);
    }
    if (!stbi_write_png(path.string().c_str(), image.width, image.height, 4, rgba.data(), image.width * 4))
        throw std::runtime_error("cannot write " + path.string());
}

bool read_png(const std::filesystem::path& path, Image& image) {
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!data) return false;
    image.width = w;
    image.height = h;
    image.pixels.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < image.pixels.size(); ++i)
        image.pixels[i] = (static_cast<std::uint32_t>(data[i * 4 + 3]) << 24) | (static_cast<std::uint32_t>(data[i * 4]) << 16) |
                          (static_cast<std::uint32_t>(data[i * 4 + 1]) << 8) | data[i * 4 + 2];
    stbi_image_free(data);
    return true;
}

}  // namespace bt
