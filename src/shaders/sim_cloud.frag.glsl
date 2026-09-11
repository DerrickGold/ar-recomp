#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0, std140) uniform Context {
    vec4 samples[3]; // UV scale, offset x/y, bank opacity
};

void main() {
    // Preserve bank order and the three original texture samples. Accumulate
    // premultiplied over transparent, then convert to the renderer's ordinary
    // straight-alpha blend. The background is composed exactly once.
    vec4 result = vec4(0.0);
    for (int i = 0; i < 3; ++i) {
        precise vec2 uv = v_uv * samples[i].x + samples[i].yz;
        vec4 bank = texture(u_texture, uv);
        float alpha = bank.a * v_color.a * samples[i].w;
        result.rgb = bank.rgb * alpha + result.rgb * (1.0 - alpha);
        result.a = alpha + result.a * (1.0 - alpha);
    }
    o_color = vec4(result.a > 0.0 ? result.rgb / result.a : vec3(0.0), result.a);
}
