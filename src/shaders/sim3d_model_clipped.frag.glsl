#version 450

// Model sources contain color, never texture coordinates. Keep this material
// untextured rather than interpolating a sentinel through the camera plane.
layout(location = 0) in vec4 vertex_color;
layout(location = 0) out vec4 output_color;

void main() {
    output_color = vertex_color * gl_FragCoord.w;
    if (output_color.a <= (0.5 / 255.0)) {
        discard;
    }
}
