#ifndef DIORAMA_CAPTURE_BLEND_H
#define DIORAMA_CAPTURE_BLEND_H

#include <stdbool.h>
#include <stdint.h>

/* F4 (2026-07-26 handback: "missing transparency on background layers in diorama
 * mode"). The diorama capture pulls each enabled main-screen layer into its own
 * plane and the host composites those planes in depth order. What it does NOT
 * reproduce is SNES colour math, so a layer the PPU was half-adding with the
 * subscreen used to arrive fully opaque and HIDE the planes behind it instead of
 * tinting them.
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

#endif /* DIORAMA_CAPTURE_BLEND_H */
