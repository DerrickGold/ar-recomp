#version 450
#define AR_SCENERY_TEXTURE_BANK 1
layout(set=2, binding=4) uniform sampler2D u_texture_1;
layout(set=2, binding=5) uniform sampler2D u_texture_2;
#include "scenery_alpha_material.glsl"
