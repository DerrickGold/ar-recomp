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
    vec4 heat_capture; // draw UV to full capture UV (compact/priority sources)
    vec4 heat_source; // capture world origin XY, texture size ZW
    vec4 heat_bounds[4];
    vec4 heat_motion[4];
    vec4 cloud_source;
    vec4 cloud_meta;
    vec4 cloud_regions[24];
    vec4 cloud_background_dim;
};

#define CLOUD_REGIONS cloud_regions
#include "cloud_light.glsl"

// Same world-space waves as DioramaHeat_Uv. Evaluate before filtering, so
// small plumes do not depend on the camera-relative tessellation of a plane.
// Both priority bands sample the same displaced point; actors/HUD stay sharp.
vec2 heat_art_uv(vec2 value) {
    if (heat_source.z <= 0.0 || heat_source.w <= 0.0) return value;
    vec2 world = (value * heat_capture.zw + heat_capture.xy) * heat_source.zw + heat_source.xy;
    float strength = 0.0, phase = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec4 r = heat_bounds[i];
        if (heat_motion[i].x <= 0.0 || r.z <= r.x || r.w <= r.y) continue;
        vec2 p = (world - r.xy) / (r.zw - r.xy);
        if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0) continue;
        float edge = min(1.0, 8.0 * min(min(p.x, 1.0-p.x), min(p.y, 1.0-p.y)));
        float a = heat_motion[i].x * edge * edge * (3.0-2.0*edge);
        if (a > strength) { strength = a; phase = heat_motion[i].y; }
    }
    if (strength <= 0.0) return value;
    vec2 offset = strength * vec2(
        sin(world.y*.12 + phase*3.0) + .4*sin(world.x*.08 - world.y*.05 + phase*2.0),
        .18*sin(world.x*.09 + world.y*.07 + phase*2.0));
    return clamp(value + offset / (heat_source.zw * heat_capture.zw), vec2(0), vec2(1));
}

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
vec4 dof_art(vec2 value) {
    if (filter_info.w > 0.0 && value.y >= filter_info.w) return vec4(0);
    vec4 sum = vec4(0);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec2 uv = value + vec2(x,y) * filter_info.xy * filter_info.z;
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
vec4 priority_art(vec2 value) {
    vec2 q = value * priority_info.xy * 4.0 - .5, b = floor(q), f = fract(q);
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
    vec2 uv = heat_art_uv(v_uv);
    vec4 art = shade.a > 1.5 ? priority_art(uv) :
        shade.a > .5 ? dof_art(uv) : sample_art(uv);
    // Match kActionReceiverMaxIllumination: both art and objects receive up to 4x light.
    vec3 incoming = texture(u_illumination, gl_FragCoord.xy * light_info.xy + light_info.zw).rgb * 4.0;
    vec3 illumination = clamp(v_color.rgb + incoming * shade.rgb, vec3(0), 4.0 * shade.rgb);
    if(cloud_meta.x>0.0)art.rgb=cloud_lighting(art.rgb,uv*heat_capture.zw+heat_capture.xy,cloud_source,cloud_meta,cloud_background_dim);
    o_color = vec4(grade_art(art.rgb) * illumination, art.a * v_color.a);
}
