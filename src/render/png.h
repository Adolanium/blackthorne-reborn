// PNG reading and writing for art packs and exports (stb_image, public domain).
#pragma once

#include <filesystem>

#include "render/renderer.h"

namespace bt {

// Writes ARGB pixels as an RGBA PNG; throws on failure.
void write_png(const std::filesystem::path& path, const Image& image);
// Reads a PNG (any format) into ARGB pixels; false if missing or unreadable.
bool read_png(const std::filesystem::path& path, Image& image);

}  // namespace bt
