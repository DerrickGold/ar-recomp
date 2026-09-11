#version 450

// One retained quad per instance. Only wind/atlas/tint vary between samples.
// Projected positions deliberately keep w=1: native art and CPU clipping use
// affine screen-space UVs, not perspective-correct world-space interpolation.
layout(location = 0) in vec4 position0;
layout(location = 1) in vec4 position1;
layout(location = 2) in vec4 position2;
layout(location = 3) in vec4 position3;
layout(location = 4) in vec4 normal0; // w = original triangle, or zero
layout(location = 5) in vec4 normal1;
layout(location = 6) in vec4 normal2;
layout(location = 7) in vec4 normal3;
layout(location = 8) in vec4 weights01;
layout(location = 9) in vec4 weights23;

layout(set = 1, binding = 0, std140) uniform Sample {
    vec4 rotation;
    vec4 offset_extent; // offset.xy, full atlas dimensions.zw
    vec4 atlas; // chart origin.xy and dimensions.zw, in texels
    vec4 color;
} sample_data;

layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 texture_uv;

const float pi = 3.14159265358979323846;

vec2 coordinate(vec3 n) {
    precise float x = n.x * sample_data.rotation.x + n.z * sample_data.rotation.y;
    precise float z = n.z * sample_data.rotation.x - n.x * sample_data.rotation.y;
    precise float y = n.y * sample_data.rotation.z + z * sample_data.rotation.w;
    precise float zz = z * sample_data.rotation.z - n.y * sample_data.rotation.w;
    // GLSL atan(0,0) is undefined. The spherical pole has no preferred
    // longitude; match the CPU's atan2(+0,+0) convention explicitly.
    float longitude = x == 0.0 && zz == 0.0 ? 0.0 : atan(zz, x);
    precise vec2 uv = vec2((longitude + pi) / (2.0 * pi),
                          acos(clamp(y, -1.0, 1.0)) / pi);
    uv += sample_data.offset_extent.xy;
    uv.x -= floor(uv.x);
    return uv;
}

void main() {
    vec4 positions[4] = vec4[4](position0, position1, position2, position3);
    vec2 weights[4] = vec2[4](weights01.xy, weights01.zw, weights23.xy, weights23.zw);
    vec2 uv[4] = vec2[4](coordinate(normal0.xyz), coordinate(normal1.xyz),
                         coordinate(normal2.xyz), coordinate(normal3.xyz));
    float minimum = min(min(uv[0].x, uv[1].x), min(uv[2].x, uv[3].x));
    float maximum = max(max(uv[0].x, uv[1].x), max(uv[2].x, uv[3].x));
    for (int i = 0; i < 4; ++i) {
        if (maximum - minimum > 0.5 && uv[i].x < 0.5) uv[i].x += 1.0;
        uv[i].y = clamp(uv[i].y, 0.0, 1.0);
        uv[i] = (sample_data.atlas.xy + uv[i] * (sample_data.atlas.zw - 1.0) + 0.5)
            / sample_data.offset_extent.zw;
    }
    int corner = gl_VertexIndex;
    int triangle = int(normal0.w);
    precise vec2 mapped = uv[corner];
    if (triangle != 0) {
        mapped = uv[0];
        mapped += weights[corner].x * (uv[triangle] - uv[0]);
        mapped += weights[corner].y * (uv[triangle + 1] - uv[0]);
    }
    gl_Position = positions[corner];
    vertex_color = sample_data.color;
    texture_uv = mapped;
}
