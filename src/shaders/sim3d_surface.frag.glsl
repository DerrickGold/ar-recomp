#version 450

// Same texture/alpha/depth policy, explicitly affine native-art attributes.
layout(set = 2, binding = 0) uniform sampler2D source_texture;
layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 texture_uv;
layout(location = 2) in vec4 focus_color;
layout(location = 0) out vec4 output_color;
void main() {
    vec2 uv = texture_uv * gl_FragCoord.w;
    vec4 texel = uv.x < 0.0 ? vec4(1.0) : texture(source_texture, uv);
    output_color = texel * (vertex_color * gl_FragCoord.w);
    if (output_color.a <= (0.5 / 255.0)) discard;
    vec4 haze = focus_color * gl_FragCoord.w;
    output_color.rgb = output_color.rgb * (1.0 - haze.a) + haze.rgb;
}
