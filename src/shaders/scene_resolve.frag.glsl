#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D scene;
layout(set = 3, binding = 0) uniform Resolve {
    vec2 uv_min;
    vec2 uv_max;
    vec2 texel;
    vec2 footprint;
    int downsample;
};

vec3 sample_scene(vec2 uv) {
    return texture(scene, clamp(uv, uv_min, uv_max)).rgb;
}

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

vec3 fxaa() {
    vec3 center = sample_scene(v_uv);
    float middle = luma(center);
    float nw = luma(sample_scene(v_uv + texel * vec2(-1, -1)));
    float ne = luma(sample_scene(v_uv + texel * vec2( 1, -1)));
    float sw = luma(sample_scene(v_uv + texel * vec2(-1,  1)));
    float se = luma(sample_scene(v_uv + texel * vec2( 1,  1)));
    float darkest = min(middle, min(min(nw, ne), min(sw, se)));
    float brightest = max(middle, max(max(nw, ne), max(sw, se)));
    if (brightest - darkest < max(0.04, brightest * 0.125))
        return center;

    vec2 direction = vec2(sw + se - nw - ne, nw + sw - ne - se);
    float noise = max((nw + ne + sw + se) / 32.0, 1.0 / 128.0);
    direction /= min(abs(direction.x), abs(direction.y)) + noise;
    direction = clamp(direction, vec2(-8), vec2(8)) * texel;
    vec3 inner = 0.5 * (sample_scene(v_uv - direction / 6.0) +
                        sample_scene(v_uv + direction / 6.0));
    vec3 outer = 0.5 * inner + 0.25 * (sample_scene(v_uv - direction * 0.5) +
                                       sample_scene(v_uv + direction * 0.5));
    float filtered = luma(outer);
    return filtered < darkest || filtered > brightest ? inner : outer;
}

void main() {
    vec3 color;
    if (downsample != 0) {
        // Four bilinear taps cover the output pixel's source footprint.
        vec2 offset = footprint * texel * 0.25;
        color = 0.25 * (sample_scene(v_uv + offset) + sample_scene(v_uv - offset) +
                        sample_scene(v_uv + offset * vec2(-1, 1)) +
                        sample_scene(v_uv + offset * vec2(1, -1)));
    } else {
        color = fxaa();
    }
    o_color = vec4(color, 1.0) * v_color;
}
