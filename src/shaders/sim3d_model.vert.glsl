#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(set = 1, binding = 0, std140) uniform ModelView {
    mat4 matrix;
    vec4 viewport; // width, height, 2/width, 2/height
};
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 texture_uv;

void main() {
    // Explicit fused arithmetic and the legacy screen-space round trip. Keeping
    // W=1 preserves the existing affine color interpolation. The caller's
    // bounds check excludes clipped/behind-eye meshes from this prototype.
    precise vec4 clip = matrix[1] * in_position.y;
    clip = fma(matrix[0], vec4(in_position.x), clip);
    clip = fma(matrix[2], vec4(in_position.z), clip);
    clip = clip + matrix[3];
    precise float inverse_w = 1.0 / clip.w;
    // Refine a backend's approximate reciprocal before rasterization. Both
    // residual and correction must be fused; splitting them loses low bits.
    // Cross-backend parity still needs the strict image oracle on that backend.
    inverse_w = fma(fma(-clip.w, inverse_w, 1.0), inverse_w, inverse_w);
    precise float x = fma(clip.x * inverse_w, 0.5, 0.5) * viewport.x;
    precise float y = (1.0 - fma(clip.y * inverse_w, 0.5, 0.5)) * viewport.y;
    precise float depth = fma(clip.z * inverse_w, 0.5, 0.5);
    precise float screen_x = fma(x, viewport.z, -1.0);
    precise float screen_y = fma(-y, viewport.w, 1.0);
    gl_Position = vec4(screen_x, screen_y, depth, 1.0);
    vertex_color = in_color;
    texture_uv = vec2(-1.0);
}
