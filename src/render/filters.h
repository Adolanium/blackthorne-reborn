// Pixel-art upscalers that run on the CPU before the frame reaches the GPU.
#pragma once

#include "render/renderer.h"

namespace bt {

// Scale2x (EPX): doubles the image, rounding off diagonal staircases while
// keeping flat colours and hard edges.
void scale2x(const Image& src, Image& dst);

}  // namespace bt
