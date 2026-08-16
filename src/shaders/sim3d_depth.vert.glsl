#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 texture_uv;

void main() {
    gl_Position = in_position;
    vertex_color = in_color;
    texture_uv = in_uv;
}
