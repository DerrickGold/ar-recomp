// Shared exact-depth placement and spherical coordinates for ordinary radial
// surfaces and batched receivers. Keep operation order/FMA identical so their
// independently compiled pipelines test against the same opaque depth.
vec4 surface_clip(vec4 point, vec4 shade, mat4 matrix, vec4 basis[3], vec4 radial) {
    precise vec3 normal;
    for (int row = 0; row < 3; ++row) {
        precise float n = basis[row].y * point.y;
        n = fma(basis[row].x, point.x, n);
        normal[row] = fma(basis[row].z, point.z, n);
    }
    precise float radius = fma(radial.y, radial.z, radial.x);
    precise float rise = (point.w - radial.y) * radial.z;
    rise = rise + shade.w * radial.w;
    precise vec3 world = radius * (normal - vec3(0.0, 0.0, 1.0));
    world = fma(normal, vec3(rise), world);
    precise vec4 clip = matrix[1] * world.y;
    clip = fma(matrix[0], vec4(world.x), clip);
    clip = fma(matrix[2], vec4(world.z), clip);
    clip = clip + matrix[3];
    return clip;
}

vec2 surface_coordinate(vec3 n, vec4 shadow_basis[3], float use_basis, vec4 rotation) {
    const float pi = 3.14159265358979323846;
    if (use_basis != 0.0) {
        n = vec3(dot(shadow_basis[0].xyz,n), dot(shadow_basis[1].xyz,n), dot(shadow_basis[2].xyz,n));
    }
    precise float x = n.x * rotation.x + n.z * rotation.y;
    precise float z = n.z * rotation.x - n.x * rotation.y;
    precise float y = n.y * rotation.z + z * rotation.w;
    precise float zz = z * rotation.z - n.y * rotation.w;
    float longitude = x == 0.0 && zz == 0.0 ? 0.0 : atan(zz, x);
    precise vec2 uv = vec2((longitude + pi) / (2.0 * pi), acos(clamp(y, -1.0, 1.0)) / pi);
    return uv;
}
