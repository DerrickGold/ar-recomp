#version 450

/* CRT post-process: one fullscreen pass over the finished frame.
 *
 * Applied at the very end of presentation, after PresentComposite has drawn
 * whichever mode is live, so flat 2D, diorama, sim3D and world-navigation all
 * get it from a single integration point.
 *
 * The pass runs over the WHOLE render target, but the game image occupies only
 * the letterboxed viewport inside it, so everything geometric works in "image
 * space" — the image rect is passed in and uv is remapped into it. Skipping
 * that would stretch the curvature across the black bars and put the scanline
 * count against the window height instead of the picture.
 *
 * Two different pitches are in play, and mixing them up is the classic way
 * these shaders end up looking wrong:
 *
 *   - SCANLINES follow the SOURCE, because they are a property of the signal:
 *     the SNES draws 224 lines whatever the window size, so the beam profile
 *     is computed from `scan_lines` over image space.
 *   - The APERTURE MASK follows OUTPUT pixels, because the phosphor stripes are
 *     a property of the physical glass. It is also sampled on the UNCURVED
 *     position: the mask sits on the tube surface, so it must not warp with
 *     the image.
 *
 * Taking the mask pitch from logical rather than output pixels is what makes
 * CRT shaders shimmer into moiré at non-integer scales — and non-integer is the
 * normal case here (the 7:6 kPixelAspect_Crt43 stretch, plus a widescreen width
 * that varies with the window).
 *
 * The uniform block is all scalars, mirroring CrtUniforms in crt_post.c
 * field-for-field. Packing any pair into a vec2 would shift every later member.
 *
 * SDL binding convention (SDL_gpu.h "Shader Resources"): fragment stage uses
 * set 2 for sampled textures and set 3 for uniform buffers; SDL's render
 * pipeline supplies COLOR0 at location 0 and TEXCOORD0 at location 1. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Context {
    float output_w;        /* render-target size, physical pixels             */
    float output_h;
    float image_x;         /* game image rect inside the target, same units   */
    float image_y;
    float image_w;
    float image_h;
    float scan_lines;      /* source scanline count (224 for the SNES)        */
    float curvature;       /* barrel amount; 0 = flat glass                   */
    float scanline_depth;  /* 0..1 beam darkening between lines               */
    float mask_strength;   /* 0..1 aperture-grille tint                       */
    float aberration;      /* RGB split, in output pixels                     */
    float vignette;        /* 0..1 corner falloff                             */
    float brightness;      /* compensates the darkening mask+scanlines cause  */
};

const float kPi = 3.14159265358979;

/* Classic barrel warp, in image space. The asymmetric divisor makes the
 * horizontal bow a little gentler than the vertical, which is how real tubes
 * read. */
vec2 curve(vec2 uv) {
    if (curvature <= 0.0) return uv;
    vec2 c = uv * 2.0 - 1.0;
    vec2 offset = abs(c.yx) / vec2(6.0, 4.0);
    c += c * offset * offset * curvature;
    return c * 0.5 + 0.5;
}

void main() {
    vec2 target = vec2(output_w, output_h);
    vec2 origin = vec2(image_x, image_y);
    vec2 extent = vec2(image_w, image_h);

    vec2 pixel = v_uv * target;          /* position in the render target     */
    vec2 image = (pixel - origin) / extent;  /* 0..1 across the game picture  */

    vec2 warped = curve(image);

    /* Outside the warped picture is bezel, not clamped edge pixels — without
     * this the border smears the outermost row/column around the curve. */
    if (warped.x < 0.0 || warped.x > 1.0 || warped.y < 0.0 || warped.y > 1.0) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    /* Back to texture space to sample the composited frame. */
    vec2 uv = (origin + warped * extent) / target;

    /* Chromatic aberration: split the red and blue taps horizontally. */
    vec3 col;
    if (aberration > 0.0) {
        float dx = aberration / output_w;
        col.r = texture(u_texture, uv + vec2(dx, 0.0)).r;
        col.g = texture(u_texture, uv).g;
        col.b = texture(u_texture, uv - vec2(dx, 0.0)).b;
    } else {
        col = texture(u_texture, uv).rgb;
    }

    /* Beam profile across each source scanline: brightest at the line centre,
     * falling toward the gap. */
    if (scanline_depth > 0.0) {
        float beam = sin(fract(warped.y * scan_lines) * kPi);
        col *= mix(1.0, beam, scanline_depth);
    }

    /* Aperture grille on the glass: uncurved, at output-pixel pitch. */
    if (mask_strength > 0.0) {
        float stripe = mod(pixel.x, 3.0);
        vec3 phosphor = stripe < 1.0 ? vec3(1.0, 0.4, 0.4)
                      : stripe < 2.0 ? vec3(0.4, 1.0, 0.4)
                                     : vec3(0.4, 0.4, 1.0);
        col *= mix(vec3(1.0), phosphor, mask_strength);
    }

    if (vignette > 0.0) {
        vec2 d = warped * (1.0 - warped);
        float v = clamp(d.x * d.y * 16.0, 0.0, 1.0);
        col *= mix(1.0, pow(v, 0.25), vignette);
    }

    col *= brightness;

    o_color = vec4(col, 1.0) * v_color;
}
