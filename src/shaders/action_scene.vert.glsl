#version 450
layout(location = 0) in vec2 source_position;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 texture_uv;
layout(location = 2) out vec2 capture_point;
layout(std430, set = 0, binding = 0) readonly buffer Motion { ivec4 values[]; } motions;
layout(std140, set = 1, binding = 0) uniform Projection {
    mat4 matrix;
    vec4 viewport;
    vec4 target_capture; // output width/height, capture texture width/height
    vec4 plane_uv;
    vec4 shape; // aspect, height scale, depth, rake
    vec4 curve; // bow, world Y offset, capture X origin, skybox mapping
    vec4 fold; // enabled, start T, height, overlap
    vec4 fold_end; // handoff Z, front Z, front drop, reserved
    vec4 sky_rect;
    vec4 sky_motion; // output Y bounds, phase, global motion slot (-1 disables)
    vec4 offset; // static capture offset XY
} settings;

float row_depth(float t) {
    t = clamp(t, 0.0, 1.0);
    return settings.shape.z + settings.shape.w * t + settings.curve.x * t * t;
}

void main() {
    vec2 displacement = settings.offset.xy;
    int slot = int(settings.sky_motion.w);
    if (slot >= 0 && motions.values[slot * 2 + 1].x != 0) {
        vec4 m = vec4(motions.values[slot * 2]);
        float phase = settings.sky_motion.z;
        // Coordinates belong to the current capture, even when the earlier
        // endpoint supplies the animation pose. Match PlaneOffset exactly.
        displacement += phase < 0.5 ? m.zw + m.xy * phase : m.zw * (1.0 - phase);
    }
    capture_point = source_position + displacement;
    precise vec2 pixel;
    if (settings.curve.w != 0.0) {
        vec2 q = (capture_point - settings.sky_rect.xy) /
            (settings.sky_rect.zw - settings.sky_rect.xy);
        pixel = settings.viewport.xy + vec2(q.x,
            settings.sky_motion.x + q.y * (settings.sky_motion.y - settings.sky_motion.x)) * settings.viewport.zw;
    } else {
        vec2 uv_point = (capture_point + vec2(settings.curve.z, 0.0)) / settings.target_capture.zw;
        vec2 st = (uv_point - settings.plane_uv.xy) / (settings.plane_uv.zw - settings.plane_uv.xy);
        float y = (0.5 - st.y) * settings.shape.y;
        float z = row_depth(st.y);
        if (settings.fold.x != 0.0 && st.y > settings.fold.y) {
            float t = clamp((st.y - settings.fold.y) * settings.shape.y / settings.fold.z, 0.0, 1.0);
            float overlap = clamp(settings.fold.w, 0.0, 1.0);
            float y_top = (0.5 - settings.fold.y) * settings.shape.y;
            float z_top = row_depth(settings.fold.y);
            float linear_y = y_top - settings.fold.z * t;
            if (overlap >= 1.0 || t <= overlap) {
                float q = overlap > 0.0 ? t / overlap : 0.0;
                y = linear_y;
                z = z_top + (settings.fold_end.x - z_top) * q;
            } else {
                float u = (t - overlap) / (1.0 - overlap);
                float bend = u * u * (3.0 - 2.0 * u);
                float front_y = y_top - settings.fold.z * overlap - settings.fold_end.z * u;
                y = linear_y + (front_y - linear_y) * bend;
                z = settings.fold_end.x + (settings.fold_end.y - settings.fold_end.x) * bend;
            }
        }
        precise float wx = (st.x - 0.5) * settings.shape.x;
        precise float wy = y + settings.curve.y;
        precise vec4 clip = settings.matrix[0] * wx + settings.matrix[1] * wy +
            settings.matrix[2] * z + settings.matrix[3];
        if (clip.w <= 0.0001) {
            gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
            vertex_color = vec4(0.0);
            texture_uv = uv;
            return;
        }
        pixel = settings.viewport.xy + vec2(clip.x / clip.w * 0.5 + 0.5,
            1.0 - (clip.y / clip.w * 0.5 + 0.5)) * settings.viewport.zw;
    }
    // Retain the shipped affine interpolation across each mesh triangle.
    // Keeping clip W here would silently introduce perspective-correct UVs.
    gl_Position = vec4(pixel / settings.target_capture.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    vertex_color = color;
    texture_uv = uv;
}
