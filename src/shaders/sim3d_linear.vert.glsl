#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_axis_depth;
layout(set = 1, binding = 0, std140) uniform LinearView {
    mat4 matrix;
    vec4 offset;
    vec4 axes[16];
    vec4 raster; // target width, height, pixel-center snapping, reserved
};
layout(location = 0) out vec4 vertex_color;

void main() {
    precise vec3 position = in_position.xyz + offset.xyz;
    position = position + in_position.w * axes[int(in_axis_depth.x)].xyz;
    precise vec4 clip = matrix[0] * position.x;
    clip = clip + matrix[1] * position.y;
    clip = clip + matrix[2] * position.z;
    clip = clip + matrix[3];
    // Keep homogeneous positions for hardware clipping, including W <= 0.
    // Snapping never projects a behind-eye vertex through an unbounded divide.
    if (raster.z != 0.0 && clip.w > 0.001) {
        precise vec2 ndc = clip.xy / clip.w;
        precise vec2 screen = vec2((ndc.x * 0.5 + 0.5) * raster.x,
            (1.0 - (ndc.y * 0.5 + 0.5)) * raster.y);
        screen = floor(screen) + vec2(0.5);
        clip.xy = vec2(screen.x * (2.0 / raster.x) - 1.0,
            1.0 - screen.y * (2.0 / raster.y)) * clip.w;
    }
    precise float depth = clip.z * 0.5 + clip.w * 0.5;
    if (in_axis_depth.y != 0.0 && clip.w > 0.001) {
        precise vec2 safety = clip.zw + matrix[2].zw * in_axis_depth.y;
        if (safety.y > 0.001)
            depth = min(depth, (safety.x / safety.y * 0.5 + 0.5) * clip.w);
    }
    gl_Position = vec4(clip.xy, depth, clip.w);
    vertex_color = in_color * clip.w;
}
