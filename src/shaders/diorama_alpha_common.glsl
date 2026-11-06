// Bounded world transparency inside one uninterrupted native priority band.
// Modes: 0 opaque, 1 nearest remaining fractional layer, 2 painter tail.
layout(location=2) flat in float v_ordinal;
layout(location=1) out vec4 o_key;
void alpha_filter() {
    if (alpha.options.x == 0.0) return;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(opaque_depth, pixel, 0).r;
    if (gl_FragCoord.z > depth || (gl_FragCoord.z == depth &&
        v_ordinal <= texelFetch(opaque_order, pixel, 0).r)) discard;
    if (alpha.options.y != 0.0) {
        vec4 previous = texelFetch(previous_key, pixel, 0);
        if (previous.x < 0.0 || gl_FragCoord.z < previous.x ||
            (gl_FragCoord.z == previous.x && v_ordinal >= previous.y)) discard;
    }
}
void alpha_output() {
    if (o_color.a <= .5 / 255.0) discard;
    // Opaque blend replaces destination; additive/read-only draws never own
    // opaque depth. Alpha edges are shaded by exactly the ordinary material.
    bool opaque = alpha.options.w != 0.0 &&
        (alpha.options.z == 0.0 || o_color.a >= 1.0);
    if ((alpha.options.x == 0.0) != opaque) discard;
    o_key = vec4(gl_FragCoord.z, v_ordinal, 0.0, 1.0);
    if (alpha.options.x == 0.0) o_key.x = v_ordinal;
    if (alpha.options.x == 1.0) {
        o_color.rgb *= o_color.a;
        if (alpha.options.z == 2.0) o_color.a = 0.0;
    }
}
