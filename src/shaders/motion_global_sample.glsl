// Shared sampling/endpoint selection for atlas and direct layer outputs.
layout(std140, set = 2, binding = 0) uniform Settings {
    vec4 phase; // interpolation, input bands, alpha correction, exact map mask
    vec4 planes[8];
    vec4 map_delta; // current minus previous authoritative map camera, pixels
} settings;
layout(set = 0, binding = 0) uniform sampler2D previous_frame;
layout(set = 0, binding = 1) uniform sampler2D current_frame;
layout(std430, set = 0, binding = 2) readonly buffer Motions { ivec4 values[]; } input_motion;

// Native captures are straight alpha. Hardware bilinear filtering mixes RGB
// with transparent padding before later material filtering/blending applies
// alpha, darkening moving cutouts twice. Preserve coverage while filtering,
// then return straight alpha for the existing compositor. The caller keeps
// every tap inside this plane, including atlas edges and narrow captures.
vec4 CoverageSample(sampler2D endpoint, vec2 source, ivec2 size, int plane) {
    vec2 q = clamp(source, vec2(.5), vec2(size) - .5) - .5;
    ivec2 p = ivec2(floor(q)), next = min(p + 1, size - 1);
    ivec2 offset = ivec2(0, plane * size.y);
    vec2 f = fract(q);
    vec4 a = texelFetch(endpoint, p + offset, 0);
    vec4 b = texelFetch(endpoint, ivec2(next.x, p.y) + offset, 0);
    vec4 c = texelFetch(endpoint, ivec2(p.x, next.y) + offset, 0);
    vec4 d = texelFetch(endpoint, next + offset, 0);
    a.rgb *= a.a; b.rgb *= b.a; c.rgb *= c.a; d.rgb *= d.a;
    vec4 color = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    return vec4(color.a > 0.0 ? color.rgb / color.a : vec3(0), color.a);
}

vec4 WarpGlobalPixel(ivec2 p, int plane) {
    ivec2 size = ivec2(settings.planes[plane].xz);
    int atlas_width = textureSize(previous_frame, 0).x;
    if (p.x >= size.x) return vec4(0.0);
    ivec2 dest = p + ivec2(0, plane * size.y);
    if ((uint(settings.phase.w) & (1u << uint(plane))) != 0u) {
        // Do not blur native pixels before the final crisp/DoF material. Both
        // endpoints are expressed in current-camera coordinates; geometry gets
        // the continuous remaining displacement. Newly revealed border pixels
        // have no previous sample, so retain their exact current artwork.
        ivec2 previous = p + ivec2(settings.map_delta.xy);
        if (settings.phase.x < .5 && all(greaterThanEqual(previous, ivec2(0))) &&
            all(lessThan(previous, size)))
            return texelFetch(previous_frame, previous + ivec2(0, plane * size.y), 0);
        return texelFetch(current_frame, dest, 0);
    }
    vec4 color;
    if (input_motion.values[plane * 2 + 1].x == 0) {
        color = texelFetch(current_frame, dest, 0);
    } else if (settings.phase.z > .5 && (settings.phase.x == 0.0 || settings.phase.x == 1.0)) {
        // Retain native endpoint bytes, including RGB under zero alpha.
        color = settings.phase.x == 0.0 ? texelFetch(previous_frame, dest, 0)
                                       : texelFetch(current_frame, dest, 0);
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
            color = settings.phase.z > .5
                ? (forward ? CoverageSample(previous_frame, source, size, plane)
                           : CoverageSample(current_frame, source, size, plane))
                : (forward ? textureLod(previous_frame, uv, 0.0) : textureLod(current_frame, uv, 0.0));
        }
    }
    return color;
}
