#version 450
layout(set = 2, binding = 0) uniform sampler2D source_texture;
layout(location = 0) flat in vec3 alpha;
layout(location = 1) in vec2 texture_uv[3];
layout(location = 0) out vec4 output_color;
void main() {
    float transmission = 1.0;
    for (int i = 0; i < 3; ++i) {
        if (alpha[i] <= (0.5 / 255.0)) continue;
        vec2 uv = texture_uv[i] * gl_FragCoord.w;
        float a = (uv.x < 0.0 ? 1.0 : texture(source_texture, uv).a) * alpha[i];
        // Cutoff belongs to EACH original draw, not just the combined result.
        if (a > (0.5 / 255.0)) transmission *= 1.0 - a;
    }
    output_color = vec4(0.0, 0.0, 0.0, 1.0 - transmission);
    if (output_color.a <= (0.5 / 255.0)) discard;
}
