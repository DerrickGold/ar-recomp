#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Native-resolution low/high captures stacked vertically, copied with NONE.
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0) uniform Context {
    vec2 size;
    float high_band;
    float additive;
};

void sample_pair(vec2 p, float weight, inout vec4 low, inout vec4 high) {
    p = clamp(p, vec2(0.0), size - 1.0);
    vec2 uv = (p + 0.5) / size;
    vec4 l = texture(u_texture, vec2(uv.x, uv.y * 0.5));
    vec4 h = texture(u_texture, vec2(uv.x, uv.y * 0.5 + 0.5));
    // Resolve any backing beneath high pixels BEFORE filtering. Accumulate
    // premultiplied color so transparent black never darkens a tile edge.
    low += vec4(l.rgb * l.a, l.a) * (1.0 - h.a) * weight;
    high += vec4(h.rgb * h.a, h.a) * weight;
}

void main() {
    // Same footprint as the ordinary nearest x4 / linear crisp path, without
    // allocating a x4 image. v_uv addresses one capture, not the full atlas.
    vec2 q = v_uv * size * 4.0 - 0.5;
    vec2 b = floor(q);
    vec2 f = fract(q);
    vec4 low = vec4(0.0), high = vec4(0.0);
    sample_pair(floor(b / 4.0), (1.0-f.x)*(1.0-f.y), low, high);
    sample_pair(floor((b+vec2(1,0)) / 4.0), f.x*(1.0-f.y), low, high);
    sample_pair(floor((b+vec2(0,1)) / 4.0), (1.0-f.x)*f.y, low, high);
    sample_pair(floor((b+vec2(1,1)) / 4.0), f.x*f.y, low, high);

    vec4 band = high_band > 0.5 ? high : low;
    vec3 rgb = band.a > 0.0 ? band.rgb / band.a : vec3(0.0);
    float alpha = band.a;
    if (high_band < 0.5 && additive < 0.5) {
        // Separate source-over draws otherwise leave a hole: .5 over .5
        // covers only .75. Condition low coverage on high NOT covering it.
        // Multiplying by (1-high.a) in the later draw then restores low.a.
        float remaining = 1.0 - high.a * v_color.a;
        alpha = remaining > 0.0 ? min(low.a / remaining, 1.0) : 0.0;
    }
    // Additive captures already contain only the subscreen's visible winner.
    // Their premultiplied contributions sum directly; conditioning low alpha
    // as for source-over would brighten the shared boundary a second time.
    o_color = vec4(rgb * v_color.rgb, alpha * v_color.a);
}
