#ifndef DIORAMA_SCROLL_MATH_H
#define DIORAMA_SCROLL_MATH_H
#include "present.h"   /* FrameSlot, DioramaScrollSnapshot, DioramaScrollDelta */

/* R17/C4: "this present carries no sub-tick phase" — screenshots, the headless
 * path, and the paused/menu keep-alive, none of which sit at a meaningful point
 * between two ticks. Deliberately NOT 0.0f, which is a perfectly legal phase
 * (the instant a tick lands) and would silently conflate "no phase" with
 * "phase zero". */
static const float kInterpPhaseNone = -1.0f;

/* Pure interpolation math. `alpha` is the main loop's sub-tick phase —
 * accumulator/kFrameNs, in [0,1) — or kInterpPhaseNone. Takes no clock: the
 * phase is passed in rather than reconstructed from SDL_GetTicksNS() against
 * an EMA of the tick period, which is what made this function corruptible by
 * non-tick presents (R16) and prone to saturating at 1.0. */
DioramaScrollDelta ComputeDioramaScrollDeltaAt(
    const FrameSlot *curr, const DioramaScrollSnapshot *prev, float alpha);

/* The pair-validity half of the above, exposed so a caller deciding whether a
 * re-present is worth doing uses the SAME predicate the math does. Keeping
 * these in one place is what stops the gate from drifting away from the
 * bail-outs — a drift that would burn a full composite per frame to draw a
 * byte-identical image while looking like the feature was working. */
bool DioramaScrollPairIsInterpolable(const FrameSlot *curr,
                                     const DioramaScrollSnapshot *prev);

/* R17/C1: shift a layer's UV window by `du`, keeping the window inside the
 * captured texture region.
 *
 * The predecessor logic (diorama.c, "clamp the WINDOW POSITION ... preserving
 * width") clamped the shifted window back into [region_u0, region_u1]. That
 * cancelled the shift ENTIRELY rather than only at the buffer's true edge,
 * because the window it clamped is exactly as wide as the region it clamped
 * into: diorama.c sets the window to the whole captured span, so there is zero
 * slack and ANY nonzero du pushes one edge out, whereupon the "excess"
 * subtracted back is the whole of du. Horizontal sub-tick interpolation was
 * therefore a no-op on every layer on every frame.
 *
 * The fix is to clamp the SHIFT to a slack margin the caller reserves inside
 * the region (see kInterpUvSlackPx), so a shift up to that margin passes
 * through untouched and only a larger one saturates. Width is always
 * preserved exactly, so there is never a visual squish.
 *
 * region_u0/region_u1: the usable sub-region, ALREADY inset by the slack.
 * slack: the margin, in the same units as the region (i.e. normalized UV).
 * Pure function of its arguments — no clock, no globals. */
void DioramaInterpUvWindow(float region_u0, float region_u1, float du,
                           float slack, float *out_u0, float *out_u1);

/* THICKNESS: one vertex of the extruded near face ("skirt") below a layer.
 *
 * A thickness extrudes the plane's BOTTOM edge forward from `z` to
 * `z + thickness`, so the layer reads as a block with a near face instead of an
 * infinitely thin sheet. This is the per-vertex arithmetic, extracted pure so the
 * geometry is testable without a renderer -- the mesh assembly and projection stay
 * in diorama.c (precedent: this file's own DioramaInterpUvWindow, and
 * diorama_skybox_uv.c).
 *
 * `t` is 0 at the fold (the plane's bottom edge) and 1 at the near lip.
 * `z_bottom` is the plane's bottom-edge depth -- z + rake, NOT z, so a room that
 * authors both a rake and a thickness does not tear at the fold.
 * `y_bottom` is the plane's bottom-edge world Y.
 *
 * The face drops in Y as it comes forward in Z, at half the thickness, so a thick
 * layer reads as a block rather than a tall wall. `out_shade` is a multiplier that
 * darkens with depth: a real near face turned away from the light is not the same
 * brightness as the top surface, and without the gradient the skirt reads as a
 * smear of the plane's bottom row rather than a separate surface.
 *
 * Clamps t to [0,1] and treats a non-positive thickness as zero, so a caller
 * cannot produce geometry above the fold or behind the plane. */
void DioramaSkirtVertex(float t, float z_bottom, float y_bottom,
                        float thickness, float *out_y, float *out_z,
                        float *out_shade);

/* Shade multiplier at the near lip of a skirt (t == 1). Exposed so the value is
 * named once rather than duplicated between the geometry and its test. */
float DioramaSkirtNearShade(void);

/* STACK: depth, shade and opacity of one copy in a stacked layer.
 *
 * A stack fills a depth gap by repeating the layer at intermediate depths instead
 * of tilting it. Tilting (rake) puts a single plane's own rows at different
 * depths, which gives that layer two different parallax rates within itself and
 * shears it as the camera moves. Every copy here stays exactly parallel, so each
 * has ONE depth and one parallax rate, and the layer keeps the flat poster-like
 * motion the diorama is built on.
 *
 * `index` is 0 for the copy nearest the plane's own depth and `copies - 1` for the
 * farthest into the gap; `copies` is clamped to >= 1. `depth` is the total fill,
 * laid from `z_base` toward `z_base + depth`.
 *
 * Copy 0 sits AT z_base -- deliberately, so it coincides with the plane the caller
 * already draws and the stack has no seam at its own front face. The caller
 * therefore skips index 0 and draws 1..copies-1 as extra passes.
 *
 * `out_alpha` and `out_shade` both fall off with distance into the gap so the
 * stack reads as receding volume rather than a smear of identical sprites. Alpha
 * never reaches 0 for a valid index, since an invisible copy is a wasted draw. */
void DioramaStackCopy(int index, int copies, float z_base, float depth,
                      float *out_z, float *out_shade, float *out_alpha);

/* Same, with a direction (a DioramaStackDirection, passed as int so this header
 * keeps no dependency on the override module).
 *
 * Forward lays copies from z_base toward z_base + depth, i.e. TOWARD the camera,
 * since higher z is nearer in this projection. That is the default and what the
 * reported Fillmore act 2 case needs -- its water sits behind the rock path, so
 * the gap to fill is on the camera side. Backward is the mirror, for a foreground
 * layer receding into the scene. Both centres the fill on the plane, for something
 * the plane sits in the middle of.
 *
 * Shade and alpha fall off with DISTANCE from the plane, not with signed depth, so
 * a Backward or Both stack recedes visually the same way a Forward one does rather
 * than brightening as it goes. */
void DioramaStackCopyDirected(int index, int copies, float z_base, float depth,
                              int direction, float *out_z, float *out_shade,
                              float *out_alpha);

/* True when a copy at `index` of `copies` is worth drawing at all: in range and
 * not fully transparent. The caller's loop guard, so the "is this a wasted draw"
 * rule lives with the arithmetic instead of being restated at the call site. */
bool DioramaStackCopyIsVisible(int index, int copies);

/* True when this copy coincides with the plane's OWN depth, so the caller must
 * skip it -- the plane's existing draw already covers it, and drawing both would
 * double-darken the front face at that depth.
 *
 * Not simply "index == 0": that only holds for a one-sided fill. A centred (Both)
 * stack puts index 0 at the FAR edge, and its redundant copy is the middle one --
 * which exists only for an ODD count. Keeping this rule here rather than at the
 * call site is what stops the two from disagreeing. */
bool DioramaStackCopyIsRedundant(int index, int copies, int direction);
#endif
