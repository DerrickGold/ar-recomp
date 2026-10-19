#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D packet;
layout(std140, set = 3, binding = 0) uniform Context { vec4 plane; } settings;
uint word(uint index) {
    uvec4 bytes = uvec4(round(texelFetch(packet, ivec2(index % 256u, index / 256u), 0) * 255.0));
    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}
#include "ppu_bg_capture_common.glsl"
void main() {
    o_color = colorAt(uvec2(v_uv * settings.plane.zw), uint(settings.plane.x), uint(settings.plane.y));
}
