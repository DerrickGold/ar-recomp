#version 450

/* SDL_Render custom fragment shader interface (SDL_gpu.h "Shader Resources"):
 *   fragment stage -> set 2 = sampled textures, set 3 = uniform buffers.
 * Vertex interface comes from SDL's own render pipeline:
 *   location 0 = COLOR0 (vertex color), location 1 = TEXCOORD0. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Context {
    vec2  texel;
    float strength;
    float pad0;
};

void main() {
    vec2 uv = v_uv;
    vec2 tx = texel;

    vec4 c = texture(u_texture, uv);

    float a_up    = texture(u_texture, uv + vec2(0.0, -tx.y)).a;
    float a_down  = texture(u_texture, uv + vec2(0.0,  tx.y)).a;
    float a_left  = texture(u_texture, uv + vec2(-tx.x, 0.0)).a;
    float a_right = texture(u_texture, uv + vec2( tx.x, 0.0)).a;

    float min_neighbor = min(min(a_up, a_down), min(a_left, a_right));
    float edge = c.a * max(0.0, c.a - min_neighbor);

    vec3 rim_color = vec3(1.0, 0.95, 0.7);
    vec3 glow = c.rgb + rim_color * (edge * strength);

    vec4 vc = v_color;
    o_color = vec4(glow * vc.rgb, c.a * vc.a);
}
