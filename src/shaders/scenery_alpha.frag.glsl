#version 450
#define AR_SCENERY_DEPTH_PASS 1
#define main material_main
#include "scenery_material.glsl"
#undef main
layout(set=2,binding=2) uniform sampler2D opaque_depth;
layout(set=2,binding=3) uniform sampler2D opaque_order;
layout(set=3,binding=1) uniform Alpha { vec4 options; } alpha;
#include "diorama_alpha_common.glsl"
void main() {
    alpha_filter();
    material_main();
    alpha_output();
}
