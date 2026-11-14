#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D scene;
layout(set = 3, binding = 0) uniform SmaaEdges {
    vec4 metrics;
    vec4 uv_bounds;
};

#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS metrics
#define SMAA_WRITE_EMPTY_LUMA_EDGES
#define SMAASamplePoint(tex, coord) texture(tex, clamp(coord, uv_bounds.xy, uv_bounds.zw))
#include "../../third_party/smaa/SMAA.hlsl"

void main() {
    vec4 offsets[3];
    SMAAEdgeDetectionVS(v_uv, offsets);
    o_color = vec4(SMAALumaEdgeDetectionPS(v_uv, offsets, scene), 0.0, 0.0) * v_color;
}
