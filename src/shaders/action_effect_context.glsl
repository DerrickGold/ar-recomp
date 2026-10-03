#include "action_skybox_mapping.glsl"
// std140 mirror of EffectUniform in action_effect_source_sdl.c.
struct EffectPlane { vec4 uv; vec4 shape; vec4 offset; vec4 bounds; vec4 sky; vec4 fold; vec4 front; };
struct SkyBand { vec4 bounds; vec4 rows; };
layout(std140, set = 2, binding = 0) uniform Context {
    mat4 matrix;
    vec4 viewport;
    vec4 capture;
    vec4 geometry;
    vec4 clock;
    vec4 follow;
    EffectPlane planes[9];
    vec4 sky_meta;
    SkyBand sky_bands[11];
    vec4 moon;
    SkyMapping sky_mapping;
} settings;
