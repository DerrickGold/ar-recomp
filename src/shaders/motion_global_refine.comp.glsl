#version 450
layout(local_size_x = 64) in;
layout(std140, set = 2, binding = 0) uniform Extents { vec4 planes[8]; } extents;
layout(set = 0, binding = 0) uniform sampler2D previous_frame;
layout(set = 0, binding = 1) uniform sampler2D current_frame;
layout(std430, set = 0, binding = 2) readonly buffer Costs { uint costs[]; } input_costs;
layout(std430, set = 1, binding = 0) writeonly buffer Directions { ivec4 values[]; } output_directions;
shared ivec2 coarse;
shared uint partial[640];
bool better(uint cost, ivec2 motion, uint best_cost, ivec2 best) {
    if (cost != best_cost) return cost < best_cost;
    int a = abs(motion.x) + abs(motion.y), b = abs(best.x) + abs(best.y);
    if (a != b) return a < b;
    if (motion.y != best.y) return motion.y < best.y;
    return motion.x < best.x;
}
void main() {
    uint lane = gl_LocalInvocationIndex, direction = gl_WorkGroupID.x;
    uint plane = direction / 2u;
    // No stale costs/directions from a previous pair may escape a skipped band.
    if (extents.planes[plane].y == 0.0) {
        if (lane == 0u) output_directions.values[direction] = ivec4(0);
        return;
    }
    ivec2 size = textureSize(previous_frame, 0); size.y = int(extents.planes[plane].z);
    size.x = int(extents.planes[plane].x);
    if (lane == 0u) {
        uint best_cost = 0xffffffffu; ivec2 best = ivec2(0);
        for (uint candidate = 0u; candidate < 225u; ++candidate) {
            ivec2 motion = ivec2(int(candidate % 15u) - 7, int(candidate / 15u) - 7);
            uint cost = input_costs.costs[direction * 225u + candidate];
            if (better(cost, motion, best_cost, best)) { best = motion; best_cost = cost; }
        }
        coarse = best;
    }
    barrier();
    int columns = max(0, (size.x - 14 + 7) / 8), rows = max(0, (size.y - 14 + 7) / 8);
    for (uint candidate = 0u; candidate < 10u; ++candidate) {
        ivec2 motion = candidate == 9u ? ivec2(0) :
            clamp(coarse + ivec2(int(candidate % 3u) - 1, int(candidate / 3u) - 1), ivec2(-7), ivec2(7));
        uint sum = 0u;
        for (uint i = lane; i < uint(columns * rows); i += 64u) {
            ivec2 p = ivec2(7 + int(i) % columns * 8, 7 + int(i) / columns * 8 + int(plane) * size.y);
            ivec4 a, b;
            if ((direction & 1u) == 0u) {
                a = ivec4(round(texelFetch(previous_frame, p, 0) * 255.0));
                b = ivec4(round(texelFetch(current_frame, p + motion, 0) * 255.0));
            } else {
                a = ivec4(round(texelFetch(current_frame, p, 0) * 255.0));
                b = ivec4(round(texelFetch(previous_frame, p + motion, 0) * 255.0));
            }
            ivec4 d = abs(a - b); sum += uint(d.x + d.y + d.z + d.w * 2);
        }
        partial[candidate * 64u + lane] = sum;
    }
    barrier();
    for (uint stride = 32u; stride != 0u; stride >>= 1u) {
        if (lane < stride) for (uint c = 0u; c < 10u; ++c)
            partial[c * 64u + lane] += partial[c * 64u + lane + stride];
        barrier();
    }
    if (lane == 0u) {
        uint best_cost = 0xffffffffu; ivec2 best = ivec2(0);
        for (uint candidate = 0u; candidate < 9u; ++candidate) {
            ivec2 motion = clamp(coarse + ivec2(int(candidate % 3u) - 1, int(candidate / 3u) - 1), ivec2(-7), ivec2(7));
            uint cost = partial[candidate * 64u] + uint(abs(motion.x) + abs(motion.y)) * 12u;
            if (better(cost, motion, best_cost, best)) { best = motion; best_cost = cost; }
        }
        uint stationary = partial[9u * 64u];
        // Maximum cost at 640x352 is small enough that the product fits uint32.
        uint required = max(64u, (stationary * 12u + 99u) / 100u);
        bool reliable = any(notEqual(best, ivec2(0))) && best_cost < stationary && stationary - best_cost >= required;
        output_directions.values[direction] = reliable ? ivec4(best, 1, 0) : ivec4(0);
    }
}
