#version 450
layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 texture_uv;
layout(location = 0) out vec4 output_color;
layout(set = 2, binding = 0) uniform sampler2D previous_texture;
layout(set = 2, binding = 1) uniform sampler2D current_texture;
// source plane width, height, input atlas slot, input atlas slot count.
layout(std140, set = 3, binding = 0) uniform Settings { vec4 plane; } settings;
void main() {
    vec2 p=clamp(texture_uv,vec2(0.5),settings.plane.xy-0.5);
    p.y+=settings.plane.z*settings.plane.y;
    vec2 uv=p/vec2(textureSize(current_texture,0));
    output_color=vertex_color.r>0.5?texture(previous_texture,uv):texture(current_texture,uv);
}
