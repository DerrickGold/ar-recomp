#version 450

// One original quad per instance. Ground and shadows share the same position
// computation and diagonal; no independently clipped/retriangulated receiver.
layout(location = 0) in vec4 point0; // chart direction.xyz, anchor elevation
layout(location = 1) in vec4 point1;
layout(location = 2) in vec4 point2;
layout(location = 3) in vec4 point3;
layout(location = 4) in vec4 shade0; // source light normal.xyz, extra rise
layout(location = 5) in vec4 shade1;
layout(location = 6) in vec4 shade2;
layout(location = 7) in vec4 shade3;
layout(location = 8) in vec4 color0;
layout(location = 9) in vec4 color1;
layout(location = 10) in vec4 color2;
layout(location = 11) in vec4 color3;
layout(location = 12) in vec4 uv01;
layout(location = 13) in vec4 uv23;
layout(location = 14) in vec4 mask_uv01;
layout(location = 15) in vec4 mask_uv23;

layout(set = 1, binding = 0, std140) uniform SurfaceView {
    mat4 matrix;
    vec4 basis[3];
    vec4 radial; // sphere radius, reference height, height scale, extra scale
    vec4 light; // source direction.xyz, ambient
    vec4 material; // diffuse, spherical mode, reserved, reserved
    vec4 rotation;
    vec4 offset_extent;
    vec4 atlas;
    vec4 tint;
    vec4 mask_rect;
    vec4 mask;
};
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 texture_uv;

const float pi = 3.14159265358979323846;
vec2 coordinate(vec3 n) {
    precise float x = n.x * rotation.x + n.z * rotation.y;
    precise float z = n.z * rotation.x - n.x * rotation.y;
    precise float y = n.y * rotation.z + z * rotation.w;
    precise float zz = z * rotation.z - n.y * rotation.w;
    float longitude = x == 0.0 && zz == 0.0 ? 0.0 : atan(zz, x);
    precise vec2 uv = vec2((longitude + pi) / (2.0 * pi), acos(clamp(y, -1.0, 1.0)) / pi);
    uv += offset_extent.xy;
    uv.x -= floor(uv.x);
    return uv;
}

void main() {
    vec4 points[4] = vec4[4](point0, point1, point2, point3);
    vec4 shades[4] = vec4[4](shade0, shade1, shade2, shade3);
    vec4 colors[4] = vec4[4](color0, color1, color2, color3);
    vec2 source_uv[4] = vec2[4](uv01.xy, uv01.zw, uv23.xy, uv23.zw);
    vec2 mask_uv[4] = vec2[4](mask_uv01.xy, mask_uv01.zw, mask_uv23.xy, mask_uv23.zw);
    int corner = gl_VertexIndex;
    vec4 point = points[corner];
    precise vec3 normal;
    for (int row = 0; row < 3; ++row) {
        precise float n = basis[row].y * point.y;
        n = fma(basis[row].x, point.x, n);
        normal[row] = fma(basis[row].z, point.z, n);
    }
    precise float radius = fma(radial.y, radial.z, radial.x);
    precise float rise = (point.w - radial.y) * radial.z;
    rise = rise + shades[corner].w * radial.w;
    precise vec3 world = radius * (normal - vec3(0.0, 0.0, 1.0));
    world = fma(normal, vec3(rise), world);
    precise vec4 clip = matrix[1] * world.y;
    clip = fma(matrix[0], vec4(world.x), clip);
    clip = fma(matrix[2], vec4(world.z), clip);
    clip = clip + matrix[3];
    precise float depth = clip.z * 0.5 + clip.w * 0.5;
    gl_Position = vec4(clip.xy, depth, clip.w);

    if (material.y != 1.0) {
        float brightness = light.w + material.x * max(0.0, dot(shades[corner].xyz, light.xyz));
        vertex_color = vec4(colors[corner].rgb * brightness, colors[corner].a);
        texture_uv = source_uv[corner];
        if (material.y >= 2.0) {
            vec2 d = max(max(mask_rect.xy - mask_uv[corner], mask_uv[corner] - mask_rect.zw), vec2(0.0));
            float t = mask.x == 0.0 ? 1.0 : clamp(length(d) / mask.x, 0.0, 1.0);
            float coverage = t * t * (3.0 - 2.0 * t);
            if (material.y == 2.0) vertex_color *= tint;
            else {
                vertex_color = vec4(tint.rgb, colors[corner].a * tint.a);
                texture_uv = vec2(-1.0);
            }
            vertex_color.a *= coverage;
        }
    } else {
        vec2 uv[4] = vec2[4](coordinate(point0.xyz), coordinate(point1.xyz),
                             coordinate(point2.xyz), coordinate(point3.xyz));
        float minimum = min(min(uv[0].x, uv[1].x), min(uv[2].x, uv[3].x));
        float maximum = max(max(uv[0].x, uv[1].x), max(uv[2].x, uv[3].x));
        for (int i = 0; i < 4; ++i) {
            if (maximum - minimum > 0.5 && uv[i].x < 0.5) uv[i].x += 1.0;
            uv[i].y = clamp(uv[i].y, 0.0, 1.0);
        }
        texture_uv = (atlas.xy + uv[corner] * (atlas.zw - 1.0) + 0.5) / offset_extent.zw;
        vertex_color = tint;
    }
    // Decode with fragment reciprocal W for affine interpolation, including
    // clip intersections crossing the eye plane on Vulkan hardware.
    vertex_color *= clip.w;
    texture_uv *= clip.w;
}
