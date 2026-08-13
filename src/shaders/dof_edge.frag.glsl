#version 450

/* Depth of field + parallax-aware edge AA, COMBINED — deliberately one shader.
 *
 * Both effects target the SAME layer set (BG1/BG2 and their priority-split
 * halves), and SDL allows only ONE custom fragment shader bound per draw call.
 * An earlier version picked edge AA over DOF whenever both were enabled, which
 * — since both default on — meant DOF silently never rendered at all. Doing
 * both in one pass fixes that, and each knob is independently zeroable:
 * blur_radius = 0 makes the box blur a no-op (all 9 taps land on the same
 * texel), edge_feather <= 0 skips the fade. So this one shader correctly
 * serves DOF-only, edge-AA-only, both, or neither.
 *
 * The uniform block is all scalars on purpose: it mirrors DofEdgeUniforms in
 * diorama.c field-for-field, so the std140 offsets and the C struct offsets
 * agree. Packing a pair into a vec2 here would shift every later member.
 *
 * SDL binding convention (SDL_gpu.h "Shader Resources"): fragment stage uses
 * set 2 for sampled textures and set 3 for uniform buffers; SDL's render
 * pipeline supplies COLOR0 at location 0 and TEXCOORD0 at location 1. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Context {
    float texel_w;
    float texel_h;
    float blur_radius;
    float u_min;
    float u_max;
    float v_min;
    float v_max;
    float edge_feather;
    float lower_content_v_max;
};

/* Layer captures use straight alpha for SDL_BLENDMODE_BLEND, but filtering
 * straight RGB through transparent texels creates dark fringes: transparent
 * capture padding has RGB=0, so averaging it directly contaminates the color
 * before the fragment alpha reveals the already-rendered layer underneath.
 * Accumulate premultiplied taps, then convert the filtered result back to
 * straight alpha for SDL's blend mode. Opaque interiors and radius=0 remain
 * algebraically unchanged. */
void accumulate_tap(inout vec4 sum, vec4 tap, float weight) {
    sum.rgb += tap.rgb * tap.a * weight;
    sum.a += tap.a * weight;
}

/* The raw plane texture is taller than the current captured content. At an
 * ordinary free edge, sampling that transparent padding is useful to soften
 * the silhouette. An attached waterfall continuation owns the lower edge,
 * however, so revealing padding there produces a colored horizontal band from
 * the repeated pixels underneath. Pin lower taps on the drawable side to its
 * final texel centre, while fragments in intentionally clipped rows remain
 * transparent for the extension to own. Extension UVs remain inside the
 * drawable limit and are unchanged. */
vec4 sample_layer(vec2 uv, vec2 center_uv) {
    if (lower_content_v_max > 0.0 && center_uv.y < lower_content_v_max) {
        uv.y = min(uv.y, lower_content_v_max - 0.5 * texel_h);
    }
    return texture(u_texture, uv);
}

void main() {
    vec2 uv = v_uv;
    /* Rows after the policy edge are not a soft silhouette: they are an
     * intentional ownership handoff to the attached geometry. Letting the
     * blur kernel reach backward from those transparent host fragments would
     * recreate the very band that the drawable-side clamp removes. */
    if (lower_content_v_max > 0.0 && uv.y >= lower_content_v_max) {
        o_color = vec4(0.0);
        return;
    }
    vec2 offset = vec2(texel_w, texel_h) * blur_radius;

    vec4 sum = vec4(0.0);
    accumulate_tap(sum, sample_layer(uv + vec2(-offset.x, -offset.y), uv), 1.0);
    accumulate_tap(sum, sample_layer(uv + vec2( 0.0,      -offset.y), uv), 1.0);
    accumulate_tap(sum, sample_layer(uv + vec2( offset.x, -offset.y), uv), 1.0);
    accumulate_tap(sum, sample_layer(uv + vec2(-offset.x,  0.0),      uv), 1.0);
    accumulate_tap(sum, sample_layer(uv,                              uv), 2.0);
    accumulate_tap(sum, sample_layer(uv + vec2( offset.x,  0.0),      uv), 1.0);
    accumulate_tap(sum, sample_layer(uv + vec2(-offset.x,  offset.y), uv), 1.0);
    accumulate_tap(sum, sample_layer(uv + vec2( 0.0,       offset.y), uv), 1.0);
    accumulate_tap(sum, sample_layer(uv + vec2( offset.x,  offset.y), uv), 1.0);
    float filtered_alpha = sum.a / 10.0;
    vec3 filtered_rgb = sum.a > 0.00001 ? sum.rgb / sum.a : vec3(0.0);
    vec4 c = vec4(filtered_rgb, filtered_alpha);

    /* Feather the layer's TRUE UV edge, not the screen edge — these planes are
     * tilted, so the geometric border lands at an arbitrary screen angle. */
    float fade = 1.0;
    if (edge_feather > 0.0) {
        float du = min(uv.x - u_min, u_max - uv.x);
        float dv_top = uv.y - v_min;
        /* An attached continuation can own the lower boundary. Do not fade the
         * host into the backdrop there: its UV-sized feather changes apparent
         * width under perspective even though both meshes use the same MVP. */
        float dv_bottom = lower_content_v_max > 0.0 ? 1.0 : v_max - uv.y;
        float dv = min(dv_top, dv_bottom);
        float d = min(du, dv);
        float texel_avg = (texel_w + texel_h) * 0.5;
        fade = clamp(d / (texel_avg * edge_feather), 0.0, 1.0);
    }

    o_color = vec4(c.rgb * v_color.rgb, c.a * fade * v_color.a);
}
