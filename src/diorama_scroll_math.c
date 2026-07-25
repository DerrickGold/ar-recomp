/* diorama_scroll_math.c — extracted from present.c's ComputeDioramaScrollDelta
 * so the pure M7 interpolation math can be golden-tested with the wall-clock
 * injected (Test gap 2). present.c keeps a thin wrapper that supplies
 * SDL_GetTicksNS() at the single production call site. */
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

DioramaScrollDelta ComputeDioramaScrollDeltaAt(
    const FrameSlot *curr, const DioramaScrollSnapshot *prev, uint64_t now_ns) {
  DioramaScrollDelta d = {0};
  if (!prev || !curr) return d;
  if (!prev->diorama_active) return d;
  if (curr->turbo_active || prev->turbo_active) return d;      /* §6.4 */
  if (curr->timestamp_ns == 0 || prev->timestamp_ns == 0) return d;
  if (curr->bg_mode != prev->bg_mode) return d;                /* §6.4 mode change */
  if (curr->timestamp_ns <= prev->timestamp_ns) return d;      /* not a real pair yet */
  uint64_t span = curr->timestamp_ns - prev->timestamp_ns;
  if (span >= 50000000ULL) return d;                            /* §6.2 sanity: <50ms */
  /* R3: smooth the tick span so a single wall-clock hitch doesn't corrupt
   * the one-cycle velocity estimate. Single-threaded after Phase 0, so this
   * function-local static needs no lock; the <50ms guard above keeps a
   * hitch out of the average. */
  static float span_ema = 0.0f;
  if (span_ema <= 0.0f) span_ema = (float)span;                 /* seed on first valid pair */
  else span_ema += ((float)span - span_ema) * 0.25f;
  uint64_t now = now_ns;
  if (now < curr->timestamp_ns) return d;
  float t = (float)(now - curr->timestamp_ns) / span_ema;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;                                       /* §6.4 turbo/frame-skip clamp */

  d.active = true;
  /* B1b (followup doc) source fix: the WRAM camera is a real world-pixel
   * position, not a 10-bit modular PPU register, so a plain signed
   * difference is exact — no wrap correction needed (the old ±512/1024
   * hScroll/vScroll fixup this replaced would itself corrupt a large
   * legitimate camera delta if kept). BG3/BG4 (index 2/3) have no WRAM
   * camera and stay at their zero-initialized d.bg_du/bg_dv — BG3 is the
   * HUD (composes with diorama_hud_flat's default, where BG3 isn't even a
   * tilted plane), and BG4 is unused in this game's Mode 1. */
  int dh1 = curr->bg1_camera_x - prev->bg1_camera_x;
  int dv1 = curr->bg1_camera_y - prev->bg1_camera_y;
  d.bg_du[0] = (t * (float)dh1) / (float)curr->snes_width;
  d.bg_dv[0] = (t * (float)dv1) / (float)curr->snes_height;

  int dh2 = curr->bg2_camera_x - prev->bg2_camera_x;
  int dv2 = curr->bg2_camera_y - prev->bg2_camera_y;
  d.bg_du[1] = (t * (float)dh2) / (float)curr->snes_width;
  d.bg_dv[1] = (t * (float)dv2) / (float)curr->snes_height;

  /* AR_INTERP_LOG=1: log BG1's interpolated offset every present, so the M7
   * acceptance test (ar-recomp-threading-impl.md milestone M7) can assert
   * "monotonic sub-steps ~half the per-tick delta" mechanically instead of
   * by eye. */
  static int log_on = -1;
  if (log_on < 0) {
    const char *e = getenv("AR_INTERP_LOG");
    log_on = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  if (log_on) {
    fprintf(stderr, "[interp] t=%.3f bg1_du=%.5f bg1_dv=%.5f span_ns=%llu\n",
            t, d.bg_du[0], d.bg_dv[0], (unsigned long long)span);
  }
  return d;
}
