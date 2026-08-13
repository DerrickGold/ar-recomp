/* diorama_scroll_math.c — extracted from present.c's ComputeDioramaScrollDelta
 * so the pure M7 interpolation math can be golden-tested (Test gap 2).
 *
 * Originally the wall clock was INJECTED here for testability, which was an
 * improvement over reading it inline but still left production reconstructing
 * the sub-tick phase from a clock plus an EMA of the tick period. R17/C4 passes
 * the phase itself instead: the main loop owns the tick schedule and already
 * knows the phase exactly, so there is nothing left to estimate and no clock in
 * this file at all. */
#include "diorama_scroll_math.h"

#include <stdio.h>   /* fprintf (AR_INTERP_LOG) */
#include <stdlib.h>  /* getenv (AR_INTERP_LOG) */

#include "constants.h"

enum { kMaximumInterpolablePairSpanMs = 50 };

void DioramaInterpUvWindow(float region_u0, float region_u1, float du,
                           float slack, float *out_u0, float *out_u1) {
  /* Clamp the SHIFT, not the window position — see the header comment for why
   * clamping the position cancelled the shift outright. Saturating at the
   * slack margin means motion stays smooth up to the margin and then stops
   * growing, instead of snapping back to zero. */
  if (slack < 0.0f) slack = 0.0f;
  if (du > slack) du = slack;
  else if (du < -slack) du = -slack;
  *out_u0 = region_u0 + du;
  *out_u1 = region_u1 + du;
}

bool DioramaScrollPairIsInterpolable(const FrameSlot *curr,
                                    const DioramaScrollSnapshot *prev) {
  if (!prev || !curr) return false;
  if (!prev->diorama_active) return false;
  if (curr->turbo_active || prev->turbo_active) return false;  /* §6.4 */
  if (curr->timestamp_ns == 0 || prev->timestamp_ns == 0) return false;
  if (curr->bg_mode != prev->bg_mode) return false;            /* §6.4 mode change */
  if (curr->timestamp_ns <= prev->timestamp_ns) return false;  /* not a real pair */
  /* §6.2 sanity: a pair spanning >=50ms is not a plausible one-tick velocity
   * estimate (savestate load, long stall). Still checked on the timestamps,
   * which remain the honest record of when the two frames were captured — the
   * span is simply no longer used as the interpolation DIVISOR. */
  if (curr->timestamp_ns - prev->timestamp_ns >=
      (uint64_t)kMaximumInterpolablePairSpanMs *
          kNanosecondsPerMillisecond)
    return false;
  /* R17/C3: 0 means "paused re-capture", which is not a pair; and it is the
   * divisor below, so this also guards the division. */
  if (curr->capture_ticks == 0) return false;
  return true;
}

DioramaScrollDelta ComputeDioramaScrollDeltaAt(
    const FrameSlot *curr, const DioramaScrollSnapshot *prev, float alpha) {
  DioramaScrollDelta d = {0};
  /* Single source of truth, shared with the caller's gate — so "should we
   * bother re-presenting?" can never drift from "will this actually
   * interpolate?", which would make an inert feature indistinguishable from a
   * working one. */
  if (!DioramaScrollPairIsInterpolable(curr, prev)) return d;
  if (alpha == kInterpPhaseNone) return d;

  /* R17/C4: the sub-tick phase is the main loop's own accumulator remainder,
   * passed in. It replaces `(SDL_GetTicksNS() - curr->timestamp_ns) /
   * span_ema` — a value reconstructed from a DIFFERENT clock than the one that
   * scheduled the tick, divided by an EMA estimate of a period that is known
   * exactly (kFrameNs). Reconstructing it was the root of three separate
   * regressions: the estimate could be polluted by non-tick presents (R16),
   * needed R3's smoothing to survive a hitch, and saturated at 1.0 whenever
   * the wall clock outran the estimate. accumulator/kFrameNs needs none of
   * that: it is exact by construction and cannot be corrupted by presents,
   * because presents do not write it.
   *
   * Divided by capture_ticks (C3) because alpha is a fraction of ONE tick
   * while the pair may span several — see the field comment in present.h. */
  float t = alpha / (float)curr->capture_ticks;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  d.active = true;
  /* B1b (followup doc) source fix: the WRAM camera is a real world-pixel
   * position, not a 10-bit modular PPU register, so a plain signed
   * difference is exact — no wrap correction needed (the old ±512/1024
   * hScroll/vScroll fixup this replaced would itself corrupt a large
   * legitimate camera delta if kept). BG3/BG4 (index 2/3) have no WRAM
   * camera and stay at their zero-initialized d.bg_du/bg_dv — BG3 is the
   * HUD (composes with diorama_hud_flat's default, where BG3 isn't even a
   * tilted plane), and BG4 is unused in this game's Mode 1. */
  /* IJ1: the U denominator is the TEXTURE width, not the visible width.
   *
   * These deltas are consumed as normalized UV offsets into the diorama layer
   * textures, which are allocated kPpuSurfaceWidth (640) wide — the widescreen
   * headroom — with the capture occupying only the leading snes_width columns.
   * diorama.c normalizes its U window the same way (uv_u1 = snes_width /
   * kPpuSurfaceWidth), and its shader uniforms pass the U texel size as
   * 1/kPpuSurfaceWidth. So one source column is 1/640 of U, NOT 1/snes_width.
   *
   * Dividing by snes_width made every horizontal shift kPpuSurfaceWidth/snes_width
   * too large — 1.75x with widescreen off. That is why interpolation jittered
   * during STEADY walking, not just on velocity changes: at t->1 the displayed
   * camera sat at P + 1.75*delta, while the next tick's t=0 lands at
   * P + delta, so 0.75*delta was discarded BACKWARD at every tick boundary —
   * a 60Hz sawtooth even at perfectly constant velocity, which is precisely
   * the case forward extrapolation is supposed to render smoothly.
   *
   * It also made the R17/C1 slack margin saturate at 2.29 camera px/tick
   * instead of the intended 4, so ordinary walking clipped the offset to a
   * constant for most of a tick and then jumped — a second, harsher artifact
   * on top of the first.
   *
   * The same allocation rule applies to V. Layer textures gained fixed top and
   * bottom margin capacity after this comment was written, so snes_height=224
   * is now only the authentic viewport while the texture is kPpuBufHeight=352.
   * Dividing by 224 applies 1.57 texture rows for each camera pixel and throws
   * the excess away at the next captured tick: a vertical-only sawtooth that is
   * most visible at an attached repeat boundary.
   *
   * (Wide diorama makes snes_width 496; the denominator still has the apron,
   * so using the captured width remains wrong even at maximum margin, though
   * less dramatically than in 4:3. The pre-fix artifact was therefore much
   * worse with widescreen off, which is a useful signature if it is ever seen
   * again.) */
  int dh1 = curr->bg1_camera_x - prev->bg1_camera_x;
  int dv1 = curr->bg1_camera_y - prev->bg1_camera_y;
  d.bg_du[0] = (t * (float)dh1) / (float)kFrameSlotLayerTextureWidth;
  d.bg_dv[0] =
      (t * (float)dv1) / (float)kFrameSlotLayerTextureHeight;

  int dh2 = curr->bg2_camera_x - prev->bg2_camera_x;
  int dv2 = curr->bg2_camera_y - prev->bg2_camera_y;
  d.bg_du[1] = (t * (float)dh2) / (float)kFrameSlotLayerTextureWidth;
  d.bg_dv[1] =
      (t * (float)dv2) / (float)kFrameSlotLayerTextureHeight;

  /* AR_INTERP_LOG=1: log BG1's interpolated offset every present, so the M7
   * acceptance test (ar-recomp-threading-impl.md milestone M7) can assert
   * "monotonic sub-steps ~half the per-tick delta" mechanically instead of
   * by eye. Fires on re-presents too (that is where the sub-steps now come
   * from), so a run where every line reads alpha=0.00 is the tell that the
   * cadence is broken again.
   *
   * NOTE: a ramping t here does NOT prove the feature works — the UV window
   * consumer can still cancel the shift downstream, which is exactly what
   * R17/C1 fixed. Judge acceptance on the mesh UVs or on screen. */
  static int log_on = -1;
  if (log_on < 0) {
    const char *e = getenv("AR_INTERP_LOG");
    log_on = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  if (log_on) {
    fprintf(stderr,
            "[interp] t=%.3f alpha=%.3f ticks=%u bg1_du=%.5f bg1_dv=%.5f\n",
            t, alpha, (unsigned)curr->capture_ticks, d.bg_du[0], d.bg_dv[0]);
  }
  return d;
}
