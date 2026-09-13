#version 450
#extension GL_GOOGLE_include_directive : require
#include "sim3d_surface_mapping.glsl"

layout(location = 0) in vec4 point0;
layout(location = 1) in vec4 point1;
layout(location = 2) in vec4 point2;
layout(location = 3) in vec4 point3;
layout(location = 4) in vec4 shade0;
layout(location = 5) in vec4 shade1;
layout(location = 6) in vec4 shade2;
layout(location = 7) in vec4 shade3;

// Prefix is the existing SurfaceView contract; only the draw-local taps follow.
layout(set = 1, binding = 0, std140) uniform ShadowBatchView {
    mat4 matrix;
    vec4 basis[3];
    vec4 radial;
    vec4 light;
    vec4 material;
    vec4 rotation;
    vec4 offset_extent;
    vec4 atlas;
    vec4 tint;
    vec4 mask_rect;
    vec4 mask;
    vec4 shadow_basis[3];
    vec4 taps[3]; // chart displacement.xy, alpha, reserved
};
layout(location = 0) flat out vec3 alpha;
layout(location = 1) out vec2 texture_uv[3];

void main() {
    vec4 points[4] = vec4[4](point0, point1, point2, point3);
    vec4 shades[4] = vec4[4](shade0, shade1, shade2, shade3);
    int corner = gl_VertexIndex;
    precise vec4 clip = surface_clip(points[corner], shades[corner], matrix, basis, radial);
    precise float depth = clip.z * 0.5 + clip.w * 0.5;
    gl_Position = vec4(clip.xy, depth, clip.w);
    // Rotation/trigonometry is shared, but each tap independently retains its
    // original per-corner wrap, seam unwrap and pole clamp before interpolation.
    vec2 base[4];
    for (int p = 0; p < 4; ++p)
        base[p] = surface_coordinate(points[p].xyz, shadow_basis, material.z, rotation);
    for (int tap = 0; tap < 3; ++tap) {
        vec2 uv[4];
        for (int p = 0; p < 4; ++p) {
            uv[p] = base[p] + taps[tap].xy;
            uv[p].x -= floor(uv[p].x);
        }
        float minimum = min(min(uv[0].x, uv[1].x), min(uv[2].x, uv[3].x));
        float maximum = max(max(uv[0].x, uv[1].x), max(uv[2].x, uv[3].x));
        if (maximum - minimum > 0.5 && uv[corner].x < 0.5) uv[corner].x += 1.0;
        uv[corner].y = clamp(uv[corner].y, 0.0, 1.0);
        texture_uv[tap] = (atlas.xy + uv[corner] * (atlas.zw - 1.0) + 0.5) / offset_extent.zw;
        texture_uv[tap] *= clip.w; // Preserve screen-linear attributes through hardware clipping.
        alpha[tap] = taps[tap].z;
    }
}
