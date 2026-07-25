#ifndef DIORAMA_SCROLL_MATH_H
#define DIORAMA_SCROLL_MATH_H
#include "present.h"   /* FrameSlot, DioramaScrollSnapshot, DioramaScrollDelta */
/* Pure interpolation math with the wall-clock injected, for testability. */
DioramaScrollDelta ComputeDioramaScrollDeltaAt(
    const FrameSlot *curr, const DioramaScrollSnapshot *prev, uint64_t now_ns);

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
#endif
