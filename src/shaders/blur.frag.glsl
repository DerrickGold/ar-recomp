#version 450

/* 3x3 weighted-box blur (9 taps, centre weighted x2) as a cheap Gaussian
 * approximation. Softens the hard-edged silhouette shadow into a soft drop
 * shadow, and doubles as the skybox defocus.
 *
 * Vertex colour (the existing black+alpha tint) is preserved by the final
 * multiply, so this stays purely additive over the CPU-side effect it
 * replaces.
 *
 * SDL binding convention (SDL_gpu.h "Shader Resources"): fragment stage uses
 * set 2 for sampled textures and set 3 for uniform buffers; SDL's render
 * pipeline supplies COLOR0 at location 0 and TEXCOORD0 at location 1. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Context {
    vec2  texel;
    float radius;
    float pad0;
};

void main() {
    vec2 offset = texel * radius;
    vec2 uv = v_uv;

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

    o_color = (sum / 10.0) * v_color;
}
