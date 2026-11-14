#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D scene;
layout(set = 2, binding = 1) uniform sampler2D weights;
layout(set = 3, binding = 0) uniform SmaaBlend {
    vec4 metrics;
    vec4 scene_rect; // origin and size in UV space
};

#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS metrics
// Clamp to texel centers before remapping to the source rectangle.
vec2 scene_uv(vec2 uv) {
    vec2 half_pixel = metrics.xy * 0.5;
    return scene_rect.xy + clamp(uv, half_pixel, 1.0 - half_pixel) * scene_rect.zw;
}
#define SMAASampleNeighborhoodColor(tex, coord) textureLod(tex, scene_uv(coord), 0.0)
#include "../../third_party/smaa/SMAA.hlsl"

void main() {
    // Weights cover the scene rectangle. Color remains in the original texture.
    vec2 uv = (v_uv - scene_rect.xy) / scene_rect.zw;
    vec4 offsets;
    SMAANeighborhoodBlendingVS(uv, offsets);
    o_color = vec4(SMAANeighborhoodBlendingPS(uv, offsets, scene, weights).rgb, 1.0) * v_color;
}
