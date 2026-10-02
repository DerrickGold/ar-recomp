#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(std140, set = 2, binding = 0) uniform Settings { vec4 phase; vec4 planes[8]; } settings;
layout(set = 0, binding = 0) uniform sampler2D previous_frame;
layout(set = 0, binding = 1) uniform sampler2D current_frame;
layout(std430, set = 0, binding = 2) readonly buffer Motions { ivec4 values[]; } input_motion;
layout(rgba8, set = 1, binding = 0) writeonly uniform image2D output_frame;
void main() {
    int plane = int(gl_GlobalInvocationID.z);
    ivec2 size = textureSize(previous_frame, 0); size.y = int(settings.planes[plane].z);
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    int atlas_width = size.x;
    size.x = int(settings.planes[plane].x);
    if (p.y >= size.y || p.x >= atlas_width) return;
    if (p.x >= size.x) {
        imageStore(output_frame, p + ivec2(0, plane * size.y), vec4(0.0));
        return;
    }
    ivec2 dest = p + ivec2(0, plane * size.y);
    vec4 color;
    if (input_motion.values[plane * 2 + 1].x == 0) {
        color = texelFetch(current_frame, dest, 0);
    } else {
        bool forward = settings.phase.x < 0.5;
        ivec4 motion = input_motion.values[plane * 2];
        vec2 offset = forward ? vec2(motion.xy) * settings.phase.x : vec2(motion.zw) * (1.0 - settings.phase.x);
        vec2 source = vec2(p) + 0.5 - offset;
        color = vec4(0.0);
        if (all(greaterThanEqual(source, vec2(0.0))) && all(lessThan(source, vec2(size)))) {
            // Clamp within this plane's own edge; never bleed into the adjacent
            // band in the atlas. Keep nearest-endpoint animation pose ownership.
            vec2 uv = (clamp(source, vec2(0.5), vec2(size) - 0.5) + vec2(0.0, plane * size.y)) / vec2(atlas_width, size.y * int(settings.phase.y));
            color = forward ? textureLod(previous_frame, uv, 0.0) : textureLod(current_frame, uv, 0.0);
        }
    }
    imageStore(output_frame, dest, color);
}
