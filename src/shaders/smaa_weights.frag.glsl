#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D edges;
layout(set = 2, binding = 1) uniform sampler2D area;
layout(set = 2, binding = 2) uniform sampler2D search;
layout(set = 3, binding = 0) uniform SmaaWeights {
    vec4 metrics;
};

#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS metrics
#include "../../third_party/smaa/SMAA.hlsl"

void main() {
    vec2 pixel;
    vec4 offsets[3];
    SMAABlendingWeightCalculationVS(v_uv, pixel, offsets);
    o_color = SMAABlendingWeightCalculationPS(v_uv, pixel, offsets, edges, area, search, vec4(0.0)) * v_color;
}
