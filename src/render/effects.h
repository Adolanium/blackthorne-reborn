// Lighting and atmosphere for the HD picture (docs/VISUALS.md).
//
// HdRenderer records what gives off light in each frame (its emission
// image: fire, torches and lava from the palette cycles, bright effect
// sprites). build_light_maps() spreads it into two small maps that the GPU
// pass (shaders/light.frag.hlsl) samples over the HD frame: a wide, soft
// light map and a tighter glow map.
#pragma once

#include "render/renderer.h"

namespace bt {

struct LightMaps {
    Image light;  // 1/4 of the emission's size, wide falloff
    Image glow;   // 1/2 of the emission's size, tight falloff
};

void build_light_maps(const Image& emission, LightMaps& out);

// The lighting pass's parameters (the shader's cbuffer, 16-byte rows).
struct LightParams {
    float playfield[4];  // the playfield rect in the frame's UV
    float ambient[4];    // rgb: unlit brightness; a: light strength
    float glow[4];       // rgb: glow tint; a: glow strength
    float misc[4];       // time, vignette, haze, flicker
};

}  // namespace bt
