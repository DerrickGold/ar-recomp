#version 450

/* Depth of field + parallax-aware edge AA, COMBINED — deliberately one shader.
 *
 * Both effects target the SAME layer set (BG1/BG2 and their priority-split
 * halves), and SDL allows only ONE custom fragment shader bound per draw call.
 * An earlier version picked edge AA over DOF whenever both were enabled, which
 * — since both default on — meant DOF silently never rendered at all. Doing
 * both in one pass fixes that, and each knob is independently zeroable:
 * blur_radius = 0 makes the box blur a no-op (all 9 taps land on the same
 * texel), edge_feather <= 0 skips the fade. So this one shader correctly
 * serves DOF-only, edge-AA-only, both, or neither.
 *
 * The uniform block is all scalars on purpose: it mirrors DofEdgeUniforms in
 * diorama.c field-for-field, so the std140 offsets and the C struct offsets
 * agree. Packing a pair into a vec2 here would shift every later member.
 *
 * SDL binding convention (SDL_gpu.h "Shader Resources"): fragment stage uses
 * set 2 for sampled textures and set 3 for uniform buffers; SDL's render
 * pipeline supplies COLOR0 at location 0 and TEXCOORD0 at location 1. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Context {
    float texel_w;
    float texel_h;
    float blur_radius;
    float u_min;
    float u_max;
    float v_min;
    float v_max;
    float edge_feather;
    float pad0;
};

void main() {
    vec2 uv = v_uv;
    vec2 offset = vec2(texel_w, texel_h) * blur_radius;

    vec4 sum = vec4(0.0);
    sum += texture(u_texture, uv + vec2(-offset.x, -offset.y));
    sum += texture(u_texture, uv + vec2( 0.0,      -offset.y));
    sum += texture(u_texture, uv + vec2( offset.x, -offset.y));
    sum += texture(u_texture, uv + vec2(-offset.x,  0.0));
    sum += texture(u_texture, uv) * 2.0;
    sum += texture(u_texture, uv + vec2( offset.x,  0.0));
    sum += texture(u_texture, uv + vec2(-offset.x,  offset.y));
    sum += texture(u_texture, uv + vec2( 0.0,       offset.y));
    sum += texture(u_texture, uv + vec2( offset.x,  offset.y));
    vec4 c = sum / 10.0;

    /* Feather the layer's TRUE UV edge, not the screen edge — these planes are
     * tilted, so the geometric border lands at an arbitrary screen angle. */
    float fade = 1.0;
    if (edge_feather > 0.0) {
        float du = min(uv.x - u_min, u_max - uv.x);
        float dv = min(uv.y - v_min, v_max - uv.y);
        float d = min(du, dv);
        float texel_avg = (texel_w + texel_h) * 0.5;
        fade = clamp(d / (texel_avg * edge_feather), 0.0, 1.0);
    }

    o_color = vec4(c.rgb * v_color.rgb, c.a * fade * v_color.a);
}
