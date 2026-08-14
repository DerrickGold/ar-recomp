#include <stdio.h>
#include <string.h>
#include <math.h>
#include "diorama_depth_shapes.h"
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

  /* A one-tick-delayed presenter is halfway from prev to curr. The current
   * texture therefore samples five pixels back toward prev. */
  DioramaScrollDelta d = ComputeDioramaScrollDeltaAt(&curr, &prev, 0.5f);
  CHECK(d.active);
  CHECK(near(d.bg_du[0], (-0.5f * 10.0f) / (float)kFrameSlotLayerTextureWidth));  /* IJ1: /tex, not /256 */
  CHECK(d.bg_du[0] < 0.0f);
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
   * increasing offsets, reaching the current endpoint at alpha -> 1. */
  {
    float prev_du = -1000.0f;
    const float phases[] = { 0.0f, 0.25f, 0.5f, 0.75f };
    for (unsigned i = 0; i < sizeof phases / sizeof *phases; i++) {
      DioramaScrollDelta r = ComputeDioramaScrollDeltaAt(&curr, &prev, phases[i]);
      CHECK(r.active);                       /* alpha 0.0 is a LEGAL phase */
      CHECK(near(r.bg_du[0], ((phases[i] - 1.0f) * 10.0f) /
                                  (float)kFrameSlotLayerTextureWidth));
      CHECK(r.bg_du[0] > prev_du);           /* strictly increasing */
      prev_du = r.bg_du[0];
    }
    /* alpha 0 shows prev; alpha 1 reaches curr. Both remain active and are
     * distinct from "no phase". */
    CHECK(near(ComputeDioramaScrollDeltaAt(&curr, &prev, 0.0f).bg_du[0],
               -10.0f / (float)kFrameSlotLayerTextureWidth));
    CHECK(near(ComputeDioramaScrollDeltaAt(&curr, &prev, 1.0f).bg_du[0],
               0.0f));
    /* kInterpPhaseNone means "this present has no phase" -> inactive. */
    CHECK(!ComputeDioramaScrollDeltaAt(&curr, &prev, kInterpPhaseNone).active);
  }

  /* R17/C3: a multi-tick pair carries proportionally more motion, so the phase
   * needs a phase of `(ticks-1+alpha)/ticks`: the presentation target is one
   * tick behind curr, not all the way back at prev. 2 ticks, 20px total,
   * alpha 0.5 -> five pixels behind curr. */
  {
    FrameSlot multi = curr;
    multi.capture_ticks = 2;
    multi.bg1_camera_x = 20;                 /* 20px over two ticks */
    DioramaScrollDelta r = ComputeDioramaScrollDeltaAt(&multi, &prev, 0.5f);
    CHECK(r.active);
    CHECK(near(r.bg_du[0], (-0.5f * 10.0f) / (float)kFrameSlotLayerTextureWidth));
    /* Same pair mislabelled as one tick would double the residual. */
    FrameSlot mislabelled = multi; mislabelled.capture_ticks = 1;
    CHECK(near(ComputeDioramaScrollDeltaAt(&mislabelled, &prev, 0.5f).bg_du[0],
               (-0.5f * 20.0f) / (float)kFrameSlotLayerTextureWidth));
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
    /* Against the real surface width, not a literal: the apron moved that
     * constant from 448 to 576, and the BG-width track moved it again to 640.
     * `span` is the written content span the window must stay inside. */
    const float tex = (float)kFrameSlotLayerTextureWidth;
    const float span = 496.0f;
    const float slack = 4.0f / tex;                  /* kInterpUvSlackPx / tex */
    const float r0 = slack, r1 = span / tex - slack;
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

    /* Saturated window stays inside the real texture span [0, span/tex]. */
    DioramaInterpUvWindow(r0, r1, 99.0f, slack, &u0, &u1);
    CHECK(u0 >= 0.0f && u1 <= span / tex + 1e-6f);
    DioramaInterpUvWindow(r0, r1, -99.0f, slack, &u0, &u1);
    CHECK(u0 >= -1e-6f && u1 <= span / tex);

    /* Zero shift is exactly identity. */
    DioramaInterpUvWindow(r0, r1, 0.0f, slack, &u0, &u1);
    CHECK(near(u0, r0) && near(u1, r1));
  }

  /* IJ1 follow-up: both axes must use the SAME unit — one allocated texture
   * texel — so a diagonal camera move produces the texture's own aspect.
   *
   * The bug: U divided by snes_width (256) while diorama.c normalizes its U
   * window by the layer texture's allocated width. V later acquired the same
   * bug when fixed vertical-margin capacity made the texture 352 rows while
   * snes_height remained the authentic 224. At t->1 the displayed vertical
   * camera sat at P + (352/224)*delta while the next tick's t=0 landed at
   * P + delta, throwing the excess backward every tick. That 60 Hz sawtooth
   * also sheared diagonal motion because H and V used different units.
   *
   * The assertion is the axis RATIO, which is unit-independent and therefore
   * survives any future change to either denominator: an equal-pixel diagonal
   * move must yield dv/du equal to the texture's own width/height ratio. */
  {
    FrameSlot diag = curr;
    diag.capture_ticks = 1;
    diag.snes_width = 256; diag.snes_height = 224;
    DioramaScrollSnapshot base = prev;
    base.bg1_camera_x = 100; base.bg1_camera_y = 100;
    base.bg2_camera_y = 200;
    diag.bg1_camera_x = 108; diag.bg1_camera_y = 108;   /* +8 px on BOTH axes */
    diag.bg2_camera_y = 204;

    DioramaScrollDelta r = ComputeDioramaScrollDeltaAt(&diag, &base, 0.0f);
    CHECK(r.active);
    /* Equal source-pixel movement normalizes independently by the two fixed
     * allocation axes. Using 224 for V would overshoot by 352/224 and then
     * snap backward at the next captured tick. */
    CHECK(near(r.bg_du[0], -8.0f / (float)kFrameSlotLayerTextureWidth));
    CHECK(near(r.bg_dv[0],
               -8.0f / (float)kFrameSlotLayerTextureHeight));
    CHECK(near(r.bg_dv[0] / r.bg_du[0],
               (float)kFrameSlotLayerTextureWidth /
                   (float)kFrameSlotLayerTextureHeight));
    CHECK(near(r.bg_dv[1],
               -4.0f / (float)kFrameSlotLayerTextureHeight));

    /* And the load-bearing property: at t=1 the shift must equal EXACTLY one
     * tick of motion in texture units at alpha=0. */
    CHECK(near(r.bg_du[0] * (float)kFrameSlotLayerTextureWidth, -8.0f));   /* 8 texels, not 14 */
    CHECK(near(r.bg_dv[0] * (float)kFrameSlotLayerTextureHeight, -8.0f));

    /* Authentic viewport height is not the allocation denominator. Changing
     * this frame descriptor cannot alter the normalized texture movement. */
    FrameSlot taller_view = diag;
    taller_view.snes_height = 240;
    DioramaScrollDelta taller =
        ComputeDioramaScrollDeltaAt(&taller_view, &base, 0.0f);
    CHECK(near(taller.bg_dv[0], r.bg_dv[0]));

    /* The R17/C1 slack margin must not saturate at ordinary walking speed. The
     * margin is 4px of the layer texture; a one-tick move of 4 source px at
     * full phase must therefore fit exactly, and 3px must fit with room. With
     * the 1.75x inflation, saturation began at 2.29px — inside walking range. */
    const float uv_slack = 4.0f / (float)kFrameSlotLayerTextureWidth;
    for (int px = 1; px <= 4; px++) {
      FrameSlot walk = diag;
      walk.bg1_camera_x = 100 + px;
      walk.bg1_camera_y = 100;
      DioramaScrollDelta w = ComputeDioramaScrollDeltaAt(&walk, &base, 0.0f);
      CHECK(w.active);
      CHECK(fabsf(w.bg_du[0]) <= uv_slack + 1e-6f); /* no clamping */
    }
  }

  /* ── THICKNESS: the extruded near face ("skirt") ────────────────────── */
  {
    const float kZ = 0.50f, kY = -0.5f, kThick = 0.20f;
    float y = 0.0f, z = 0.0f, sh = 0.0f;

    /* At the fold (t=0) the skirt must start EXACTLY on the plane's bottom edge
     * and at full brightness. Any offset here is a visible seam, and any shade
     * below 1 is a dark line along the fold. */
    DioramaSkirtVertex(0.0f, kZ, kY, kThick, &y, &z, &sh);
    CHECK(near(y, kY));
    CHECK(near(z, kZ));
    CHECK(near(sh, 1.0f));

    /* At the near lip (t=1) it has come forward by the full thickness and
     * dropped by half of it. */
    DioramaSkirtVertex(1.0f, kZ, kY, kThick, &y, &z, &sh);
    CHECK(near(z, kZ + kThick));
    CHECK(near(y, kY - kThick * 0.5f));
    CHECK(near(sh, DioramaSkirtNearShade()));
    /* The shade must actually darken, or the fold is invisible -- the whole
     * point of the gradient. */
    CHECK(DioramaSkirtNearShade() < 1.0f);
    CHECK(DioramaSkirtNearShade() > 0.0f);   /* not a black band either */

    /* Monotonic forward in Z and downward in Y across the whole span: a
     * non-monotonic face would self-intersect and z-fight against itself. */
    float prev_z = kZ - 1.0f, prev_y = kY + 1.0f, prev_sh = 2.0f;
    for (int i = 0; i <= 10; i++) {
      DioramaSkirtVertex((float)i / 10.0f, kZ, kY, kThick, &y, &z, &sh);
      CHECK(z >= prev_z);
      CHECK(y <= prev_y);
      CHECK(sh <= prev_sh);
      prev_z = z; prev_y = y; prev_sh = sh;
    }

    /* A rake and a thickness COMPOSE: the caller passes the raked bottom edge as
     * z_bottom, so the skirt starts where the raked plane ends rather than where
     * the unraked plane would have. Getting this wrong tears the fold open,
     * which is the exact defect thickness exists to close. */
    const float kRake = 0.29f;
    DioramaSkirtVertex(0.0f, kZ + kRake, kY, kThick, &y, &z, &sh);
    CHECK(near(z, kZ + kRake));
    DioramaSkirtVertex(1.0f, kZ + kRake, kY, kThick, &y, &z, &sh);
    CHECK(near(z, kZ + kRake + kThick));

  }

  /* ── STACK: fill a depth gap with PARALLEL copies ───────────────────── */
  {
    /* Fillmore act 2's real numbers: water Bg2Hi z=0.21, rock path Bg1 z=0.50.
     * Higher z is nearer the camera, so the fill runs forward across the void. */
    const float kBase = 0.21f, kDepth = 0.29f;
    float z = 0.0f, sh = 0.0f, al = 0.0f;

    /* Copy 0 sits EXACTLY on the plane's own depth, at full shade and opacity.
     * That is what lets the caller skip it -- the plane's own draw IS copy 0, so
     * any offset or dimming here would show as a seam or a double-darkened front
     * face on every stacked layer. */
    DioramaStackCopyShaped(
        0, 4, kBase, kDepth, 0, false, &z, &sh, &al);
    CHECK(near(z, kBase));
    CHECK(near(sh, 1.0f));
    CHECK(near(al, 1.0f));

    /* The last copy reaches exactly the far end of the authored fill -- so
     * stack:0.29 on the water lands its farthest slice on the rock's own depth,
     * which is what "fills the gap" has to mean. */
    DioramaStackCopyShaped(
        3, 4, kBase, kDepth, 0, false, &z, &sh, &al);
    CHECK(near(z, kBase + kDepth));
    CHECK(sh < 1.0f);   /* recedes rather than smearing identical sprites */
    CHECK(al < 1.0f);
    CHECK(al > 0.0f);   /* never invisible: that would be a wasted draw call */

    /* Evenly spaced and monotonic forward, dimming as it goes. Non-monotonic
     * depth would make the painter's-algorithm draw order wrong. */
    float prev_z = kBase - 1.0f, prev_al = 2.0f;
    for (int c = 0; c < 4; c++) {
      DioramaStackCopyShaped(
          c, 4, kBase, kDepth, 0, false, &z, &sh, &al);
      CHECK(z >= prev_z);
      CHECK(al <= prev_al);
      CHECK(near(z, kBase + kDepth * (float)c / 3.0f));
      prev_z = z; prev_al = al;
    }

    /* THE POINT OF THE SHAPE: every copy is at a SINGLE depth, so a stacked
     * layer cannot acquire the two-parallax-rates shear a rake does. Nothing
     * here returns a per-vertex or per-row depth -- the arithmetic is per COPY.
     * If this ever grows a `t` parameter, that invariant has been lost. */
    float za = 0.0f, zb = 0.0f;
    DioramaStackCopyShaped(
        2, 4, kBase, kDepth, 0, false, &za, &sh, &al);
    DioramaStackCopyShaped(
        2, 4, kBase, kDepth, 0, false, &zb, &sh, &al);
    CHECK(near(za, zb));
  }

  /* ── STACK DIRECTION and the redundant-copy rule ─────────────────────── */
  {
    const float kBase = 0.21f, kDepth = 0.29f;
    float z = 0.0f, sh = 0.0f, al = 0.0f;
    /* These must match DioramaStackDirection in diorama_layer_order.h. The pure
     * module deliberately does not include that header, so this is the assertion
     * that keeps the two numberings from drifting apart. */
    const int kFwd = 0, kBack = 1, kBoth = 2;

    /* FORWARD is the default and the reported case: Fillmore act 2's water
     * (z=0.21) sits BEHIND the rock path (z=0.50), and higher z is nearer the
     * camera, so the gap to fill is on the camera side. The far copy must land on
     * the rock's depth. */
    DioramaStackCopyShaped(
        3, 4, kBase, kDepth, kFwd, false, &z, &sh, &al);
    CHECK(near(z, kBase + kDepth));
    CHECK(z > kBase);                    /* forward really means toward camera */
    /* BACKWARD is the mirror: a foreground layer receding into the scene. */
    DioramaStackCopyShaped(
        3, 4, kBase, kDepth, kBack, false, &z, &sh, &al);
    CHECK(near(z, kBase - kDepth));
    CHECK(z < kBase);

    /* BOTH centres the fill on the plane: index 0 is the FAR edge, the last is
     * the near edge, and the span is still exactly `depth`. */
    float z_far = 0.0f, z_near = 0.0f;
    DioramaStackCopyShaped(
        0, 5, kBase, kDepth, kBoth, false, &z_far, &sh, &al);
    DioramaStackCopyShaped(
        4, 5, kBase, kDepth, kBoth, false, &z_near, &sh, &al);
    CHECK(near(z_far, kBase - kDepth * 0.5f));
    CHECK(near(z_near, kBase + kDepth * 0.5f));
    CHECK(near(z_near - z_far, kDepth));

    /* Fade follows DISTANCE from the plane, not signed depth -- otherwise a
     * backward or centred stack would brighten as it receded. */
    DioramaStackCopyShaped(
        3, 4, kBase, kDepth, kBack, false, &z, &sh, &al);
    CHECK(sh < 1.0f && al < 1.0f);
    /* Centred: the two EDGES are equally faded, and the middle is brightest. */
    float sh_far = 0.0f, sh_mid = 0.0f, sh_near = 0.0f;
    DioramaStackCopyShaped(
        0, 5, kBase, kDepth, kBoth, false, &z, &sh_far, &al);
    DioramaStackCopyShaped(
        2, 5, kBase, kDepth, kBoth, false, &z, &sh_mid, &al);
    DioramaStackCopyShaped(
        4, 5, kBase, kDepth, kBoth, false, &z, &sh_near, &al);
    CHECK(near(sh_far, sh_near));
    CHECK(sh_mid > sh_far);
    CHECK(near(sh_mid, 1.0f));
    /* And each EDGE must reach the SAME full falloff a one-sided stack's farthest
     * copy reaches -- a centred fill is not a half-strength one. Asserting only
     * symmetry let a mutation that halved the range survive, because both edges
     * were still equal to each other. */
    float sh_oneside = 0.0f;
    DioramaStackCopyShaped(
        4, 5, kBase, kDepth, kFwd, false, &z, &sh_oneside, &al);
    CHECK(near(sh_far, sh_oneside));
    CHECK(near(sh_near, sh_oneside));

    /* THE REDUNDANT COPY. Drawing the copy that coincides with the plane would
     * double-darken the front face; missing a non-redundant one leaves a gap. */
    CHECK(DioramaStackCopyIsRedundant(0, 4, kFwd));
    CHECK(!DioramaStackCopyIsRedundant(1, 4, kFwd));
    CHECK(DioramaStackCopyIsRedundant(0, 4, kBack));
    /* Centred: index 0 is the far EDGE and must be drawn. */
    CHECK(!DioramaStackCopyIsRedundant(0, 5, kBoth));
    /* An odd centred count has a copy exactly at the plane -- the midpoint. */
    CHECK(DioramaStackCopyIsRedundant(2, 5, kBoth));
    /* An even one does not, so nothing may be skipped. */
    for (int c = 0; c < 4; c++)
      CHECK(!DioramaStackCopyIsRedundant(c, 4, kBoth));
    /* And the skipped midpoint really is at the plane's depth. */
    DioramaStackCopyShaped(
        2, 5, kBase, kDepth, kBoth, false, &z, &sh, &al);
    CHECK(near(z, kBase));
  }

  /* ── VOXEL: a SOLID extrusion, vs a stack's receding layers ──────────── */
  {
    const float kBase = 0.21f, kDepth = 0.20f;
    const int kFwd = 0, kBoth = 2;
    float z_s = 0.0f, sh_s = 0.0f, al_s = 0.0f;
    float z_v = 0.0f, sh_v = 0.0f, al_v = 0.0f;

    /* Same depth arithmetic as a stack -- a voxel is a stack geometrically, so
     * any divergence here would be a bug, not a feature. */
    for (int c = 0; c < 6; c++) {
      DioramaStackCopyShaped(
          c, 6, kBase, kDepth, kFwd, false, &z_s, &sh_s, &al_s);
      DioramaStackCopyShaped(
          c, 6, kBase, kDepth, kFwd, true, &z_v, &sh_v, &al_v);
      CHECK(near(z_s, z_v));
    }

    /* THE DEFINING DIFFERENCE. A stack FADES with depth so the eye reads separate
     * things receding. A voxel must NOT: it is one object with volume, and a
     * falloff would make its own back half look like fog. */
    DioramaStackCopyShaped(
        5, 6, kBase, kDepth, kFwd, false, &z_s, &sh_s, &al_s);
    DioramaStackCopyShaped(
        5, 6, kBase, kDepth, kFwd, true, &z_v, &sh_v, &al_v);
    CHECK(sh_s < 1.0f && al_s < 1.0f);      /* stack recedes */
    CHECK(near(al_v, 1.0f));                /* voxel stays opaque */
    CHECK(sh_v > sh_s);                     /* and much less darkened */

    /* Uniform, not depth-varying: every voxel slice gets the SAME treatment, which
     * is what makes it read as solid. If this ever varies with index the shape has
     * silently become a stack again. */
    float first_sh = 0.0f, first_al = 0.0f;
    DioramaStackCopyShaped(
        0, 8, kBase, kDepth, kFwd, true, &z_v, &first_sh, &first_al);
    for (int c = 1; c < 8; c++) {
      DioramaStackCopyShaped(
          c, 8, kBase, kDepth, kFwd, true, &z_v, &sh_v, &al_v);
      CHECK(near(sh_v, first_sh));
      CHECK(near(al_v, first_al));
    }
    /* Still tinted a little, or an extruded layer is indistinguishable from the
     * flat one it replaced. */
    CHECK(first_sh < 1.0f);
    CHECK(first_sh > 0.5f);   /* but not so dark it reads as a shadow */

    /* Direction still applies -- a voxel can extrude backward or straddle. */
    DioramaStackCopyShaped(
        0, 5, kBase, kDepth, kBoth, true, &z_v, &sh_v, &al_v);
    CHECK(near(z_v, kBase - kDepth * 0.5f));
    /* And the redundant-copy rule is direction-based, not shape-based. */
    CHECK(DioramaStackCopyIsRedundant(0, 6, kFwd));
    CHECK(DioramaStackCopyIsRedundant(2, 5, kBoth));
  }

  /* ── VERTICAL REPEAT: overflow is independent of BG Extents ─────────── */
  {
    DioramaVerticalRepeatPlan repeat;
    CHECK(DioramaVerticalRepeatPlan_Build(32, 224, 288, 352, &repeat));
    CHECK(repeat.source_y0 == 32 && repeat.source_y1 == 256);
    CHECK(repeat.fold_y == 256 && repeat.repeat_height == 224);

    /* The continuation is anchored solely to the immutable authentic band.
     * A BG Extents valid-span plan is intentionally not an input, so fixed,
     * available, or live-tuned margins all produce this same handoff. */
    DioramaVerticalRepeatPlan same;
    CHECK(DioramaVerticalRepeatPlan_Build(32, 224, 320, 352, &same));
    CHECK(same.source_y0 == repeat.source_y0 &&
          same.source_y1 == repeat.source_y1 &&
          same.fold_y == repeat.fold_y &&
          same.repeat_height == repeat.repeat_height);

    /* It also remains defined with no synthetic vertical capture at all, and
     * malformed intervals fail closed with a cleared output. */
    CHECK(DioramaVerticalRepeatPlan_Build(0, 224, 224, 352, &same));
    CHECK(same.source_y0 == 0 && same.source_y1 == 224 && same.fold_y == 224);
    CHECK(!DioramaVerticalRepeatPlan_Build(32, 224, 240, 352, &same));
    CHECK(same.source_y0 == 0 && same.source_y1 == 0 && same.fold_y == 0);
  }

  /* ── FOLDED OVERFLOW: overlap is exact, then the sheet comes forward ── */
  {
    const float y_top = -0.50f;
    const float z_top = -0.30f;
    const float z_handoff = -0.25f;
    const float height = 1.0f;
    const float overlap = 0.25f;
    const float front_z = 0.45f;
    const float front_drop = 0.18f;
    float y = 0.0f, z = 0.0f;

    DioramaOverflowFoldPoint(
        0.0f, y_top, z_top, z_handoff, height, overlap,
        front_z, front_drop, &y, &z);
    CHECK(near(y, y_top) && near(z, z_top));

    /* The host's last drawable row and overflow meet at one exact point. */
    DioramaOverflowFoldPoint(
        overlap, y_top, z_top, z_handoff, height, overlap,
        front_z, front_drop, &y, &z);
    CHECK(near(y, y_top - height * overlap));
    CHECK(near(z, z_handoff));

    /* Halfway through the bend, smoothstep is 0.5. The surface has begun
     * trading vertical continuation for forward depth. */
    DioramaOverflowFoldPoint(
        0.625f, y_top, z_top, z_handoff, height, overlap,
        front_z, front_drop, &y, &z);
    CHECK(near(y, -0.9825f));
    CHECK(near(z, 0.10f));

    DioramaOverflowFoldPoint(
        1.0f, y_top, z_top, z_handoff, height, overlap,
        front_z, front_drop, &y, &z);
    CHECK(near(y, y_top - height * overlap - front_drop));
    CHECK(near(z, front_z));

    /* The eased bend is continuous at the end of the coplanar overlap. */
    float y_before, z_before, y_after, z_after;
    DioramaOverflowFoldPoint(
        overlap - 0.0001f, y_top, z_top, z_handoff, height, overlap,
        front_z, front_drop, &y_before, &z_before);
    DioramaOverflowFoldPoint(
        overlap + 0.0001f, y_top, z_top, z_handoff, height, overlap,
        front_z, front_drop, &y_after, &z_after);
    CHECK(fabsf(y_after - y_before) < 0.001f);
    CHECK(fabsf(z_after - z_before) < 0.001f);

    /* Aitos's opaque repeat is tucked beneath the host's 32-row lower margin
     * and stays flat for two additional native 8 px tile rows. Pin that
     * non-uniform
     * handoff: a uniform 1/6 row would begin bending inside the tolerance and
     * let landing pitch expose a crack. */
    const float aitos_underlap = (32.0f + 16.0f) / 224.0f;
    CHECK(near(DioramaOverflowFoldRowT(0, 7, aitos_underlap), 0.0f));
    CHECK(near(DioramaOverflowFoldRowT(1, 7, aitos_underlap),
               aitos_underlap));
    CHECK(near(DioramaOverflowFoldRowT(7, 7, aitos_underlap), 1.0f));
    float previous_t = -1.0f;
    for (int row = 0; row <= 7; row++) {
      const float row_t = DioramaOverflowFoldRowT(
          row, 7, aitos_underlap);
      CHECK(row_t > previous_t);
      previous_t = row_t;
    }
  }

  /* ── TILT: rake is linear, bow is eased ──────────────────────────────── */
  {
    const float kZ = 0.21f, kAmount = 0.29f;

    /* THE NO-OP GUARANTEE: no tilt authored must return z untouched, exactly, so
     * every unauthored room stays bit-identical. */
    for (int i = 0; i <= 4; i++) {
      float t = (float)i / 4.0f;
      CHECK(DioramaTiltedRowDepth(kZ, 0.0f, 0.0f, t) == kZ);
    }

    /* Both land the BOTTOM edge in the same place -- they are the same fix with
     * different distribution, so `bow:0.29` closes the gap `rake:0.29` closes. */
    CHECK(near(DioramaTiltedRowDepth(kZ, kAmount, 0.0f, 1.0f), kZ + kAmount));
    CHECK(near(DioramaTiltedRowDepth(kZ, 0.0f, kAmount, 1.0f), kZ + kAmount));
    /* And both leave the TOP edge alone. */
    CHECK(near(DioramaTiltedRowDepth(kZ, kAmount, 0.0f, 0.0f), kZ));
    CHECK(near(DioramaTiltedRowDepth(kZ, 0.0f, kAmount, 0.0f), kZ));

    /* THE DIFFERENCE, and the reason bow exists. A bow displaces LESS than a rake
     * everywhere in between, so the upper rows keep their original depth and the
     * parallax change is concentrated near the fold where it is needed. If this
     * ever fails, bow has become a second spelling of rake. */
    for (int i = 1; i < 4; i++) {
      float t = (float)i / 4.0f;
      float r = DioramaTiltedRowDepth(kZ, kAmount, 0.0f, t);
      float b = DioramaTiltedRowDepth(kZ, 0.0f, kAmount, t);
      CHECK(b < r);
    }
    /* Concretely at the midpoint: linear is half the displacement, quadratic a
     * quarter. Hand-computed rather than via the function under test. */
    CHECK(near(DioramaTiltedRowDepth(kZ, kAmount, 0.0f, 0.5f),
               kZ + kAmount * 0.5f));
    CHECK(near(DioramaTiltedRowDepth(kZ, 0.0f, kAmount, 0.5f),
               kZ + kAmount * 0.25f));

    /* Monotonic for a positive tilt either way: a non-monotonic plane would fold
     * through itself. */
    float prev_r = kZ - 1.0f, prev_b = kZ - 1.0f;
    for (int i = 0; i <= 8; i++) {
      float t = (float)i / 8.0f;
      float r = DioramaTiltedRowDepth(kZ, kAmount, 0.0f, t);
      float b = DioramaTiltedRowDepth(kZ, 0.0f, kAmount, t);
      CHECK(r >= prev_r);
      CHECK(b >= prev_b);
      prev_r = r; prev_b = b;
    }

    /* Negative tilts go the other way, and a bow still eases. */
    CHECK(near(DioramaTiltedRowDepth(kZ, 0.0f, -kAmount, 1.0f), kZ - kAmount));
    CHECK(DioramaTiltedRowDepth(kZ, 0.0f, -kAmount, 0.5f) >
          DioramaTiltedRowDepth(kZ, -kAmount, 0.0f, 0.5f));

    /* They SUM: a gentle linear tilt with extra curve near the bottom. */
    CHECK(near(DioramaTiltedRowDepth(kZ, 0.10f, 0.20f, 1.0f), kZ + 0.30f));
    CHECK(near(DioramaTiltedRowDepth(kZ, 0.10f, 0.20f, 0.5f),
               kZ + 0.10f * 0.5f + 0.20f * 0.25f));

    /* t clamps, so a caller cannot project rows outside the layer. */
    CHECK(near(DioramaTiltedRowDepth(kZ, kAmount, 0.0f, -1.0f), kZ));
    CHECK(near(DioramaTiltedRowDepth(kZ, kAmount, 0.0f, 2.0f), kZ + kAmount));
  }

  if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  puts("diorama_scroll_math_test: PASS");
  return 0;
}
