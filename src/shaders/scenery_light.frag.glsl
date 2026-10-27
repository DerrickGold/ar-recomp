#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_illumination;
layout(set = 3, binding = 0) uniform Context {
    vec4 filter_info; // source texel XY, DOF radius, lower content boundary
    vec4 priority_info; // capture size XY, high band, additive
    vec4 light_info; // output pixel to illumination UV: scale XY, offset ZW
    vec4 shade; // authored depth tint RGB, material mode
    vec4 grade; // strength, saturation, reserved
    vec4 grade_shadows;
    vec4 grade_highlights;
};

vec3 grade_art(vec3 color) {
    if (grade.x <= 0.0) return color;
    const vec3 weights = vec3(.2126, .7152, .0722);
    float luminance = dot(color, weights);
    vec3 saturated = mix(vec3(luminance), color, grade.y);
    vec3 tone = mix(grade_shadows.rgb, grade_highlights.rgb,
        smoothstep(.06, .60, luminance));
    // Split tone changes hue, not the room's exposure. Black stays black;
    // light is still applied to the original material after this operation.
    float tone_luma = dot(tone, weights);
    tone = tone_luma > .001 ? tone / tone_luma : vec3(1);
    return mix(color, clamp(saturated * tone, vec3(0), vec3(1)), grade.x);
}

// Keep the same premultiplied filtering and complementary priority coverage
// as the ordinary DOF/priority materials. Illumination never changes alpha.
vec4 sample_art(vec2 uv) { return texture(u_texture, uv); }
void accumulate(inout vec4 sum, vec4 c, float weight) {
    sum += vec4(c.rgb * c.a, c.a) * weight;
}
vec4 dof_art() {
    if (filter_info.w > 0.0 && v_uv.y >= filter_info.w) return vec4(0);
    vec4 sum = vec4(0);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec2 uv = v_uv + vec2(x,y) * filter_info.xy * filter_info.z;
        if (filter_info.w > 0.0) uv.y = min(uv.y, filter_info.w - .5 * filter_info.y);
        accumulate(sum, sample_art(uv), x == 0 && y == 0 ? 2.0 : 1.0);
    }
    return vec4(sum.a > .00001 ? sum.rgb / sum.a : vec3(0), sum.a * .1);
}
void pair_tap(vec2 p, float weight, inout vec4 low, inout vec4 high) {
    vec2 uv = (clamp(p, vec2(0), priority_info.xy - 1.0) + .5) / priority_info.xy;
    vec4 l = sample_art(vec2(uv.x, uv.y * .5));
    vec4 h = sample_art(vec2(uv.x, uv.y * .5 + .5));
    accumulate(low, l, (1.0 - h.a) * weight);
    accumulate(high, h, weight);
}
vec4 priority_art() {
    vec2 q = v_uv * priority_info.xy * 4.0 - .5, b = floor(q), f = fract(q);
    vec4 low = vec4(0), high = vec4(0);
    pair_tap(floor(b/4.0), (1.0-f.x)*(1.0-f.y), low, high);
    pair_tap(floor((b+vec2(1,0))/4.0), f.x*(1.0-f.y), low, high);
    pair_tap(floor((b+vec2(0,1))/4.0), (1.0-f.x)*f.y, low, high);
    pair_tap(floor((b+vec2(1,1))/4.0), f.x*f.y, low, high);
    vec4 band = priority_info.z > .5 ? high : low;
    float alpha = band.a;
    if (priority_info.z < .5 && priority_info.w < .5) {
        float remaining = 1.0 - high.a * v_color.a;
        alpha = remaining > 0.0 ? min(alpha / remaining, 1.0) : 0.0;
    }
    return vec4(band.a > 0.0 ? band.rgb / band.a : vec3(0), alpha);
}
void main() {
    vec4 art = shade.a > 1.5 ? priority_art() :
        shade.a > .5 ? dof_art() : sample_art(v_uv);
    // Match kActionReceiverMaxIllumination: both art and objects receive up to 4x light.
    vec3 incoming = texture(u_illumination, gl_FragCoord.xy * light_info.xy + light_info.zw).rgb * 4.0;
    vec3 illumination = clamp(v_color.rgb + incoming * shade.rgb, vec3(0), 4.0 * shade.rgb);
    o_color = vec4(grade_art(art.rgb) * illumination, art.a * v_color.a);
}
