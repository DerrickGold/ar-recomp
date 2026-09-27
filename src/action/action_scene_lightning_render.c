/* ActionSceneLightningRender: lightning and beam effects in action scenes: the
 * projected ribbons behind boss, Marahna and trap lightning, the Bloodpool
 * and Centaur bosses' bolts, and sword-beam trails, each as particles plus
 * lighting.
 * Phase: pure.
 * Tests: tests/action_effect_render_test.c */
#include "action/action_effect_render_internal.h"

/* Visuals $00-$05 are not frames of one generic bolt. The `$7E:5000`
 * compositions author two different centre lines (vertical and diagonal),
 * each clipped to long/medium/short lengths. These are the per-row centroids
 * of the real 8x8 OAM parts at $5346/$5401/$5492 and $54F2/$55C2/$5661.
 * Following them fixes both endpoint placement and every bend angle; a coarse
 * interpolation across the culling rect cannot recover this information. */
static const int8_t kBossLightningVerticalX[] = {
  4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 6, 7, 6, 5, 7, 7, 2, 5, 4,
  4, 4, 4, 6, 6, 4,
};
static const int8_t kBossLightningDiagonalX[] = {
  0, -4, -5, -5, -6, -12, -10, -11, -16, -20, -20, -22, -26,
  -30, -30, -32, -30, -29, -29, -33, -36, -36, -40, -44, -44,
};

/* Centaur $635B..$645E: bright-pixel centroids sampled every eight rows
 * from the eight growing bolt compositions (CHR $13:B12F). They are not
 * scaled copies: the short/medium ends and the full diagonal turn differ. */
static const int8_t kCentaurLightningX[8][14] = {
    {-1, -2, 4},
    {-1, -2, 2, 8, 12, 7, -2},
    {-1, -2, 2, 8, 12, 0, -13, -17, -18, -12},
    {-1, -2, 2, 8, 12, 0, -13, -17, -18, -14, -8, -4, -9, -16},
    {-2, -8, -14},
    {-2, -14, -27, -34, -39, -46},
    {-2, -14, -27, -34, -39, -46, -48, -46, -52, -60},
    {-2, -14, -27, -34, -39, -46, -48, -46, -52, -56, -51, -44, -46, -42},
};

static ArRenderColorF BossLightningColor(const ActionEffectInstance *effect, ArRenderColorF color) {
  if (effect->kind == kActionEffect_CentaurLightning) {
    /* The same white-hot core/corona style as the Wizard, in the Centaur's
     * native blue palette. Swapping red/blue preserves the alpha profile. */
    const float red = color.r;
    color.r = color.b;
    color.b = red;
  }
  return color;
}

static bool BossLightningPathFor(const ActionEffectInstance *effect,
                                 const int8_t **path_x,
                                 unsigned *joint_count) {
  if (!effect || !joint_count) return false;
  if (effect->kind == kActionEffect_CentaurLightning) {
    static const uint8_t kCounts[] = {3, 7, 10, 14, 3, 6, 10, 14};
    if (effect->visual < 0x19 || effect->visual > 0x20) return false;
    const unsigned visual = effect->visual - 0x19;
    if (path_x) *path_x = kCentaurLightningX[visual];
    *joint_count = kCounts[visual];
    return true;
  }
  if (effect->visual > 5u) return false;
  const unsigned family_visual = effect->visual % 3u;
  if (path_x) {
    *path_x = effect->visual < 3u ? kBossLightningVerticalX
                                 : kBossLightningDiagonalX;
  }
  *joint_count = family_visual == 0u ? 25u
      : (family_visual == 1u ? 19u : 13u);
  return true;
}

static bool BossLightningPathPoint(const ActionEffectInstance *effect,
                                   unsigned joint, float *x, float *y) {
  const int8_t *path_x = NULL;
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect, &path_x, &joint_count) ||
      joint >= joint_count || !x || !y)
    return false;
  *x = (float)path_x[joint];
  if (effect->kind == kActionEffect_CentaurLightning) *x += .5f;
  if (effect->flags & kActionEffectFlag_FlipHorizontal) *x = -*x;
  /* `$8D68`'s action-OBJ emitter stores Y with one extra draw-bias pixel
   * after the camera-origin bias cancels. Subtract it here so the filament
   * runs through the emitted tile centres, not one row below them. */
  *y = (effect->kind == kActionEffect_CentaurLightning ? 3.5f : -80.0f) + (float)joint * 8.0f;
  return true;
}

static bool BossLightningPathSample(const ActionEffectInstance *effect,
                                    float along, float *x, float *y) {
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect, NULL, &joint_count) || !x || !y ||
      !isfinite(along))
    return false;
  along = fmaxf(0.0f, fminf(1.0f, along));
  const float scaled = along * (float)(joint_count - 1u);
  unsigned segment = (unsigned)scaled;
  if (segment >= joint_count - 1u)
    return BossLightningPathPoint(effect, joint_count - 1u, x, y);
  float x0, y0, x1, y1;
  if (!BossLightningPathPoint(effect, segment, &x0, &y0) ||
      !BossLightningPathPoint(effect, segment + 1u, &x1, &y1))
    return false;
  const float t = scaled - (float)segment;
  *x = x0 + (x1 - x0) * t;
  *y = y0 + (y1 - y0) * t;
  return true;
}

static bool AppendProjectedRibbonSegments(
    ActionEffectGeometryWriter *writer, const ArRenderPointF *points,
    const float *scales, unsigned joint_count, float half_width,
    ArRenderColorF color) {
  if (!writer || !points || !scales || joint_count < 2u) return true;
  const unsigned segment_count = joint_count - 1u;
  if (!Reserve(writer, (int)segment_count * 4, (int)segment_count * 6))
    return false;

  for (unsigned i = 0; i < segment_count; i++) {
    float dx = points[i + 1].x - points[i].x;
    float dy = points[i + 1].y - points[i].y;
    const float length = hypotf(dx, dy);
    if (length < 0.001f) continue;
    dx /= length;
    dy /= length;
    const float taper0 = i == 0 ? 0.55f : 1.0f;
    const float taper1 = i + 1 == segment_count
        ? 0.55f : 1.0f;
    const float width0 = half_width * scales[i] * taper0;
    const float width1 = half_width * scales[i + 1] * taper1;
    const int base = writer->vertex_count;
    writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
      {points[i].x - dy * width0, points[i].y + dx * width0},
      color, {0.0f, 0.0f},
    };
    writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
      {points[i].x + dy * width0, points[i].y - dx * width0},
      color, {0.0f, 0.0f},
    };
    writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
      {points[i + 1].x + dy * width1, points[i + 1].y - dx * width1},
      color, {0.0f, 0.0f},
    };
    writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
      {points[i + 1].x - dy * width1, points[i + 1].y + dx * width1},
      color, {0.0f, 0.0f},
    };
    static const int kQuad[6] = {0, 1, 2, 0, 2, 3};
    for (int j = 0; j < 6; j++)
      writer->indices[writer->index_count++] = base + kQuad[j];
  }
  return true;
}

static bool AppendBossLightningRibbonLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float half_width, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectLightningSegments + 1 };
  ArRenderPointF points[kJoints];
  float scales[kJoints];
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect, NULL, &joint_count)) return true;
  for (unsigned i = 0; i < joint_count; i++) {
    float local_x, local_y, scale_x, scale_y;
    if (!BossLightningPathPoint(effect, i, &local_x, &local_y) ||
        !ProjectWithScale(effect, project_point, userdata, local_x, local_y,
                          &points[i], &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  return AppendProjectedRibbonSegments(writer, points, scales, joint_count,
                                       half_width, color);
}

static bool AppendBossLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->phase != kActionEffectPhase_BossLightningStrike) return true;
  const float pulse = DeterministicPulse(effect);
  ArRenderColorF corona = {1.00f, 0.54f, 0.04f, 0.17f * pulse};
  ArRenderColorF filament = {1.00f, 0.98f, 0.68f, 0.88f * pulse};
  return AppendBossLightningRibbonLayer(writer, effect, 4.8f, BossLightningColor(effect, corona),
                                        project_point, userdata) &&
         AppendBossLightningRibbonLayer(writer, effect, 1.15f, BossLightningColor(effect, filament),
                                        project_point, userdata);
}

static bool AppendMarahnaLightningRibbonLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float half_width, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectMarahnaLightningSegments + 1 };
  ArRenderPointF points[kJoints];
  float scales[kJoints];
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const bool horizontal = rect->x1 - rect->x0 >= rect->y1 - rect->y0;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->phase_ticks);
  for (unsigned i = 0; i < kJoints; i++) {
    const float along = (float)i / (float)(kJoints - 1u);
    float x = horizontal ? rect->x0 + (rect->x1 - rect->x0) * along
                         : mid_x;
    float y = horizontal ? mid_y
                         : rect->y0 + (rect->y1 - rect->y0) * along;
    if (i && i + 1u < kJoints) {
      const uint32_t seed = DeterministicHash_Mix32(
          effect->generation * 0x9E3779B9u ^ i * 0x85EBCA6Bu);
      const float static_bend = HashUnit(seed) - 0.5f;
      const float animated_bend =
          TriangleWave(ticks + i * 5u, 13u) - 0.5f;
      const float bend = static_bend * 3.0f + animated_bend * 5.0f;
      if (horizontal) y += bend;
      else x += bend;
    }
    float scale_x, scale_y;
    if (!ProjectWithScale(effect, project_point, userdata, x, y, &points[i],
                          &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  return AppendProjectedRibbonSegments(writer, points, scales, kJoints,
                                       half_width, color);
}

static bool AppendMarahnaLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->kind != kActionEffect_MarahnaLightningLink ||
      effect->phase != kActionEffectPhase_MarahnaLightningActive)
    return true;
  const float pulse = DeterministicPulse(effect);
  const ArRenderColorF corona = {0.24f, 0.34f, 1.00f, 0.24f * pulse};
  const ArRenderColorF filament = {0.90f, 0.98f, 1.00f, 0.94f * pulse};
  return AppendMarahnaLightningRibbonLayer(
             writer, effect, 4.0f, corona, project_point, userdata) &&
      AppendMarahnaLightningRibbonLayer(
             writer, effect, 1.05f, filament, project_point, userdata);
}

static void MarahnaBossBoltEndpoints(const ActionEffectInstance *effect,
                                     float *x0, float *y0,
                                     float *x1, float *y1) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const bool left = effect->velocity_x < 0;
  *x0 = left ? rect->x1 : rect->x0;
  *y0 = rect->y0;
  *x1 = left ? rect->x0 : rect->x1;
  *y1 = rect->y1;
}

static bool AppendMarahnaBossLightningRibbonLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float half_width, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectMarahnaBossLightningSegments + 1 };
  ArRenderPointF points[kJoints];
  float scales[kJoints];
  float x0, y0, x1, y1;
  MarahnaBossBoltEndpoints(effect, &x0, &y0, &x1, &y1);
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float length = hypotf(dx, dy);
  if (length < 0.001f) return true;
  const float normal_x = -dy / length;
  const float normal_y = dx / length;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->phase_ticks);
  for (unsigned i = 0; i < kJoints; i++) {
    const float along = (float)i / (float)(kJoints - 1u);
    float x = x0 + dx * along;
    float y = y0 + dy * along;
    if (i && i + 1u < kJoints) {
      const uint32_t seed = DeterministicHash_Mix32(
          effect->generation * 0x9E3779B9u ^ i * 0x85EBCA6Bu);
      const float bend = (HashUnit(seed) - 0.5f) * 3.0f +
          (TriangleWave(ticks + i * 5u, 11u) - 0.5f) * 5.0f;
      x += normal_x * bend;
      y += normal_y * bend;
    }
    float scale_x, scale_y;
    if (!ProjectWithScale(effect, project_point, userdata, x, y, &points[i],
                          &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  return AppendProjectedRibbonSegments(writer, points, scales, kJoints,
                                       half_width, color);
}

static bool AppendMarahnaBossLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->kind != kActionEffect_MarahnaBossLightning ||
      effect->phase != kActionEffectPhase_MarahnaBossLightningBolt)
    return true;
  const float pulse = DeterministicPulse(effect);
  const ArRenderColorF corona = {0.22f, 0.48f, 1.00f, 0.27f * pulse};
  const ArRenderColorF filament = {0.92f, 0.99f, 1.00f, 0.96f * pulse};
  return AppendMarahnaBossLightningRibbonLayer(
             writer, effect, 4.4f, corona, project_point, userdata) &&
      AppendMarahnaBossLightningRibbonLayer(
             writer, effect, 1.10f, filament, project_point, userdata);
}

static bool AppendSwordBeamTrailLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float length, float head_half_width, float tail_half_width,
    ArRenderColorF head_color, ArRenderColorF tail_color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  float hx = 1.0f, hy = 0.0f;
  if (!SceneActorHeading(effect, &hx, &hy)) return true;
  const float px = -hy, py = hx;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float centre_x = (rect->x0 + rect->x1) * 0.5f;
  const float centre_y = (rect->y0 + rect->y1) * 0.5f;
  const float tail_x = centre_x - hx * length;
  const float tail_y = centre_y - hy * length;
  const float local_x[] = {
    centre_x + px * head_half_width,
    centre_x - px * head_half_width,
    tail_x - px * tail_half_width,
    tail_x + px * tail_half_width,
  };
  const float local_y[] = {
    centre_y + py * head_half_width,
    centre_y - py * head_half_width,
    tail_y - py * tail_half_width,
    tail_y + py * tail_half_width,
  };
  ArRenderPointF points[4];
  for (unsigned i = 0; i < 4; i++)
    if (!project_point(userdata, effect, local_x[i], local_y[i], &points[i]))
      return true;
  if (!Reserve(writer, 4, 6)) return false;
  const int base = writer->vertex_count;
  writer->vertices[writer->vertex_count++] =
      (ArRenderVertex2D){points[0], head_color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] =
      (ArRenderVertex2D){points[1], head_color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] =
      (ArRenderVertex2D){points[2], tail_color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] =
      (ArRenderVertex2D){points[3], tail_color, {0.0f, 0.0f}};
  static const int kQuad[6] = {0, 1, 2, 0, 2, 3};
  for (unsigned i = 0; i < 6; i++)
    writer->indices[writer->index_count++] = base + kQuad[i];
  return true;
}

static bool AppendSwordBeamTrail(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->kind != kActionEffect_SwordBeam ||
      effect->phase != kActionEffectPhase_SwordBeamFlight)
    return true;
  const float pulse = DeterministicPulse(effect);
  ArRenderColorF outer_head = {0.32f, 0.78f, 1.00f, 0.19f * pulse};
  ArRenderColorF outer_tail = {0.05f, 0.22f, 0.82f, 0.00f};
  ArRenderColorF inner_head = {0.78f, 0.98f, 1.00f, 0.27f * pulse};
  ArRenderColorF inner_tail = {0.10f, 0.42f, 1.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float crescent_half_height = (rect->y1 - rect->y0) * 0.5f;
  const float outer_head_width = fmaxf(6.0f, crescent_half_height * 0.95f);
  const float inner_head_width = fmaxf(2.75f,
                                       crescent_half_height * 0.58f);
  /* Both layers now meet nearly the full decoded crescent height. They remain
   * low-alpha and taper immediately, avoiding the detached headlight while
   * fixing the centreline-only attachment captured in 20260810-190729. */
  return AppendSwordBeamTrailLayer(
             writer, effect, 80.0f, outer_head_width, 2.0f,
             outer_head, outer_tail, project_point, userdata) &&
      AppendSwordBeamTrailLayer(
             writer, effect, 56.0f, inner_head_width, 1.0f,
             inner_head, inner_tail, project_point, userdata);
}
static const SceneParticleLifetime kSceneLightningLifetime = {11, 6, 7};

bool AppendSwordBeamParticles(ActionEffectGeometryWriter *writer,
                                     const ActionEffectInstance *effect,
                                     ActionEffectProjectPointFn project_point,
                                     void *userdata) {
  const unsigned count = kActionSceneEffectSwordStarCount;
  const ArRenderColorF hot = {0.96f, 1.00f, 1.00f, 1.00f};
  const ArRenderColorF cool = {0.10f, 0.48f, 1.00f, 0.55f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneActorHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    float x = 0.0f, y = 0.0f;
    float sword_path_t = 0.0f;
    /* Sixteen fixed cross-sections span the path, each with top/centre/
     * bottom lanes. Position depends only on identity; the materialization
     * clock below changes alpha and size without pushing stars backward. */
    const unsigned lane_count = 3u;
    const unsigned cross_section_count = count / lane_count;
    const unsigned cross_section = i / lane_count;
    sword_path_t =
        cross_section_count > 1u
            ? (float)cross_section / (float)(cross_section_count - 1u)
            : 0.0f;
    const float centre_x = (rect->x0 + rect->x1) * 0.5f;
    const float centre_y = (rect->y0 + rect->y1) * 0.5f;
    const float half_height = (rect->y1 - rect->y0) * 0.5f;
    const float lane = (float)(i % lane_count) - 1.0f;
    const float lane_half_span =
        fmaxf(2.0f, half_height - 2.5f) * (1.0f - 0.35f * sword_path_t);
    const float jitter = (HashUnit(seed ^ 0x53u) - 0.5f) * 1.5f;
    const float side = lane * lane_half_span + jitter;
    const float distance = 4.0f + 84.0f * sword_path_t;
    x = centre_x - heading_x * distance - heading_y * side;
    y = centre_y - heading_y * distance + heading_x * side;

    ArRenderColorF color = MixColor(hot, cool, sword_path_t);
    float materialize = TriangleWave(visual_ticks + i * 7u, 18u);
    materialize = materialize * materialize * (3.0f - 2.0f * materialize);
    color.a *= materialize * (1.0f - 0.35f * sword_path_t) *
               (0.82f + 0.18f * HashUnit(seed ^ 0xA7u));
    const float base_size =
        1.15f + 1.30f * (1.0f - sword_path_t) *
                    (0.78f + 0.22f * HashUnit(seed ^ 0xD3u));
    const float star_size = base_size * (0.70f + 0.50f * materialize);
    if (!AppendSceneStarParticle(writer, effect, x, y, star_size, color,
                                 project_point, userdata))
      return false;
  }
  return true;
}

bool AppendSwordBeamLighting(ActionEffectGeometryWriter *writer,
                                    const ActionEffectInstance *effect,
                                    ActionEffectProjectPointFn project_point,
                                    void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  float hx = 1.0f, hy = 0.0f;
  SceneActorHeading(effect, &hx, &hy);
  static const ActionEffectGlowStyle kSpill = {
      .radius_x = 24.0f,
      .radius_y = 22.0f,
      .ring_scale = {0.25f, 0.64f, 1.0f},
      .centre = {0.52f, 0.88f, 1.00f, 0.13f},
      .ring = {{0.26f, 0.70f, 1.00f, 0.09f},
               {0.08f, 0.34f, 0.98f, 0.035f},
               {0.02f, 0.10f, 0.60f, 0.00f}},
      .flare = 0.035f,
      .rise = 0.11f};
  spill = kSpill;
  spill.axis_x = hx;
  spill.axis_y = hy;
  spill.lift_x = -hx;
  spill.lift_y = -hy;
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 10.0f,
      .radius_y = 18.0f,
      .ring_scale = {0.20f, 0.58f, 1.0f},
      .centre = {0.94f, 1.00f, 1.00f, 0.52f},
      .ring = {{0.48f, 0.92f, 1.00f, 0.30f},
               {0.12f, 0.56f, 1.00f, 0.10f},
               {0.03f, 0.18f, 0.72f, 0.00f}},
      .flare = 0.025f,
      .rise = 0.10f};
  body = kBody;
  body.axis_x = hx;
  body.axis_y = hy;
  body.lift_x = -hx;
  body.lift_y = -hy;
  body.seed = (unsigned)effect->pulse_generation;
  /* Anchor the restrained halo to the decoded OAM rectangle. Only the
   * outer spill leans slightly into the wake; the luminous core remains
   * on the painted crescent. */
  spill_x = mid_x - hx * 2.0f;
  spill_y = mid_y - hy * 2.0f;
  body_x = mid_x;
  body_y = mid_y;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return AppendSwordBeamTrail(writer, effect, project_point, userdata);
}

bool AppendMarahnaLightningLinkParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {0.98f, 1.00f, 1.00f, 0.96f};
  const ArRenderColorF cool = {0.20f, 0.18f, 1.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneLightningLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const bool horizontal = rect->x1 - rect->x0 >= rect->y1 - rect->y0;
    const float mid_x = (rect->x0 + rect->x1) * 0.5f;
    const float mid_y = (rect->y0 + rect->y1) * 0.5f;
    if (i >= count * 3u / 4u) {
      const bool at_end = (i & 1u) != 0;
      const float endpoint_x =
          horizontal ? (at_end ? rect->x1 : rect->x0) : mid_x;
      const float endpoint_y =
          horizontal ? mid_y : (at_end ? rect->y1 : rect->y0);
      const float *direction =
          kCircle32[(i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
      const float distance = 2.0f + 15.0f * t;
      const float old_distance = 2.0f + 15.0f * previous_t;
      x = endpoint_x + direction[0] * distance;
      y = endpoint_y + direction[1] * distance;
      previous_x = endpoint_x + direction[0] * old_distance;
      previous_y = endpoint_y + direction[1] * old_distance;
    } else {
      const float along = HashUnit(seed ^ 0x29u);
      const float jitter =
          (HashUnit(seed ^ (visual_ticks * 0x27D4EB2Du)) - 0.5f) * 9.0f;
      const float old_jitter = -jitter * 0.45f;
      if (horizontal) {
        x = rect->x0 + (rect->x1 - rect->x0) * along;
        y = mid_y + jitter;
        previous_x = x;
        previous_y = mid_y + old_jitter;
      } else {
        x = mid_x + jitter;
        y = rect->y0 + (rect->y1 - rect->y0) * along;
        previous_x = mid_x + old_jitter;
        previous_y = y;
      }
    }
    width = 0.42f + 0.28f * (1.0f - t);
    reach = 2.0f + 2.5f * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

bool AppendMarahnaLightningLinkLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  const bool horizontal = rect->x1 - rect->x0 >= rect->y1 - rect->y0;
  const float axis_x = horizontal ? 1.0f : 0.0f;
  const float axis_y = horizontal ? 0.0f : 1.0f;
  static const ActionEffectGlowStyle kSpill = {
      .radius_x = 48.0f,
      .radius_y = 16.0f,
      .ring_scale = {0.22f, 0.72f, 1.0f},
      .centre = {0.62f, 0.82f, 1.00f, 0.18f},
      .ring = {{0.40f, 0.62f, 1.00f, 0.12f},
               {0.18f, 0.28f, 0.96f, 0.05f},
               {0.07f, 0.08f, 0.58f, 0.00f}},
      .flare = 0.08f,
      .lift_y = -1.0f};
  spill = kSpill;
  spill.axis_x = axis_x;
  spill.axis_y = axis_y;
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 42.0f,
      .radius_y = 6.0f,
      .ring_scale = {0.14f, 0.84f, 1.0f},
      .centre = {0.98f, 1.00f, 1.00f, 0.74f},
      .ring = {{0.76f, 0.92f, 1.00f, 0.42f},
               {0.30f, 0.46f, 1.00f, 0.15f},
               {0.10f, 0.12f, 0.72f, 0.00f}},
      .flare = 0.15f,
      .lift_y = -1.0f};
  body = kBody;
  body.axis_x = axis_x;
  body.axis_y = axis_y;
  body.seed = (unsigned)effect->pulse_generation;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return AppendMarahnaLightningRibbon(writer, effect, project_point, userdata);
}

bool AppendMarahnaBossLightningParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {0.98f, 1.00f, 1.00f, 0.98f};
  const ArRenderColorF cool = {0.10f, 0.32f, 1.00f, 0.00f};
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneActorHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneLightningLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    if (effect->phase == kActionEffectPhase_MarahnaBossLightningBolt) {
      float x0, y0, x1, y1;
      MarahnaBossBoltEndpoints(effect, &x0, &y0, &x1, &y1);
      const float along = HashUnit(seed ^ 0x29u);
      const float old_along = fmaxf(0.0f, along - 0.18f);
      const float jitter =
          (HashUnit(seed ^ (visual_ticks * 0x27D4EB2Du)) - 0.5f) * 8.0f;
      const float dx = x1 - x0, dy = y1 - y0;
      const float length = fmaxf(0.001f, hypotf(dx, dy));
      x = x0 + dx * along - dy / length * jitter;
      y = y0 + dy * along + dx / length * jitter;
      previous_x = x0 + dx * old_along + dy / length * jitter * 0.4f;
      previous_y = y0 + dy * old_along - dx / length * jitter * 0.4f;
    } else if (effect->phase ==
               kActionEffectPhase_MarahnaBossLightningGroundCharge) {
      /* The post-impact charge slides horizontally along the floor. Eight
       * sparks form a low wake behind it; four short-lived contacts jump
       * around the leading orb so the effect still reads on its compact
       * first animation frame. All coordinates remain in OBJ-local space
       * and therefore share the production flat/Diorama projection. */
      if (i < count * 2u / 3u) {
        const float side = (HashUnit(seed ^ 0x53u) - 0.5f) * (7.0f + 9.0f * t);
        const float distance = 5.0f + 35.0f * t;
        const float old_distance = 5.0f + 35.0f * previous_t;
        x = -heading_x * distance - heading_y * side;
        y = -heading_y * distance + heading_x * side;
        previous_x = -heading_x * old_distance - heading_y * side;
        previous_y = -heading_y * old_distance + heading_x * side;
      } else {
        const float *direction =
            kCircle32[(i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
        const float distance = 3.0f + 18.0f * t;
        const float old_distance = 3.0f + 18.0f * previous_t;
        x = heading_x * 3.0f + direction[0] * distance;
        y = direction[1] * distance * 0.44f;
        previous_x = heading_x * 3.0f + direction[0] * old_distance;
        previous_y = direction[1] * old_distance * 0.44f;
      }
      width = 0.48f + 0.36f * (1.0f - t);
      reach = 2.4f + 3.4f * (1.0f - t);
    } else {
      const float centre_y = -24.0f;
      const float angle_x = HashUnit(seed ^ 0x29u) * 2.0f - 1.0f;
      const float angle_y = HashUnit(seed ^ 0x71u) * 2.0f - 1.0f;
      const float radius =
          effect->phase == kActionEffectPhase_MarahnaBossLightningOrb ? 24.0f
                                                                      : 46.0f;
      x = angle_x * radius;
      y = centre_y + angle_y * radius * 0.46f;
      previous_x = x * 0.82f;
      previous_y = centre_y + (y - centre_y) * 0.82f;
    }
    width = 0.45f + 0.34f * (1.0f - t);
    reach = 2.2f + 3.2f * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

bool AppendMarahnaBossLightningLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  if (effect->phase == kActionEffectPhase_MarahnaBossLightningBolt) {
    float x0, y0, x1, y1;
    MarahnaBossBoltEndpoints(effect, &x0, &y0, &x1, &y1);
    spill_x = body_x = (x0 + x1) * 0.5f;
    spill_y = body_y = (y0 + y1) * 0.5f;
    const float dx = x1 - x0, dy = y1 - y0;
    const float length = fmaxf(0.001f, hypotf(dx, dy));
    static const ActionEffectGlowStyle kSpill = {
        .radius_x = 38.0f,
        .radius_y = 16.0f,
        .ring_scale = {0.22f, 0.72f, 1.0f},
        .centre = {0.66f, 0.88f, 1.00f, 0.21f},
        .ring = {{0.38f, 0.68f, 1.00f, 0.14f},
                 {0.12f, 0.30f, 1.00f, 0.055f},
                 {0.03f, 0.09f, 0.64f, 0.00f}},
        .flare = 0.09f,
        .lift_y = -1.0f};
    spill = kSpill;
    spill.axis_x = dx / length;
    spill.axis_y = dy / length;
    spill.seed = (unsigned)effect->record_address;
    static const ActionEffectGlowStyle kBody = {
        .radius_x = 29.0f,
        .radius_y = 6.0f,
        .ring_scale = {0.14f, 0.82f, 1.0f},
        .centre = {0.98f, 1.00f, 1.00f, 0.82f},
        .ring = {{0.74f, 0.94f, 1.00f, 0.46f},
                 {0.24f, 0.54f, 1.00f, 0.17f},
                 {0.06f, 0.14f, 0.76f, 0.00f}},
        .flare = 0.16f,
        .lift_y = -1.0f};
    body = kBody;
    body.axis_x = dx / length;
    body.axis_y = dy / length;
    body.seed = (unsigned)effect->pulse_generation;
  } else if (effect->phase ==
             kActionEffectPhase_MarahnaBossLightningGroundCharge) {
    float hx = 1.0f, hy = 0.0f;
    SceneActorHeading(effect, &hx, &hy);
    spill_x = mid_x - hx * 4.0f;
    spill_y = mid_y - hy * 4.0f;
    body_x = mid_x;
    body_y = mid_y;
    static const ActionEffectGlowStyle kSpill = {
        .radius_x = 43.0f,
        .radius_y = 19.0f,
        .ring_scale = {0.22f, 0.70f, 1.0f},
        .centre = {0.62f, 0.86f, 1.00f, 0.24f},
        .ring = {{0.34f, 0.64f, 1.00f, 0.16f},
                 {0.10f, 0.26f, 0.98f, 0.06f},
                 {0.02f, 0.07f, 0.58f, 0.00f}},
        .flare = 0.10f,
        .rise = 0.06f};
    spill = kSpill;
    spill.axis_x = hx;
    spill.axis_y = hy;
    spill.lift_x = -hx;
    spill.lift_y = -hy;
    spill.seed = (unsigned)effect->record_address;
    static const ActionEffectGlowStyle kBody = {
        .radius_x = 22.0f,
        .radius_y = 13.0f,
        .ring_scale = {0.15f, 0.76f, 1.0f},
        .centre = {0.98f, 1.00f, 1.00f, 0.88f},
        .ring = {{0.72f, 0.94f, 1.00f, 0.52f},
                 {0.22f, 0.50f, 1.00f, 0.18f},
                 {0.04f, 0.12f, 0.72f, 0.00f}},
        .flare = 0.16f,
        .rise = 0.08f};
    body = kBody;
    body.axis_x = hx;
    body.axis_y = hy;
    body.lift_x = -hx;
    body.lift_y = -hy;
    body.seed = (unsigned)effect->pulse_generation;
  } else {
    const bool orb =
        effect->phase == kActionEffectPhase_MarahnaBossLightningOrb;
    spill = (ActionEffectGlowStyle){
        .radius_x = orb ? 46.0f : 64.0f,
        .radius_y = orb ? 38.0f : 31.0f,
        .ring_scale = {0.22f, 0.68f, 1.0f},
        .centre = {0.62f, 0.84f, 1.00f, orb ? 0.26f : 0.18f},
        .ring = {{0.34f, 0.66f, 1.00f, orb ? 0.17f : 0.12f},
                 {0.11f, 0.27f, 0.96f, 0.055f},
                 {0.03f, 0.07f, 0.55f, 0.00f}},
        .flare = 0.10f,
        .axis_x = 1.0f,
        .lift_y = -1.0f,
        .seed = (unsigned)effect->record_address,
    };
    body = (ActionEffectGlowStyle){
        .radius_x = orb ? 26.0f : 50.0f,
        .radius_y = orb ? 26.0f : 15.0f,
        .ring_scale = {0.16f, 0.78f, 1.0f},
        .centre = {0.98f, 1.00f, 1.00f, orb ? 0.86f : 0.64f},
        .ring = {{0.70f, 0.92f, 1.00f, orb ? 0.50f : 0.34f},
                 {0.22f, 0.48f, 1.00f, 0.15f},
                 {0.05f, 0.12f, 0.70f, 0.00f}},
        .flare = 0.15f,
        .axis_x = 1.0f,
        .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
    };
    spill_y = body_y = -24.0f;
  }
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return AppendMarahnaBossLightningRibbon(writer, effect, project_point,
                                          userdata);
}

bool AppendLightningTrapParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {0.98f, 1.00f, 1.00f, 0.92f};
  const ArRenderColorF cool = {0.18f, 0.48f, 1.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneLightningLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    /* Most sparks crawl across the full bolt; the last quarter burst away
     * from its lower impact so the strike has both a shaft and a contact. */
    if (i >= count * 3u / 4u) {
      const float *direction =
          kCircle32[(i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
      const float distance = 3.0f + 20.0f * t;
      const float old_distance = 3.0f + 20.0f * previous_t;
      x = (rect->x0 + rect->x1) * 0.5f + direction[0] * distance;
      y = rect->y1 + direction[1] * distance * 0.55f;
      previous_x = (rect->x0 + rect->x1) * 0.5f + direction[0] * old_distance;
      previous_y = rect->y1 + direction[1] * old_distance * 0.55f;
    } else {
      const float along = HashUnit(seed ^ 0x29u);
      const float base_y = rect->y0 + (rect->y1 - rect->y0) * along;
      const float jitter =
          (HashUnit(seed ^ (visual_ticks * 0x27D4EB2Du)) - 0.5f) * 12.0f;
      const float old_jitter = -jitter * 0.55f;
      x = (rect->x0 + rect->x1) * 0.5f + jitter;
      y = base_y + (t - 0.5f) * 5.0f;
      previous_x = (rect->x0 + rect->x1) * 0.5f + old_jitter;
      previous_y = base_y + (previous_t - 0.5f) * 5.0f;
    }
    width = 0.42f + 0.28f * (1.0f - t);
    reach = 2.0f + 2.5f * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

bool AppendLightningTrapLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  spill = (ActionEffectGlowStyle){
      .radius_x = 31.0f,
      .radius_y = fmaxf(46.0f, (rect->y1 - rect->y0) * 0.59f),
      /* Ring 2 is transparent. Keep the last COLOURED ring at the captured
       * electrode endpoints; the old 0.62 scale made the visible aura die
       * near the middle even though its transparent geometry was full-size. */
      .ring_scale = {0.24f, 0.86f, 1.0f},
      .centre = {0.68f, 0.90f, 1.00f, 0.13f},
      .ring = {{0.48f, 0.76f, 1.00f, 0.09f},
               {0.22f, 0.48f, 1.00f, 0.04f},
               {0.08f, 0.18f, 0.72f, 0.00f}},
      .flare = 0.07f,
      .axis_x = 1.0f,
      .lift_y = -1.0f,
      .seed = (unsigned)effect->record_address,
  };
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 9.0f,
      .ring_scale = {0.16f, 0.92f, 1.0f},
      .centre = {1.00f, 1.00f, 1.00f, 0.62f},
      .ring = {{0.82f, 0.96f, 1.00f, 0.34f},
               {0.34f, 0.68f, 1.00f, 0.13f},
               {0.10f, 0.28f, 0.84f, 0.00f}},
      .flare = 0.13f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  body = kBody;
  body.radius_y = fmaxf(38.0f, (rect->y1 - rect->y0) * 0.55f);
  body.seed = (unsigned)effect->pulse_generation;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

bool AppendBossLightningParticles(ActionEffectGeometryWriter *writer,
                                  const ActionEffectInstance *effect,
                                  ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot =
      BossLightningColor(effect, (ArRenderColorF){1.00f, 1.00f, 0.82f, 0.98f});
  const ArRenderColorF cool =
      BossLightningColor(effect, (ArRenderColorF){1.00f, 0.34f, 0.01f, 0.00f});
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float emission_y = effect->kind == kActionEffect_CentaurLightning
                               ? (rect->y0 + rect->y1) * .5f
                               : rect->y1 - 2.0f;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneLightningLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    if (effect->phase == kActionEffectPhase_BossLightningStrike) {
      float endpoint_x = (rect->x0 + rect->x1) * 0.5f;
      float endpoint_y = rect->y1;
      BossLightningPathSample(effect, 1.0f, &endpoint_x, &endpoint_y);
      if (i >= count * 3u / 4u) {
        const float *direction =
            kCircle32[(i * 9u + (seed >> 11)) & kActionEffectGlowSegmentMask];
        const float distance = 4.0f + 24.0f * t;
        const float old_distance = 4.0f + 24.0f * previous_t;
        x = endpoint_x + direction[0] * distance;
        y = endpoint_y + direction[1] * distance * 0.48f;
        previous_x = endpoint_x + direction[0] * old_distance;
        previous_y = endpoint_y + direction[1] * old_distance * 0.48f;
      } else {
        const float birth = HashUnit(seed ^ 0x29u);
        float along = birth + 0.34f * t;
        float previous_along = birth + 0.34f * previous_t;
        if (along > 1.0f) along -= 1.0f;
        if (previous_along > 1.0f) previous_along -= 1.0f;
        if (!BossLightningPathSample(effect, along, &x, &y) ||
            !BossLightningPathSample(effect, previous_along, &previous_x,
                                     &previous_y))
          continue;
      }
      width = 0.55f + 0.38f * (1.0f - t);
      reach = 2.5f + 3.5f * (1.0f - t);
    } else {
      /* Charges and floor bursts expand around their own captured artwork. */
      const float *direction =
          kCircle32[(i * 11u + (seed >> 13)) & kActionEffectGlowSegmentMask];
      const float distance = 2.0f + 26.0f * t;
      const float old_distance = 2.0f + 26.0f * previous_t;
      x = (rect->x0 + rect->x1) * 0.5f + direction[0] * distance;
      y = emission_y + direction[1] * distance * 0.38f;
      previous_x = (rect->x0 + rect->x1) * 0.5f + direction[0] * old_distance;
      previous_y = emission_y + direction[1] * old_distance * 0.38f;
      width = 0.50f + 0.32f * (1.0f - t);
      reach = 2.0f + 2.8f * (1.0f - t);
    }

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

bool AppendBossLightningLighting(ActionEffectGeometryWriter *writer,
                                 const ActionEffectInstance *effect,
                                 ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  if (effect->phase == kActionEffectPhase_BossLightningStrike) {
    BossLightningPathSample(effect, 0.5f, &spill_x, &spill_y);
    body_x = spill_x;
    body_y = spill_y;
    static const ActionEffectGlowStyle kSpill = {
        .ring_scale = {0.24f, 0.72f, 1.0f},
        .centre = {1.00f, 0.72f, 0.17f, 0.19f},
        .ring = {{1.00f, 0.48f, 0.06f, 0.13f},
                 {0.86f, 0.18f, 0.01f, 0.055f},
                 {0.52f, 0.04f, 0.00f, 0.00f}},
        .flare = 0.10f,
        .axis_x = 1.0f,
        .lift_y = -1.0f};
    spill = kSpill;
    spill.radius_x = fmaxf(42.0f, (rect->x1 - rect->x0) * 1.15f);
    spill.radius_y = fmaxf(58.0f, (rect->y1 - rect->y0) * 0.64f);
    spill.seed = (unsigned)effect->record_address;
    static const ActionEffectGlowStyle kBody = {
        .radius_x = 15.0f,
        .ring_scale = {0.16f, 0.78f, 1.0f},
        .centre = {1.00f, 1.00f, 0.86f, 0.70f},
        .ring = {{1.00f, 0.88f, 0.34f, 0.42f},
                 {1.00f, 0.47f, 0.05f, 0.16f},
                 {0.80f, 0.10f, 0.00f, 0.00f}},
        .flare = 0.16f,
        .axis_x = 1.0f,
        .lift_y = -1.0f};
    body = kBody;
    body.radius_y = fmaxf(45.0f, (rect->y1 - rect->y0) * 0.53f);
    body.seed = (unsigned)effect->pulse_generation;
    float start_x, start_y, end_x, end_y;
    if (BossLightningPathSample(effect, 0.0f, &start_x, &start_y) &&
        BossLightningPathSample(effect, 1.0f, &end_x, &end_y)) {
      const float path_x = end_x - start_x;
      const float path_y = end_y - start_y;
      const float path_length = hypotf(path_x, path_y);
      if (path_length > 0.001f) {
        /* Local +Y is the ellipse's long axis; rotate it onto the
         * authored start-to-end chord. The ribbon retains the individual
         * OAM bends while its surrounding body agrees with their angle. */
        spill.axis_x = body.axis_x = path_y / path_length;
        spill.axis_y = body.axis_y = -path_x / path_length;
      }
    }
  } else {
    static const ActionEffectGlowStyle kSpill = {
        .radius_x = 37.0f,
        .radius_y = 15.0f,
        .ring_scale = {0.24f, 0.66f, 1.0f},
        .centre = {1.00f, 0.82f, 0.35f, 0.18f},
        .ring = {{1.00f, 0.60f, 0.12f, 0.13f},
                 {0.92f, 0.28f, 0.02f, 0.055f},
                 {0.58f, 0.05f, 0.00f, 0.00f}},
        .flare = 0.08f,
        .axis_x = 1.0f,
        .lift_y = -1.0f};
    spill = kSpill;
    spill.seed = (unsigned)effect->record_address;
    static const ActionEffectGlowStyle kBody = {
        .radius_x = 18.0f,
        .radius_y = 8.0f,
        .ring_scale = {0.18f, 0.58f, 1.0f},
        .centre = {1.00f, 1.00f, 0.84f, 0.68f},
        .ring = {{1.00f, 0.86f, 0.32f, 0.38f},
                 {1.00f, 0.42f, 0.04f, 0.14f},
                 {0.76f, 0.08f, 0.00f, 0.00f}},
        .flare = 0.12f,
        .axis_x = 1.0f,
        .lift_y = -1.0f};
    body = kBody;
    body.seed = (unsigned)effect->pulse_generation;
    spill_y = body_y = effect->kind == kActionEffect_CentaurLightning ? mid_y : rect->y1 - 2.0f;
    if (effect->phase == kActionEffectPhase_CentaurStaffCharge) {
      spill.radius_x = spill.radius_y = 26;
      body.radius_x = body.radius_y = 10;
    }
  }
  spill.centre = BossLightningColor(effect, spill.centre);
  body.centre = BossLightningColor(effect, body.centre);
  for (int i = 0; i < kActionEffectGlowRings; ++i) {
    spill.ring[i] = BossLightningColor(effect, spill.ring[i]);
    body.ring[i] = BossLightningColor(effect, body.ring[i]);
  }
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return AppendBossLightningRibbon(writer, effect, project_point, userdata);
}
