#version 450
layout(location=0) in vec4 v_color;
layout(location=1) in vec2 v_uv;
layout(location=0) out vec4 o_color;
layout(set=2,binding=0) uniform sampler2D u_texture;
layout(set=2,binding=1) uniform sampler2D opaque_depth;
layout(set=2,binding=2) uniform sampler2D opaque_order;
layout(set=3,binding=0) uniform Alpha { vec4 options; } alpha;
#include "diorama_alpha_common.glsl"
void main() {
    alpha_filter();
    o_color = v_color * (v_uv.x < 0.0 ? vec4(1) : texture(u_texture, v_uv));
    alpha_output();
}
