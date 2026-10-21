// The lighting pass over the HD frame (src/render/effects.h), drawn by SDL's
// GPU renderer as a custom render state: t0 is the frame, t1 the light map
// (wide, soft light from fires and flashes), t2 the glow map (the same
// sources, tight), t3 the UI mask (text boxes stay as they are), all in
// screen UV. Compiled to DXIL at build time.

cbuffer Params : register(b0, space3)
{
    float4 playfield;  // the playfield rect in UV (x0, y0, x1, y1); the HUD outside it is left as is
    float4 ambient;    // rgb: how bright unlit areas are; a: light strength
    float4 glow;       // rgb: glow tint; a: glow strength
    float4 misc;       // x: time (seconds), y: vignette, z: haze, w: flicker
    float4 view;       // xy: camera in game pixels (fog moves with the world); zw: one frame texel in UV
    float4 relief;     // x: relief strength (light wrapping around the painted shapes)
};

Texture2D u_frame : register(t0, space2);
SamplerState s_frame : register(s0, space2);
Texture2D u_light : register(t1, space2);
SamplerState s_light : register(s1, space2);
Texture2D u_glow : register(t2, space2);
SamplerState s_glow : register(s2, space2);
Texture2D u_ui : register(t3, space2);  // text and dialogue boxes: left unlit
SamplerState s_ui : register(s3, space2);

// SDL's renderer vertex shader (compiled by SDL_shadercross) passes these
// as TEXCOORD0 and TEXCOORD1.
struct PSInput {
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
};

struct PSOutput {
    float4 o_color : SV_Target;
};

float hash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

PSOutput main(PSInput input)
{
    PSOutput output;
    float2 uv = input.v_uv;
    float4 c = u_frame.Sample(s_frame, uv);
    bool inside = uv.x >= playfield.x && uv.y >= playfield.y && uv.x <= playfield.z && uv.y <= playfield.w;
    if (!inside) {
        output.o_color = c * input.v_color;
        return output;
    }
    float t = misc.x;
    // Local UV within the playfield, for the vignette and haze.
    float2 p = (uv - playfield.xy) / max(playfield.zw - playfield.xy, 1e-4);

    // Flicker: slow noise in time, a little different across the screen.
    float flick = 1.0 + misc.w * (noise(float2(t * 6.0, p.x * 3.0)) - 0.5);
    float3 light = u_light.Sample(s_light, uv).rgb * ambient.a * flick;
    float3 g = u_glow.Sample(s_glow, uv).rgb * glow.a * flick;

    // Relief: the painting's shading as a height, the light map's slope as
    // the direction light comes from. Surfaces facing a fire catch more of
    // its light, those facing away less.
    float2 px = view.zw * 2.0;
    float hl = dot(u_frame.Sample(s_frame, uv - float2(px.x, 0)).rgb, float3(0.3, 0.59, 0.11));
    float hr = dot(u_frame.Sample(s_frame, uv + float2(px.x, 0)).rgb, float3(0.3, 0.59, 0.11));
    float hu = dot(u_frame.Sample(s_frame, uv - float2(0, px.y)).rgb, float3(0.3, 0.59, 0.11));
    float hd = dot(u_frame.Sample(s_frame, uv + float2(0, px.y)).rgb, float3(0.3, 0.59, 0.11));
    float3 n = normalize(float3((hl - hr) * 4.0, (hu - hd) * 4.0, 1.0));
    float2 lx = view.zw * 24.0;
    float ll = dot(u_light.Sample(s_light, uv - float2(lx.x, 0)).rgb, 1.0);
    float lr = dot(u_light.Sample(s_light, uv + float2(lx.x, 0)).rgb, 1.0);
    float lu = dot(u_light.Sample(s_light, uv - float2(0, lx.y)).rgb, 1.0);
    float ld = dot(u_light.Sample(s_light, uv + float2(0, lx.y)).rgb, 1.0);
    float3 L = normalize(float3(lr - ll, ld - lu, 0.35));
    float facing = saturate(dot(n, -L) * 0.5 + 0.5);
    light *= lerp(1.0, 0.35 + 1.3 * facing, relief.x);

    // Unlit areas fall to the ambient level; light adds back colour where
    // fires and flashes are, without blowing out what's already bright.
    float3 lit = c.rgb * (ambient.rgb + light);
    lit += g * glow.rgb;

    // Haze: faint, slowly drifting, darker towards the bottom.
    // Two fog layers that move with the world at their own depth.
    float2 w = view.xy / 256.0;
    float h = (noise(p * float2(3.0, 2.0) + w * 0.6 + float2(t * 0.05, t * 0.02)) * 0.6 +
               noise(p * float2(6.0, 4.0) + w * 1.2 + float2(-t * 0.08, t * 0.03)) * 0.4) * misc.z;
    lit = lerp(lit, float3(0.10, 0.11, 0.16), h * (0.4 + 0.6 * p.y));

    // Vignette.
    float2 d = p - 0.5;
    lit *= 1.0 - misc.y * dot(d, d) * 1.6;

    float ui = u_ui.Sample(s_ui, uv).r;
    output.o_color = float4(lerp(saturate(lit), c.rgb, ui), c.a) * input.v_color;
    return output;
}
