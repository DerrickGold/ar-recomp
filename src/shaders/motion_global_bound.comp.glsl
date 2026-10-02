#version 450
// Nine nearby displacements establish an exact upper bound for pruning. The fixed
// interior sample grid match PresentationFrameGeneration_Analyze's global path.
layout(local_size_x = 64) in;
layout(std140, set = 2, binding = 0) uniform Extents { vec4 planes[8]; } extents;
layout(set = 0, binding = 0) uniform sampler2D previous_frame;
layout(set = 0, binding = 1) uniform sampler2D current_frame;
layout(std430, set = 1, binding = 0) writeonly buffer Costs { uint costs[]; } output_costs;
shared uint partial[64];
void main() {
    uint lane = gl_LocalInvocationIndex, candidate = gl_WorkGroupID.x;
    uint direction = gl_WorkGroupID.y, plane = direction / 2u;
    if (extents.planes[plane].y == 0.0) return;
    ivec2 size = textureSize(previous_frame, 0); size.y = int(extents.planes[plane].z);
    size.x = int(extents.planes[plane].x);
    ivec2 motion = ivec2(int(candidate % 3u) - 1, int(candidate / 3u) - 1);
    int columns = max(0, (size.x - 14 + 15) / 16);
    int rows = max(0, (size.y - 14 + 15) / 16);
    uint sum = 0u;
    for (uint i = lane; i < uint(columns * rows); i += 64u) {
        ivec2 p = ivec2(7 + int(i) % columns * 16, 7 + int(i) / columns * 16 + int(plane) * size.y);
        ivec4 a, b;
        if ((direction & 1u) == 0u) {
            a = ivec4(round(texelFetch(previous_frame, p, 0) * 255.0));
            b = ivec4(round(texelFetch(current_frame, p + motion, 0) * 255.0));
        } else {
            a = ivec4(round(texelFetch(current_frame, p, 0) * 255.0));
            b = ivec4(round(texelFetch(previous_frame, p + motion, 0) * 255.0));
        }
        ivec4 d = abs(a - b);
        sum += uint(d.x + d.y + d.z + d.w * 2);
    }
    partial[lane] = sum; barrier();
    for (uint stride = 32u; stride != 0u; stride >>= 1u) {
        if (lane < stride) partial[lane] += partial[lane + stride];
        barrier();
    }
    if (lane == 0u) output_costs.costs[direction * 9u + candidate] =
        partial[0] + uint(abs(motion.x) + abs(motion.y)) * 12u;
}
