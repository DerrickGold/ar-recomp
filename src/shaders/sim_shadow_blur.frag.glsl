#version 450

/* One axis of the SIM shadow's established seven-tap box blur. The legacy
 * fallback draws the same texture seven times with 36/255 alpha; this shader
 * keeps that exact kernel and normalization in one full-target draw.
 * Its uniform block mirrors the private upload layout in
 * platform/sdl/sim_shadow_effect_backend_sdl.c.
 *
 * SDL binding convention: fragment sampled textures are set 2 and fragment
 * uniform buffers are set 3. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Context {
    vec2 texel;
    float radius;
    float pad0;
};

vec4 sample_mask(vec2 uv) {
    /* The fallback shifts a destination rectangle, leaving uncovered pixels
     * transparent. Explicit bounds reproduce that edge behavior instead of
     * depending on a backend's sampler address mode. */
    if (any(lessThan(uv, vec2(0.0))) ||
        any(greaterThanEqual(uv, vec2(1.0))))
        return vec4(0.0);
    return texture(u_texture, uv);
}

void main() {
    vec2 delta = texel * (radius / 3.0);
    vec4 sum = vec4(0.0);
    sum += sample_mask(v_uv - 3.0 * delta);
    sum += sample_mask(v_uv - 2.0 * delta);
    sum += sample_mask(v_uv - 1.0 * delta);
    sum += sample_mask(v_uv);
    sum += sample_mask(v_uv + 1.0 * delta);
    sum += sample_mask(v_uv + 2.0 * delta);
    sum += sample_mask(v_uv + 3.0 * delta);
    o_color = sum * (36.0 / 255.0) * v_color;
}
