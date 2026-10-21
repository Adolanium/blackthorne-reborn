// Lighting and atmosphere for the HD picture (docs/VISUALS.md).
//
// HdRenderer records what gives off light in each frame (its emission
// image: fire, torches and lava from the palette cycles, bright effect
// sprites). build_light_maps() spreads it into two small maps that the GPU
// pass (shaders/light.frag.hlsl) samples over the HD frame: a wide, soft
// light map and a tighter glow map.
#pragma once

#include <vector>

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
    float view[4];       // camera x, y (game pixels); one frame texel in UV
    float relief[4];     // relief strength
};

// Embers and sparks rising from fire, in world coordinates (game pixels),
// so they scroll with the level.
struct Particle {
    float x, y, vx, vy, life, age, size;
    std::uint32_t colour;
};

class Embers {
public:
    // Spawns from the fire in this frame's emission image (screen pixels;
    // the playfield starts at play_x and shows the world from view_x,
    // cam_y) and moves the particles on by dt seconds.
    void update(const Image& emission, int play_x, int view_x, int cam_y, float dt);
    const std::vector<Particle>& particles() const { return parts_; }
    void clear() { parts_.clear(); }

private:
    std::vector<Particle> parts_;
    std::vector<int> fire_;
    float budget_ = 0;
    std::uint32_t seed_ = 0x9E3779B9u;
    float random();
};

}  // namespace bt
