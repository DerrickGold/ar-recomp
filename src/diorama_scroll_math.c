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
  if (curr->timestamp_ns - prev->timestamp_ns >= 50000000ULL) return false;
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
   * textures, which are allocated kPpuBufWidth (448) wide — the widescreen
   * headroom — with the capture occupying only the leading snes_width columns.
   * diorama.c normalizes its U window the same way (uv_u1 = snes_width /
   * kPpuBufWidth), and its shader uniforms pass the U texel size as
   * 1/kPpuBufWidth. So one source column is 1/448 of U, NOT 1/snes_width.
   *
   * Dividing by snes_width made every horizontal shift kPpuBufWidth/snes_width
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
   * V was already correct: snes_height IS the texture height, and diorama.c
   * normalizes V by the same tex_h. Only U was inconsistent, which is also why
   * diagonal motion sheared horizontally against vertically.
   *
   * (Widescreen ON makes snes_width 446, i.e. a factor of 1.0045 — nearly
   * correct. So the pre-fix artifact was much worse with widescreen off, which
   * is a useful signature if it is ever seen again.) */
  int dh1 = curr->bg1_camera_x - prev->bg1_camera_x;
  int dv1 = curr->bg1_camera_y - prev->bg1_camera_y;
  d.bg_du[0] = (t * (float)dh1) / (float)kFrameSlotLayerTextureWidth;
  d.bg_dv[0] = (t * (float)dv1) / (float)curr->snes_height;

  int dh2 = curr->bg2_camera_x - prev->bg2_camera_x;
  int dv2 = curr->bg2_camera_y - prev->bg2_camera_y;
  d.bg_du[1] = (t * (float)dh2) / (float)kFrameSlotLayerTextureWidth;
  d.bg_dv[1] = (t * (float)dv2) / (float)curr->snes_height;

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

/* ── thickness: the extruded near face ────────────────────────────────── */

/* How dark the near lip gets. 1.0 would make the fold invisible; too low reads
 * as a black band rather than the same material turned away from the light. */
static const float kSkirtNearShade = 0.55f;

float DioramaSkirtNearShade(void) { return kSkirtNearShade; }

void DioramaSkirtVertex(float t, float z_bottom, float y_bottom,
                        float thickness, float *out_y, float *out_z,
                        float *out_shade) {
  if (!(thickness > 0.0f)) thickness = 0.0f;   /* also catches NaN */
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  /* Half the thickness in Y: the face drops as it comes forward, so a thick
   * layer reads as a block instead of a tall wall. */
  if (out_y) *out_y = y_bottom - t * thickness * 0.5f;
  if (out_z) *out_z = z_bottom + t * thickness;
  if (out_shade) *out_shade = 1.0f - (1.0f - kSkirtNearShade) * t;
}

/* ── stack: fill a depth gap with parallel copies ─────────────────────── */

/* Falloff at the FARTHEST copy. Both are multipliers on the layer's own values,
 * so a stacked translucent layer stays translucent. Not 0: a fully transparent
 * copy is a draw call that paints nothing. */
static const float kStackFarShade = 0.50f;
static const float kStackFarAlpha = 0.35f;

bool DioramaStackCopyIsVisible(int index, int copies) {
  if (copies < 1) copies = 1;
  return index >= 0 && index < copies;
}

void DioramaStackCopy(int index, int copies, float z_base, float depth,
                      float *out_z, float *out_shade, float *out_alpha) {
  if (copies < 1) copies = 1;
  if (!(depth > 0.0f)) depth = 0.0f;      /* also catches NaN */
  if (index < 0) index = 0;
  if (index > copies - 1) index = copies - 1;
  /* Fraction of the way into the gap. A single copy is entirely at z_base, which
   * keeps `copies:1` a no-op rather than a division by zero. */
  float f = (copies > 1) ? (float)index / (float)(copies - 1) : 0.0f;
  if (out_z) *out_z = z_base + f * depth;
  if (out_shade) *out_shade = 1.0f - (1.0f - kStackFarShade) * f;
  if (out_alpha) *out_alpha = 1.0f - (1.0f - kStackFarAlpha) * f;
}
