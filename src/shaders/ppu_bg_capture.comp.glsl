#version 450
#extension GL_GOOGLE_include_directive : require
// The packet layout and color semantics match ppu_bg_capture.frag.glsl.
// Read integer words directly instead of unpacking them from an RGBA texture.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer Packet { uint words[]; } packet;
layout(rgba8, set = 1, binding = 0) writeonly uniform image2D lowBand;
layout(rgba8, set = 1, binding = 1) writeonly uniform image2D highBand;
layout(rgba8, set = 1, binding = 2) writeonly uniform image2D farBand;
layout(std140, set = 2, binding = 0) uniform Context { uvec4 plane; } settings;
uint word(uint index) { return packet.words[index]; }
#include "ppu_bg_capture_common.glsl"
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(p, settings.plane.zw))) return;
    uint source = settings.plane.x, bands = settings.plane.y;
    if ((bands & 1u) != 0u) imageStore(lowBand, ivec2(p), colorAt(p, source, 0u));
    if ((bands & 2u) != 0u) imageStore(highBand, ivec2(p), colorAt(p, source, 1u));
    if ((bands & 4u) != 0u) imageStore(farBand, ivec2(p), colorAt(p, source, 2u));
}
