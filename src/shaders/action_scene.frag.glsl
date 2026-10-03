#version 450
layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 texture_uv;
layout(location = 2) in vec2 capture_point;
layout(location = 0) out vec4 output_color;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(std140, set = 3, binding = 0) uniform Material {
    vec4 texture_info; // width, height, textured, finite clip enabled
    vec4 atlas; // source pixel rectangle
    vec4 clip_rect; // capture bounds after motion
} settings;
void main() {
    if (settings.texture_info.w != 0.0 &&
        (any(lessThan(capture_point, settings.clip_rect.xy)) ||
         any(greaterThan(capture_point, settings.clip_rect.zw)))) discard;
    vec4 sample_color = vec4(1.0);
    if (settings.texture_info.z != 0.0) {
        vec2 pixel = settings.atlas.xy + texture_uv * settings.atlas.zw;
        pixel = clamp(pixel, settings.atlas.xy + 0.5, settings.atlas.xy + settings.atlas.zw - 0.5);
        sample_color = texture(image, pixel / settings.texture_info.xy);
    }
    output_color = sample_color * vertex_color;
}
