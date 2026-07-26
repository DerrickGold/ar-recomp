#include <stdio.h>
#include <string.h>
#include <math.h>
#include "diorama_scroll_math.h"

static int failures;
#define CHECK(e) do { if(!(e)){ fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e); failures++; } } while(0)
static int near(float a, float b){ return fabsf(a-b) < 1e-4f; }

int main(void) {
  FrameSlot curr; DioramaScrollSnapshot prev;
  memset(&curr, 0, sizeof(curr)); memset(&prev, 0, sizeof(prev));
  curr.snes_width = 256; curr.snes_height = 224;
  curr.diorama_active = prev.diorama_active = true;
  curr.capture_ticks = 1;                             /* a one-tick pair */
  prev.timestamp_ns = 1000000000ULL;
  curr.timestamp_ns = 1000000000ULL + 16600000ULL;   /* +16.6ms */
  prev.bg1_camera_x = 0;  curr.bg1_camera_x = 10;     /* +10 px */

  /* alpha 0.5 = halfway to the next tick. */
  DioramaScrollDelta d = ComputeDioramaScrollDeltaAt(&curr, &prev, 0.5f);
  CHECK(d.active);
  CHECK(near(d.bg_du[0], (0.5f * 10.0f) / 448.0f));   /* IJ1: /448, not /256 */
  CHECK(d.bg_du[0] > 0.0f);
  /* Refuted-claim guard: do NOT assert bg_du[1]==0 (BG2 has a WRAM camera).
   * BG3 (index 2) and BG4 (index 3) DO stay zero. */
  CHECK(d.bg_du[2] == 0.0f && d.bg_dv[2] == 0.0f);
  CHECK(d.bg_du[3] == 0.0f && d.bg_dv[3] == 0.0f);
  /* bg_du[1]==0 here only because bg2_camera_x is equal in both snapshots: */
  CHECK(d.bg_du[1] == 0.0f);

  /* Turbo suppresses interpolation. */
  curr.turbo_active = true;
  CHECK(!ComputeDioramaScrollDeltaAt(&curr, &prev, 0.5f).active);
  curr.turbo_active = false;

  /* span >= 50ms suppresses (the timestamps are still the honest record of
   * when the two frames were captured; they are just no longer the divisor). */
  FrameSlot slow = curr; slow.timestamp_ns = prev.timestamp_ns + 60000000ULL;
  CHECK(!ComputeDioramaScrollDeltaAt(&slow, &prev, 0.5f).active);

  /* R17/C4: the sub-tick phase is now PASSED IN, so the whole class of bugs
   * that came from reconstructing it (R16's span_ema pollution, R3's smoothing,
   * saturation at t=1.0) is gone with the quantity itself. The former
   * span_ema-leak regression case that lived here is deliberately REPLACED
   * rather than deleted: the average it guarded no longer exists, so it could
   * only ever pass. What matters now is the BINDING — that a present at a real
   * sub-tick phase produces a real shift — which is what every historical
   * failure actually got wrong while the arithmetic stayed correct.
   *
   * Monotone ramp: the same pair at increasing phase must give strictly
   * increasing offsets, reaching exactly one tick of motion at alpha -> 1. */
  {
    float prev_du = -1.0f;
    const float phases[] = { 0.0f, 0.25f, 0.5f, 0.75f };
    for (unsigned i = 0; i < sizeof phases / sizeof *phases; i++) {
      DioramaScrollDelta r = ComputeDioramaScrollDeltaAt(&curr, &prev, phases[i]);
      CHECK(r.active);                       /* alpha 0.0 is a LEGAL phase */
      CHECK(near(r.bg_du[0], (phases[i] * 10.0f) / 448.0f));
      CHECK(r.bg_du[0] > prev_du);           /* strictly increasing */
      prev_du = r.bg_du[0];
    }
    /* alpha 0 means zero shift, but ACTIVE — distinct from "no phase". */
    CHECK(near(ComputeDioramaScrollDeltaAt(&curr, &prev, 0.0f).bg_du[0], 0.0f));
    /* kInterpPhaseNone means "this present has no phase" -> inactive. */
    CHECK(!ComputeDioramaScrollDeltaAt(&curr, &prev, kInterpPhaseNone).active);
  }

  /* R17/C3: a multi-tick pair carries proportionally more motion, so the phase
   * must be divided by capture_ticks. Without this the extrapolation overshoots
   * by exactly that factor — the correction the deleted wall-clock span EMA had
   * been making implicitly. 2 ticks, 20px total, alpha 0.5 -> half of ONE
   * tick's 10px. */
  {
    FrameSlot multi = curr;
    multi.capture_ticks = 2;
    multi.bg1_camera_x = 20;                 /* 20px over two ticks */
    DioramaScrollDelta r = ComputeDioramaScrollDeltaAt(&multi, &prev, 0.5f);
    CHECK(r.active);
    CHECK(near(r.bg_du[0], (0.5f * 10.0f) / 448.0f));
    /* Same pair mislabelled as one tick would double it — the bug this guards. */
    FrameSlot mislabelled = multi; mislabelled.capture_ticks = 1;
    CHECK(near(ComputeDioramaScrollDeltaAt(&mislabelled, &prev, 0.5f).bg_du[0],
               (0.5f * 20.0f) / 448.0f));
  }

  /* capture_ticks == 0 is a paused re-capture, not a pair — and it is the
   * divisor, so this also guards against a division by zero. */
  {
    FrameSlot paused = curr; paused.capture_ticks = 0;
    CHECK(!ComputeDioramaScrollDeltaAt(&paused, &prev, 0.5f).active);
  }

  /* The gate and the math must agree exactly, or "should we re-present?" drifts
   * from "will this interpolate?" and an inert feature looks like a working one
   * (H3). Same matrix through both entry points. */
  {
    FrameSlot m = curr;
    /* equal timestamps: not a pair */
    m.timestamp_ns = prev.timestamp_ns;
    CHECK(DioramaScrollPairIsInterpolable(&m, &prev) ==
          ComputeDioramaScrollDeltaAt(&m, &prev, 0.5f).active);
    CHECK(!DioramaScrollPairIsInterpolable(&m, &prev));
    /* NULL prev */
    CHECK(!DioramaScrollPairIsInterpolable(&curr, NULL));
    CHECK(DioramaScrollPairIsInterpolable(&curr, NULL) ==
          ComputeDioramaScrollDeltaAt(&curr, NULL, 0.5f).active);
    /* prev not in diorama mode */
    DioramaScrollSnapshot flat = prev; flat.diorama_active = false;
    CHECK(!DioramaScrollPairIsInterpolable(&curr, &flat));
    CHECK(DioramaScrollPairIsInterpolable(&curr, &flat) ==
          ComputeDioramaScrollDeltaAt(&curr, &flat, 0.5f).active);
    /* the valid pair agrees too */
    CHECK(DioramaScrollPairIsInterpolable(&curr, &prev));
    CHECK(DioramaScrollPairIsInterpolable(&curr, &prev) ==
          ComputeDioramaScrollDeltaAt(&curr, &prev, 0.5f).active);
    /* bg_mode change */
    FrameSlot mode = curr; mode.bg_mode = (uint8_t)(prev.bg_mode + 1);
    CHECK(!DioramaScrollPairIsInterpolable(&mode, &prev));
    CHECK(DioramaScrollPairIsInterpolable(&mode, &prev) ==
          ComputeDioramaScrollDeltaAt(&mode, &prev, 0.5f).active);
  }

  /* R17/C1: the UV window must actually SHIFT.
   *
   * The predecessor logic clamped the shifted window back inside the captured
   * region, and because the window was exactly as wide as that region, the
   * "excess" it subtracted was the whole shift — horizontal interpolation was
   * a total no-op on every layer on every frame. These cases fail against
   * that logic (they assert the window MOVED) and pass against the
   * clamp-the-shift rule. Acceptance is stated on the returned UV window, not
   * on the interpolation alpha: a ramping alpha with a cancelled window is
   * exactly the bug this replaced. */
  {
    const float slack = 4.0f / 448.0f;               /* kInterpUvSlackPx / kPpuBufWidth */
    const float r0 = slack, r1 = 446.0f / 448.0f - slack;
    const float width = r1 - r0;
    float u0, u1;

    /* Half the slack: passes through untouched, width preserved. */
    float du = 0.5f * slack;
    DioramaInterpUvWindow(r0, r1, du, slack, &u0, &u1);
    CHECK(near(u0, r0 + du));
    CHECK(near(u1, r1 + du));
    CHECK(near(u1 - u0, width));
    CHECK(u0 != r0);                                  /* it MOVED — the bug's tell */

    /* Negative shift is symmetric. */
    DioramaInterpUvWindow(r0, r1, -du, slack, &u0, &u1);
    CHECK(near(u0, r0 - du));
    CHECK(near(u1 - u0, width));
    CHECK(u0 != r0);

    /* Beyond the margin: saturates AT the margin, still never snaps to zero. */
    DioramaInterpUvWindow(r0, r1, 3.0f * slack, slack, &u0, &u1);
    CHECK(near(u0, r0 + slack));
    CHECK(near(u1 - u0, width));
    DioramaInterpUvWindow(r0, r1, -3.0f * slack, slack, &u0, &u1);
    CHECK(near(u0, r0 - slack));
    CHECK(near(u1 - u0, width));

    /* Saturated window stays inside the real texture span [0, 446/448]. */
    DioramaInterpUvWindow(r0, r1, 99.0f, slack, &u0, &u1);
    CHECK(u0 >= 0.0f && u1 <= 446.0f / 448.0f + 1e-6f);
    DioramaInterpUvWindow(r0, r1, -99.0f, slack, &u0, &u1);
    CHECK(u0 >= -1e-6f && u1 <= 446.0f / 448.0f);

    /* Zero shift is exactly identity. */
    DioramaInterpUvWindow(r0, r1, 0.0f, slack, &u0, &u1);
    CHECK(near(u0, r0) && near(u1, r1));
  }

  /* IJ1: the U and V axes must use the SAME unit — one source texel — so a
   * diagonal camera move produces a shift with the same aspect as the motion.
   *
   * The bug: U divided by snes_width (256) while diorama.c normalizes its U
   * window by the layer texture's allocated width (kPpuBufWidth = 448). V
   * divided by snes_height, which IS the texture height, so V was right and U
   * was 1.75x too large. Two consequences, both of which the author saw as
   * jitter:
   *   - At t->1 the displayed camera sat at P + 1.75*delta while the next
   *     tick's t=0 lands at P + delta, so 0.75*delta was thrown away BACKWARD
   *     every tick — a 60Hz sawtooth even at constant velocity.
   *   - Diagonal motion sheared, because H and V disagreed by 1.75x.
   *
   * The assertion is the axis RATIO, which is unit-independent and therefore
   * survives any future change to either denominator: an equal-pixel diagonal
   * move must yield du/dv equal to the texture's own height/width ratio. */
  {
    FrameSlot diag = curr;
    diag.capture_ticks = 1;
    diag.snes_width = 256; diag.snes_height = 224;
    DioramaScrollSnapshot base = prev;
    base.bg1_camera_x = 100; base.bg1_camera_y = 100;
    diag.bg1_camera_x = 108; diag.bg1_camera_y = 108;   /* +8 px on BOTH axes */

    DioramaScrollDelta r = ComputeDioramaScrollDeltaAt(&diag, &base, 1.0f);
    CHECK(r.active);
    /* One texel of U is 1/448; one row of V is 1/224. So an equal-pixel move
     * must give dv exactly 2x du (448/224), NOT the 1.75x-inflated du the bug
     * produced (which yielded dv/du = 224/... -> 1.143). */
    CHECK(near(r.bg_du[0], 8.0f / 448.0f));
    CHECK(near(r.bg_dv[0], 8.0f / 224.0f));
    CHECK(near(r.bg_dv[0] / r.bg_du[0], 448.0f / 224.0f));

    /* And the load-bearing property: at t=1 the shift must equal EXACTLY one
     * tick of motion in texture units, so extrapolation lands precisely on the
     * next tick's captured position instead of overshooting past it. Overshoot
     * is what made the offset get discarded backward every tick. */
    CHECK(near(r.bg_du[0] * 448.0f, 8.0f));    /* 8 texels, not 14 */
    CHECK(near(r.bg_dv[0] * 224.0f, 8.0f));

    /* The R17/C1 slack margin must not saturate at ordinary walking speed. The
     * margin is 4px of the 448-wide texture; a one-tick move of 4 source px at
     * full phase must therefore fit exactly, and 3px must fit with room. With
     * the 1.75x inflation, saturation began at 2.29px — inside walking range. */
    const float uv_slack = 4.0f / 448.0f;
    for (int px = 1; px <= 4; px++) {
      FrameSlot walk = diag;
      walk.bg1_camera_x = 100 + px;
      walk.bg1_camera_y = 100;
      DioramaScrollDelta w = ComputeDioramaScrollDeltaAt(&walk, &base, 1.0f);
      CHECK(w.active);
      CHECK(w.bg_du[0] <= uv_slack + 1e-6f);   /* does not need clamping */
    }
  }

  if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  puts("diorama_scroll_math_test: PASS");
  return 0;
}
