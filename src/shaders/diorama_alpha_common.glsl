// World transparency inside one uninterrupted native priority band.
// Modes: 0 opaque writers, 1 blended tail in band draw order.
layout(location=2) flat in float v_ordinal;
layout(location=1) out vec4 o_key;
void alpha_filter() {
    if (alpha.options.x == 0.0) return;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(opaque_depth, pixel, 0).r;
    if (gl_FragCoord.z > depth || (gl_FragCoord.z == depth &&
        v_ordinal <= texelFetch(opaque_order, pixel, 0).r)) discard;
}
void alpha_output() {
    if (o_color.a <= .5 / 255.0) discard;
    // Opaque blend replaces destination; additive/read-only draws never own
    // opaque depth. Alpha edges are shaded by exactly the ordinary material.
    bool opaque = alpha.options.w != 0.0 &&
        (alpha.options.z == 0.0 || o_color.a >= 1.0);
    if ((alpha.options.x == 0.0) != opaque) discard;
    o_key = vec4(v_ordinal, 0.0, 0.0, 1.0);
}
