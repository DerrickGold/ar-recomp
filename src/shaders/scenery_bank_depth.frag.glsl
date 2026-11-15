#version 450
#define AR_SCENERY_DEPTH_PASS 1
#define AR_SCENERY_TEXTURE_BANK 1
layout(set=2, binding=2) uniform sampler2D u_texture_1;
layout(set=2, binding=3) uniform sampler2D u_texture_2;
#include "scenery_material.glsl"
