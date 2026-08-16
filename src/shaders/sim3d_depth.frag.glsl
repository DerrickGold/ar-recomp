#version 450

layout(set = 2, binding = 0) uniform sampler2D source_texture;

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 texture_uv;
layout(location = 0) out vec4 output_color;

void main() {
    vec4 texel = texture_uv.x < 0.0
        ? vec4(1.0)
        : texture(source_texture, texture_uv);
    output_color = texel * vertex_color;
    if (output_color.a <= (0.5 / 255.0)) {
        discard;
    }
}
