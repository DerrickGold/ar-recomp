#version 450

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 texture_uv;
layout(location = 0) out vec4 output_color;
layout(set = 2, binding = 0) uniform sampler2D source_texture;
layout(set = 3, binding = 0, std140) uniform Rim {
    vec4 sample_offset;
    vec4 color;
};

void main() {
    vec4 texel = texture(source_texture, texture_uv);
    float neighbour = texture(source_texture, texture_uv + sample_offset.xy).a;
    float edge = texel.a * max(0.0, texel.a - neighbour);
    float alpha = texel.a * vertex_color.a;
    // Premultiplied source-over plus an inward additive rim. Sprite colour
    // math can halve the body without incorrectly halving its light as well.
    // Transparent padding never paints either a halo or depth.
    output_color = vec4(texel.rgb * vertex_color.rgb * alpha +
                        color.rgb * color.a * edge, alpha);
}
