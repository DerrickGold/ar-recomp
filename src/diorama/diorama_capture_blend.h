#ifndef DIORAMA_CAPTURE_BLEND_H
#define DIORAMA_CAPTURE_BLEND_H

#include <stdbool.h>
#include <stdint.h>

/* F4 (2026-07-26 handback: "missing transparency on background layers in diorama
 * mode"). The diorama capture pulls each enabled visual source into its own
 * plane (main-screen rendering preferred, subscreen rendering used when that is
 * the source's only home) and the host composites those planes in depth order.
 * What it does NOT reproduce is arbitrary SNES colour math, so a layer the PPU
 * was half-adding with the subscreen used to arrive fully opaque and HIDE the
 * planes behind it instead of tinting them.
 *
 * (main + sub) / 2 is exactly a 50% source-over of the layer onto whatever is
 * behind it, and the subscreen is what "behind" means here — so the whole effect
 * is reproducible as per-pixel alpha $80, with no shader and no second pass.
 * This decides which layers get that treatment.
 *
 * Measured in Fillmore act 2 (2026-07-27, AR_WS_LAYERS over the whole act):
 * cgwsel=$02 cgadsub=$43 main=$17 sub=$11, stable across all 4952 frames.
 * That decodes to half-add with the subscreen as addend, colour math enabled on
 * BG1 and BG2 -- and only BG2 qualifies, which is the entire subtlety below.
 *
 * Pure: no SDL, no PPU struct, no globals, so the policy is unit-testable
 * without a ROM or a renderer (precedent: diorama_skybox_uv.c). */

/* CGADSUB/CGWSEL bit meanings used here, named so the call site does not read as
 * magic numbers. */
enum {
  kCgwselAddendIsSubscreen = 0x02, /* else the addend is the fixed colour */
  kCgadsubHalf = 0x40,
  kCgadsubSubtract = 0x80,
  kCgadsubLayerMask = 0x3f,
};

/* True when the frame's colour math is the one form reproducible as alpha: a
 * HALF ADD whose addend is the SUBSCREEN.
 *
 * Fails closed on purpose. A full (non-half) add is not an alpha blend at all,
 * and a subtract is not either; both keep the opaque capture rather than being
 * approximated. A fixed-colour addend is a different effect again -- there is no
 * "behind" for it to blend with, so alpha cannot express it. */
bool DioramaCaptureBlend_IsHalfAddWithSubscreen(uint8_t cgwsel, uint8_t cgadsub);

/* True when `layer_bit` (1 << kPpuOverlaySource_*, which matches the CGADSUB and
 * screen-enable bit order for BG1..BG4 and OBJ) should be captured at 50% alpha.
 *
 * Requires all of:
 *   - the frame's math is half-add-with-subscreen;
 *   - colour math is enabled for this layer in CGADSUB;
 *   - the layer is NOT itself on the subscreen. This last one is the subtle one
 *     and it is load-bearing: a layer that is also on the subscreen is being
 *     half-added with ITSELF, which is the identity, so marking it would make an
 *     unchanged layer 50% transparent. In the measured frame this is precisely
 *     what separates BG1 (on the subscreen, unchanged) from BG2 (the water,
 *     genuinely blended over the rock path behind it).
 *
 * OBJ is deliberately not handled here: its colour-math eligibility is per
 * palette group rather than per layer, which kPpuOverlayFlag_MarkObjColorMath
 * already covers. Callers pass BG layers only. */
bool DioramaCaptureBlend_LayerIsHalfAdded(uint8_t cgwsel, uint8_t cgadsub,
                                          uint8_t screen_sub,
                                          uint8_t layer_bit);

/* Return the subscreen sources that are exact full-add inputs to a disjoint
 * main/sub scene. This is the second colour-math form the separated diorama
 * compositor can reproduce: it draws the resolved subscreen winner with
 * saturated additive blending over the main-screen winner.
 *
 * The ownership partition is load-bearing. A source enabled on both screens
 * is rendered through two potentially different window sets, while the PPU's
 * capture scratch stores only one isolated rendering at a time. Failing closed
 * on any overlap keeps the later per-pixel winner resolve exact. Likewise,
 * cgwsel must be exactly $02: direct colour and colour-window clipping/math
 * prevention need more state than an additive plane can encode.
 *
 * `screen_main`/`screen_sub` use the TM/TS bit order. The result uses that same
 * order and contains BG/OBJ source bits only; backdrop is never returned. */
uint8_t DioramaCaptureBlend_FullAddSubscreenSources(
    uint8_t cgwsel, uint8_t cgadsub,
    uint8_t screen_main, uint8_t screen_sub);

/* True when a BG layer's colour math is an exact fixed-colour subtraction
 * that the capture pipeline can bake into the extracted pixels. This is the
 * palette-dimming state measured in Bloodpool act 2 map 4:
 * cgwsel=$00 cgadsub=$81 fixed=$0822 (RGB5 2,1,2), with math on BG1.
 *
 * Requiring cgwsel == 0 excludes subscreen math, direct colour, and colour-
 * window clipping/prevention. Half subtract is excluded because its rounding
 * is a separate operation. A zero fixed colour is a no-op and stays unmarked. */
bool DioramaCaptureBlend_LayerUsesFixedColorSubtract(
    uint8_t cgwsel, uint8_t cgadsub, uint16_t fixed_color,
    uint8_t layer_bit);

#endif /* DIORAMA_CAPTURE_BLEND_H */
