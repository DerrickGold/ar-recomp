#version 450

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_elevation;
layout(location = 2) in vec4 in_color;
layout(location = 3) in float in_variant;
layout(set = 1, binding = 0, std140) uniform RadialView {
    mat4 matrix;
    vec4 basis[3];
    vec4 radial; // sphere radius, reference elevation, height scale, variant
};
layout(location = 0) out vec4 vertex_color;

void main() {
    vertex_color = in_color;
    if (in_variant != 0.0 && in_variant != radial.w) {
        // Every vertex of an inactive quad takes this branch. It degenerates
        // outside the frustum, contributing neither color nor depth.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    precise vec3 normal;
    for (int row = 0; row < 3; ++row) {
        precise float n = basis[row].y * in_normal.y;
        n = fma(basis[row].x, in_normal.x, n);
        normal[row] = fma(basis[row].z, in_normal.z, n);
    }
    precise float radius = fma(radial.y, radial.z, radial.x);
    precise float rise = (in_elevation.x - radial.y) * radial.z;
    rise = rise + in_elevation.y;
    precise vec3 world = radius * (normal - vec3(0.0, 0.0, 1.0));
    world = fma(normal, vec3(rise), world);
    precise vec4 clip = matrix[1] * world.y;
    clip = fma(matrix[0], vec4(world.x), clip);
    clip = fma(matrix[2], vec4(world.z), clip);
    clip = clip + matrix[3];
    precise float depth = clip.z * 0.5 + clip.w * 0.5;
    gl_Position = vec4(clip.xy, depth, clip.w);
    vertex_color = in_color * clip.w;
}
