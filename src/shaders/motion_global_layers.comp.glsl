#version 450
#extension GL_GOOGLE_include_directive : require
#include "motion_global_sample.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, set = 1, binding = 0) writeonly uniform image2D output_0;
layout(rgba8, set = 1, binding = 1) writeonly uniform image2D output_1;
layout(rgba8, set = 1, binding = 2) writeonly uniform image2D output_2;
layout(rgba8, set = 1, binding = 3) writeonly uniform image2D output_3;
layout(rgba8, set = 1, binding = 4) writeonly uniform image2D output_4;
layout(rgba8, set = 1, binding = 5) writeonly uniform image2D output_5;
layout(rgba8, set = 1, binding = 6) writeonly uniform image2D output_6;
layout(rgba8, set = 1, binding = 7) writeonly uniform image2D output_7;
void main() {
    int plane = int(gl_GlobalInvocationID.z);
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (settings.planes[plane].y == 0.0 ||
        any(greaterThanEqual(p, ivec2(settings.planes[plane].xz)))) return;
    vec4 color = WarpGlobalPixel(p, plane);
    ivec2 dest = p + ivec2(int(settings.planes[plane].w), 0);
    // Explicit bindings avoid descriptor indexing and nonuniform resource
    // array requirements. Every workgroup follows one branch (plane = z).
    switch (plane) {
        case 0: imageStore(output_0, dest, color); break;
        case 1: imageStore(output_1, dest, color); break;
        case 2: imageStore(output_2, dest, color); break;
        case 3: imageStore(output_3, dest, color); break;
        case 4: imageStore(output_4, dest, color); break;
        case 5: imageStore(output_5, dest, color); break;
        case 6: imageStore(output_6, dest, color); break;
        case 7: imageStore(output_7, dest, color); break;
    }
}
