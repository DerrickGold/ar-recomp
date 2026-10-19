#version 450
#extension GL_GOOGLE_include_directive : require
#include "motion_global_sample.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, set = 1, binding = 0) writeonly uniform image2D output_frame;
void main() {
    int plane = int(gl_GlobalInvocationID.z);
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    int height = int(settings.planes[plane].z);
    if (p.y >= height || p.x >= textureSize(previous_frame, 0).x) return;
    imageStore(output_frame, p + ivec2(0, plane * height), WarpGlobalPixel(p, plane));
}
