#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer Directions { ivec4 values[]; } input_directions;
layout(std430, set = 1, binding = 0) writeonly buffer Motions { ivec4 values[]; } output_motion;
void main() {
    uint plane = gl_GlobalInvocationID.x;
    if (plane >= 8u) return;
    ivec4 forward = input_directions.values[plane * 2u];
    ivec4 backward = input_directions.values[plane * 2u + 1u];
    bool valid = forward.z != 0 && backward.z != 0 && all(lessThanEqual(abs(forward.xy + backward.xy), ivec2(1)));
    output_motion.values[plane * 2u] = valid ? ivec4(forward.xy, backward.xy) : ivec4(0);
    output_motion.values[plane * 2u + 1u] = ivec4(valid ? 1 : 0, 0, 0, 0);
}
