/* ActionSceneEffectRender: geometry for the effects placed in action scenes
 * (lava pits and reservoirs, molten rock, water splashes, waterfalls and their
 * mist, wall torches, statue fire, fireballs, the flaming wheel, projectiles),
 * each as particles plus lighting, and the dispatch from a scene effect to its
 * family. Lightning lives in action_scene_lightning_render.c.
 * Phase: pure.
 * Tests: tests/action_effect_render_test.c */
#include "action/action_effect_render_internal.h"

/* Mist is a mass of overlapping puffs rather than a precision light halo.
 * Twelve segments keep each silhouette round at SNES presentation scale while
 * making a dense 24-puff volume cheaper than six generic 32-segment glows. */
static const float
kMistCircle12[kActionSceneEffectWaterfallMistCloudSegments][2] = {
  { 1.000000f, 0.000000f }, { 0.866025f, 0.500000f },
  { 0.500000f, 0.866025f }, { 0.000000f, 1.000000f },
  {-0.500000f, 0.866025f }, {-0.866025f, 0.500000f },
  {-1.000000f, 0.000000f }, {-0.866025f,-0.500000f },
  {-0.500000f,-0.866025f }, { 0.000000f,-1.000000f },
  { 0.500000f,-0.866025f }, { 0.866025f,-0.500000f },
};

/* One source-alpha cloud puff. Unlike AppendGlow this is not a light and is
 * never submitted additively: its opaque-ish core actually conceals the hard
 * BG2/skybox seam, while two soft rings feather it into neighbouring puffs.
 * Per-puff radius, tint and opacity let four overlapping tiers imply volume
 * without a backend-specific 3D-noise shader. */
static bool AppendWaterfallMistCloud(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,
    float local_x, float local_y, float local_radius_x, float local_radius_y,
    ArRenderColorF tint, float opacity, unsigned seed,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum {
    kSegments = kActionSceneEffectWaterfallMistCloudSegments,
    kRings = kActionEffectGlowRings,
  };
  ArRenderPointF anchor;
  float scale_x, scale_y;
  if (!ProjectWithScale(effect, project_point, userdata, local_x, local_y,
                        &anchor, &scale_x, &scale_y))
    return true;
  if (!Reserve(writer,
               kActionSceneEffectWaterfallMistCloudVertices,
               kActionSceneEffectWaterfallMistCloudIndices))
    return false;

  const ArRenderColorF white = {1.0f, 1.0f, 1.0f, opacity};
  ArRenderColorF centre_color = MixColor(tint, white, 0.42f);
  centre_color.a = opacity;
  const int centre = writer->vertex_count;
  writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
    anchor, centre_color, {0.0f, 0.0f},
  };

  const float radius_x = fmaxf(4.0f, local_radius_x * scale_x);
  const float radius_y = fmaxf(3.0f, local_radius_y * scale_y);
  static const float kRingScale[kRings] = {0.24f, 0.70f, 1.0f};
  ArRenderColorF ring_color[kRings] = {
    MixColor(tint, white, 0.24f), tint, tint,
  };
  ring_color[0].a = opacity * 0.84f;
  ring_color[1].a = opacity * 0.34f;
  ring_color[2].a = 0.0f;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->pulse_ticks) / 4u;

  int ring_base[kRings];
  for (int ring = 0; ring < kRings; ring++) {
    ring_base[ring] = writer->vertex_count;
    const float wobble = 0.035f + 0.055f * (float)ring;
    for (int segment = 0; segment < kSegments; segment++) {
      const int silhouette_segment =
          segment * kActionEffectGlowSegments / kSegments;
      const float shape = 1.0f + wobble * FlameSilhouette(
          seed, ticks, silhouette_segment);
      writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
        {
          anchor.x + kMistCircle12[segment][0] * radius_x *
              kRingScale[ring] * shape,
          anchor.y + kMistCircle12[segment][1] * radius_y *
              kRingScale[ring] * shape,
        },
        ring_color[ring], {0.0f, 0.0f},
      };
    }
  }

  for (int segment = 0; segment < kSegments; segment++) {
    const int next = (segment + 1) % kSegments;
    writer->indices[writer->index_count++] = centre;
    writer->indices[writer->index_count++] = ring_base[0] + segment;
    writer->indices[writer->index_count++] = ring_base[0] + next;
  }
  for (int ring = 0; ring + 1 < kRings; ring++) {
    for (int segment = 0; segment < kSegments; segment++) {
      const int next = (segment + 1) % kSegments;
      const int inner0 = ring_base[ring] + segment;
      const int inner1 = ring_base[ring] + next;
      const int outer0 = ring_base[ring + 1] + segment;
      const int outer1 = ring_base[ring + 1] + next;
      writer->indices[writer->index_count++] = inner0;
      writer->indices[writer->index_count++] = outer0;
      writer->indices[writer->index_count++] = outer1;
      writer->indices[writer->index_count++] = inner0;
      writer->indices[writer->index_count++] = outer1;
      writer->indices[writer->index_count++] = inner1;
    }
  }
  return true;
}

static bool AppendWaterfallMistCloudVolume(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kColumns = 6, kTiers = 4 };
  _Static_assert(kColumns * kTiers ==
                     kActionSceneEffectWaterfallMistCloudCount,
                 "waterfall cloud layout must fill its bounded budget");
  static const float kTierY[kTiers] = {48.0f, 20.0f, -24.0f, -5.0f};
  static const float kTierRadiusX[kTiers] = {108.0f, 92.0f, 76.0f, 62.0f};
  static const float kTierRadiusY[kTiers] = {88.0f, 70.0f, 54.0f, 32.0f};
  static const float kTierOpacity[kTiers] = {0.090f, 0.115f, 0.135f, 0.205f};
  static const float kTierOffsetX[kTiers] = {0.0f, 28.0f, -20.0f, 12.0f};
  static const ArRenderColorF kTierTint[kTiers] = {
    {0.58f, 0.76f, 0.88f, 1.0f},
    {0.68f, 0.85f, 0.94f, 1.0f},
    {0.78f, 0.91f, 0.98f, 1.0f},
    {0.90f, 0.97f, 1.00f, 1.0f},
  };
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float span = rect->x1 - rect->x0;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->pulse_ticks);

  /* Back to front: broad cool banks establish depth, rising mid-tier puffs
   * break their upper silhouette, and the compact bright tier reads as fresh
   * foam boiling where the waterfall meets the cloud. */
  for (unsigned cloud = 0;
       cloud < kActionSceneEffectWaterfallMistCloudCount; cloud++) {
    const unsigned tier = cloud / kColumns;
    const unsigned column = cloud % kColumns;
    const unsigned seed = DeterministicHash_Mix32(
        (uint32_t)effect->pulse_generation ^
        ((uint32_t)cloud + 1u) * 0x9E3779B9u);
    const unsigned x_period = 132u + tier * 17u + (seed & 15u);
    const unsigned y_period = 96u + tier * 13u + ((seed >> 4) & 15u);
    float x_wave = TriangleWave(ticks + seed % x_period, x_period) * 2.0f - 1.0f;
    float y_wave = TriangleWave(
        ticks + (seed >> 8) % y_period, y_period);
    if (cloud & 1u) x_wave = -x_wave;
    const float lane = ((float)column + 0.5f) / (float)kColumns;
    const float jitter_x = (HashUnit(seed ^ 0x53u) - 0.5f) * 28.0f;
    const float jitter_y = (HashUnit(seed ^ 0xB5u) - 0.5f) *
        (tier == 3u ? 14.0f : 28.0f);
    float x = rect->x0 + span * lane + kTierOffsetX[tier] +
        jitter_x + x_wave * (6.0f + 2.5f * (float)tier);
    float y = kTierY[tier] + jitter_y -
        y_wave * (5.0f + 1.5f * (float)tier);
    /* A fixed first anchor gives the production projection regression one
     * stable point while the other 23 puffs drift independently around it. */
    if (cloud == 0u) {
      x = rect->x0 + span * lane;
      y = kTierY[tier];
    }
    const float size_jitter = 0.88f + 0.24f * HashUnit(seed ^ 0x71u);
    const float breathe = 0.94f + 0.08f * TriangleWave(
        ticks + (seed >> 16) % 113u, 113u);
    const float opacity = kTierOpacity[tier] *
        (0.90f + 0.10f * pulse) *
        (0.92f + 0.08f * HashUnit(seed ^ 0xA7u));
    if (!AppendWaterfallMistCloud(
            writer, effect, x, y,
            kTierRadiusX[tier] * size_jitter * breathe,
            kTierRadiusY[tier] * size_jitter / breathe,
            kTierTint[tier], opacity, seed,
            project_point, userdata))
      return false;
  }
  return true;
}

/* ── Independent scene lights ─────────────────────────────────────────────
 *
 * Scene effects intentionally reuse the same ring-gradient geometry and SDL
 * additive submission as spells. No renderer shader or backend-specific
 * uniform path is involved: GLSL/SPIR-V/MSL support therefore does not gate
 * these accents, and SDL's software/D3D renderers receive the same batch.
 * Unlike a spell cast, each scene record remains an independent light centre. */

bool SceneActorHeading(const ActionEffectInstance *effect,
                              float *x, float *y) {
  if (!effect || !x || !y) return false;
  const float vx = (float)effect->velocity_x;
  const float vy = (float)effect->velocity_y;
  const float speed = hypotf(vx, vy);
  if (speed < 0.001f) return false;
  *x = vx / speed;
  *y = vy / speed;
  return true;
}

static void SceneFireballHeading(const ActionEffectInstance *effect,
                                 float *x, float *y) {
  if (!x || !y) return;
  *x = 1.0f;
  *y = 0.0f;
  if (SceneActorHeading(effect, x, y)) return;
  if (effect && effect->kind == kActionEffect_AitosLavaFireball) {
    /* Its reset frame sits above the pit before relaunch; retain the rising
     * shot's downward wake rather than snapping horizontally while stopped. */
    *x = 0.0f;
    *y = -1.0f;
  } else if (effect && effect->kind == kActionEffect_MarahnaFireball &&
             effect->phase == kActionEffectPhase_MarahnaFireballOrb) {
    /* `$E047` deliberately pauses twice in its left/right animation. A still
     * ball remains a flame: let heat climb instead of inventing a rightward
     * trail for the two zero-velocity entries. */
    *x = 0.0f;
    *y = 1.0f;
  }
}

/* Centres of the twelve 16x16 fireball parts in the wheel's four measured
 * full-ring compositions ($5276/$5398/$54BA/$55DC). The compositions rotate
 * tile art and flip selection, but this symmetric set of authored centres is
 * invariant. Keeping the literal OAM-local anchors prevents a procedural
 * circle from drifting away from the authentic square-round silhouette. */
static const float
kFlamingWheelFireballAnchors[kActionSceneEffectFlamingWheelFireballs][2] = {
  {-24.0f, -24.0f}, {-8.0f, -24.0f}, {8.0f, -24.0f}, {24.0f, -24.0f},
  {-24.0f,  -8.0f}, {24.0f,  -8.0f},
  {-24.0f,   8.0f}, {24.0f,   8.0f},
  {-24.0f,  24.0f}, {-8.0f,  24.0f}, {8.0f,  24.0f}, {24.0f,  24.0f},
};

static bool FlamingWheelHasAuthoredFireballRing(
    const ActionEffectInstance *effect) {
  if (!effect || effect->kind != kActionEffect_FlamingWheel) return false;
  return effect->composition == 0x5276 || effect->composition == 0x5398 ||
      effect->composition == 0x54BA || effect->composition == 0x55DC;
}

static bool AppendFlamingWheelFireballLighting(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!FlamingWheelHasAuthoredFireballRing(effect)) return true;
  for (unsigned i = 0; i < kActionSceneEffectFlamingWheelFireballs; i++) {
    const ActionEffectGlowStyle flame = {
      .radius_x = 10.5f, .radius_y = 12.5f,
      .ring_scale = {0.20f, 0.60f, 1.0f},
      .centre = {1.00f, 1.00f, 0.78f, 0.72f},
      .ring = {{1.00f, 0.70f, 0.16f, 0.42f},
               {1.00f, 0.22f, 0.01f, 0.15f},
               {0.72f, 0.04f, 0.00f, 0.00f}},
      .flare = 0.34f, .rise = 0.38f,
      .axis_x = 1.0f, .lift_y = -1.0f,
      .seed = (unsigned)effect->pulse_generation + i * 0x45D9u,
    };
    if (!AppendGlow(writer, effect, &flame, pulse,
                    kFlamingWheelFireballAnchors[i][0],
                    kFlamingWheelFireballAnchors[i][1],
                    project_point, userdata))
      return false;
  }
  return true;
}

static unsigned LavaReservoirGlowSegmentCount(
    const ActionEffectInstance *effect) {
  if (!effect || effect->kind != kActionEffect_AitosLavaReservoir ||
      effect->geometry.kind != kActionEffectGeometry_Rect)
    return 0;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float width = rect->x1 - rect->x0;
  if (!isfinite(width) || width <= 0.0f) return 0;
  if (width > (float)(kActionSceneEffectMaxLavaGlowSegments *
                      kActionSceneEffectLavaGlowSpanPixels))
    return kActionSceneEffectMaxLavaGlowSegments + 1u;
  return (unsigned)ceilf(
      width / (float)kActionSceneEffectLavaGlowSpanPixels);
}

/* Long Act-2 lakes cannot use one reservoir-wide radial gradient: its outer
 * ring is intentionally transparent, so the ends look unlit until the camera
 * approaches the lake centre. Overlapping bounded emitters keep every part of
 * the lip locally hot and also follow Diorama perspective more faithfully. */
static bool AppendLavaReservoirLighting(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned segments = LavaReservoirGlowSegmentCount(effect);
  if (!segments || segments > kActionSceneEffectMaxLavaGlowSegments)
    return false;
  const float segment_width = (rect->x1 - rect->x0) / (float)segments;
  for (unsigned i = 0; i < segments; i++) {
    const float centre_x = rect->x0 + ((float)i + 0.5f) * segment_width;
    const ActionEffectGlowStyle spill = {
      .radius_x = fmaxf(38.0f, segment_width * 0.72f + 12.0f),
      .radius_y = 42.0f,
      .ring_scale = {0.18f, 0.68f, 1.0f},
      .centre = {1.00f, 0.43f, 0.04f, 0.13f},
      .ring = {{1.00f, 0.28f, 0.01f, 0.10f},
               {0.82f, 0.07f, 0.00f, 0.04f},
               {0.48f, 0.01f, 0.00f, 0.00f}},
      .flare = 0.07f, .rise = 0.10f,
      .axis_x = 1.0f, .lift_y = -1.0f,
      .seed = (unsigned)effect->generation + i * 0x5BD1u,
    };
    const ActionEffectGlowStyle body = {
      .radius_x = fmaxf(30.0f, segment_width * 0.58f + 7.0f),
      .radius_y = 9.0f,
      .ring_scale = {0.15f, 0.78f, 1.0f},
      .centre = {1.00f, 1.00f, 0.72f, 0.62f},
      .ring = {{1.00f, 0.68f, 0.10f, 0.36f},
               {1.00f, 0.20f, 0.01f, 0.13f},
               {0.72f, 0.04f, 0.00f, 0.00f}},
      .flare = 0.12f, .rise = 0.18f,
      .axis_x = 1.0f, .lift_y = -1.0f,
      .seed = (unsigned)effect->pulse_generation + i * 0x7A4Du,
    };
    if (!AppendGlow(writer, effect, &spill, pulse, centre_x, -10.0f,
                    project_point, userdata) ||
        !AppendGlow(writer, effect, &body, pulse, centre_x, 0.0f,
                    project_point, userdata))
      return false;
  }
  return true;
}

bool AppendSceneParticle(ActionEffectGeometryWriter *writer,
                                const ActionEffectInstance *effect,
                                float x, float y, float previous_x,
                                float previous_y, float width, float reach,
                                ArRenderColorF color,
                                ActionEffectProjectPointFn project_point,
                                void *userdata) {
  ArRenderPointF position, previous;
  float scale_x, scale_y;
  if (!ProjectWithScale(effect, project_point, userdata, x, y, &position,
                        &scale_x, &scale_y) ||
      !project_point(userdata, effect, previous_x, previous_y, &previous))
    return true;
  const float output_scale = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  float dx = position.x - previous.x;
  float dy = position.y - previous.y;
  const float length = hypotf(dx, dy);
  if (length < 0.001f) {
    dx = 0.0f;
    dy = -1.0f;
  } else {
    dx /= length;
    dy /= length;
  }
  width *= output_scale;
  reach *= output_scale;
  if (!Reserve(writer, 4, 6)) return false;
  const int base = writer->vertex_count;
  writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
    {position.x + dx * reach, position.y + dy * reach}, color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
    {position.x - dy * width, position.y + dx * width}, color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
    {position.x - dx * reach, position.y - dy * reach}, color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
    {position.x + dy * width, position.y - dx * width}, color, {0.0f, 0.0f}};
  static const int kQuad[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++)
    writer->indices[writer->index_count++] = base + kQuad[i];
  return true;
}

/* A sword-beam sparkle is two crossed additive diamonds. Forty-eight fixed
 * glints independently materialize along the magical path rather than moving
 * backward like fire embers. The bounded extra capacity is explicit in
 * action_effect_render.h.
 * Projected local unit vectors keep the cross on the OBJ plane in Diorama
 * mode instead of leaving it screen-axis-aligned. */
bool AppendSceneStarParticle(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float local_x, float local_y, float size, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  ArRenderPointF centre, sample_x, sample_y;
  if (!project_point(userdata, effect, local_x, local_y, &centre) ||
      !project_point(userdata, effect, local_x + 1.0f, local_y, &sample_x) ||
      !project_point(userdata, effect, local_x, local_y + 1.0f, &sample_y))
    return true;
  float xx = sample_x.x - centre.x, xy = sample_x.y - centre.y;
  float yx = sample_y.x - centre.x, yy = sample_y.y - centre.y;
  const float x_length = hypotf(xx, xy), y_length = hypotf(yx, yy);
  if (x_length < 0.001f || y_length < 0.001f) return true;
  xx /= x_length;
  xy /= x_length;
  yx /= y_length;
  yy /= y_length;
  const float long_x = size * x_length;
  const float long_y = size * 1.35f * y_length;
  const float thin_x = fmaxf(0.45f, size * 0.23f * x_length);
  const float thin_y = fmaxf(0.45f, size * 0.23f * y_length);
  if (!Reserve(writer, 8, 12)) return false;
  const int base = writer->vertex_count;
  const ArRenderPointF points[] = {
    {centre.x + xx * long_x, centre.y + xy * long_x},
    {centre.x + yx * thin_y, centre.y + yy * thin_y},
    {centre.x - xx * long_x, centre.y - xy * long_x},
    {centre.x - yx * thin_y, centre.y - yy * thin_y},
    {centre.x + yx * long_y, centre.y + yy * long_y},
    {centre.x + xx * thin_x, centre.y + xy * thin_x},
    {centre.x - yx * long_y, centre.y - yy * long_y},
    {centre.x - xx * thin_x, centre.y - xy * thin_x},
  };
  for (unsigned i = 0; i < 8; i++)
    writer->vertices[writer->vertex_count++] =
        (ArRenderVertex2D){points[i], color, {0.0f, 0.0f}};
  static const int kDiamonds[12] = {
    0, 1, 2, 0, 2, 3,
    4, 5, 6, 4, 6, 7,
  };
  for (unsigned i = 0; i < 12; i++)
    writer->indices[writer->index_count++] = base + kDiamonds[i];
  return true;
}
static const SceneParticleLifetime kSceneReservoirLifetime = {31, 5, 21};

SceneParticleClock
SceneParticleClockAt(const ActionEffectInstance *effect, unsigned visual_ticks,
                     unsigned index, SceneParticleLifetime timing) {
  const uint32_t seed = DeterministicHash_Mix32(
      effect->pulse_generation * 0x9E3779B9u ^
      (uint32_t)effect->record_address * 0x85EBCA6Bu ^ index * 0xC2B2AE35u);
  const unsigned lifetime =
      timing.minimum + ((seed >> timing.seed_shift) & timing.seed_mask);
  const unsigned birth_phase = seed % lifetime;
  const unsigned age = (visual_ticks + birth_phase) % lifetime;
  const float t = (float)age / (float)(lifetime - 1u);
  return (SceneParticleClock){seed, t, fmaxf(0.0f, t - 0.14f)};
}

ArRenderColorF SceneParticleColor(ArRenderColorF hot,
                                         ArRenderColorF cool,
                                         SceneParticleClock clock) {
  ArRenderColorF color = MixColor(hot, cool, clock.t);
  const float birth_fade = fminf(1.0f, clock.t * 8.0f);
  const float retirement_fade = 1.0f - clock.t;
  color.a *= birth_fade * retirement_fade *
             (0.82f + 0.18f * HashUnit(clock.seed ^ 0xA7u));
  return color;
}

static bool AppendLavaPitParticles(ActionEffectGeometryWriter *writer,
                                   const ActionEffectInstance *effect,
                                   ActionEffectProjectPointFn project_point,
                                   void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {1.00f, 0.91f, 0.38f, 0.92f};
  const ArRenderColorF cool = {0.90f, 0.06f, 0.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float half_width = (rect->x1 - rect->x0) * 0.5f;
    const float birth =
        (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) * fmaxf(1.0f, half_width - 3.0f);
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 10.0f;
    x = birth + drift * t * t;
    /* The captured rectangle covers the full bubbly volume. Its geometric
     * centre still reads too low in the isometric mouth: the apparent
     * surface sits one quarter-height above it. Keep births in a narrow
     * band around that authored plane; the fraction scales correctly for
     * both one- and two-row pits. */
    const float source_surface_y =
        (rect->y0 + rect->y1) * 0.5f - (rect->y1 - rect->y0) * 0.25f;
    const float source_y =
        source_surface_y + (HashUnit(seed ^ 0xB5u) - 0.5f) * 3.0f;
    y = source_y - 5.0f * t - 13.0f * t * t;
    previous_x = birth + drift * previous_t * previous_t;
    previous_y = source_y - 5.0f * previous_t - 13.0f * previous_t * previous_t;
    width = 0.50f + 0.42f * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendLavaPitLighting(ActionEffectGeometryWriter *writer,
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
  const float half_width = (rect->x1 - rect->x0) * 0.5f;
  const float half_height = (rect->y1 - rect->y0) * 0.5f;
  static const ActionEffectGlowStyle kSpill = {
      .ring_scale = {0.22f, 0.70f, 1.0f},
      .centre = {1.00f, 0.42f, 0.04f, 0.17f},
      .ring = {{1.00f, 0.28f, 0.02f, 0.13f},
               {0.80f, 0.08f, 0.00f, 0.055f},
               {0.45f, 0.01f, 0.00f, 0.00f}},
      .flare = 0.10f,
      .rise = 0.12f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  spill = kSpill;
  spill.radius_x = fmaxf(34.0f, half_width + 14.0f);
  spill.radius_y = fmaxf(23.0f, half_height + 13.0f);
  spill.seed = (unsigned)effect->generation;
  static const ActionEffectGlowStyle kBody = {
      .ring_scale = {0.16f, 0.78f, 1.0f},
      .centre = {1.00f, 0.98f, 0.66f, 0.78f},
      .ring = {{1.00f, 0.66f, 0.10f, 0.46f},
               {1.00f, 0.20f, 0.01f, 0.18f},
               {0.72f, 0.04f, 0.00f, 0.00f}},
      .flare = 0.19f,
      .rise = 0.24f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  body = kBody;
  body.radius_x = fmaxf(28.0f, half_width + 3.0f);
  body.radius_y = fmaxf(8.0f, half_height + 2.0f);
  body.seed = (unsigned)effect->pulse_generation;
  spill_y = -half_height * 0.30f;
  body_y = 0.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendLavaReservoirParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectLavaReservoirParticleCount;
  const ArRenderColorF hot = {1.00f, 0.94f, 0.48f, 0.94f};
  const ArRenderColorF cool = {0.94f, 0.08f, 0.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneReservoirLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float half_width = (rect->x1 - rect->x0) * 0.5f;
    const float birth_x =
        (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) * fmaxf(1.0f, half_width - 2.0f);
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 22.0f;
    const float source_y =
        rect->y0 + 1.5f + (HashUnit(seed ^ 0xB5u) - 0.5f) * 2.0f;
    x = birth_x + drift * t * t;
    y = source_y - 9.0f * t - 34.0f * t * t;
    previous_x = birth_x + drift * previous_t * previous_t;
    previous_y = source_y - 9.0f * previous_t - 34.0f * previous_t * previous_t;
    width = 0.42f + 0.48f * (1.0f - t);
    reach = 1.8f + 4.2f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendMoltenRockParticles(ActionEffectGeometryWriter *writer,
                                      const ActionEffectInstance *effect,
                                      ActionEffectProjectPointFn project_point,
                                      void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {1.00f, 0.92f, 0.48f, 0.86f};
  const ArRenderColorF cool = {0.94f, 0.12f, 0.00f, 0.00f};
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneActorHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    /* Close sparks tumble off a hot solid surface. They do not align into a
     * directional flame wake, which is what distinguishes molten rock from
     * the actual `$CF9E` lava fireballs. */
    const float *direction =
        kCircle32[(i * 11u + (seed >> 10)) & kActionEffectGlowSegmentMask];
    const float distance = 6.0f + 10.0f * t;
    const float old_distance = 6.0f + 10.0f * previous_t;
    x = direction[0] * distance + heading_x * 2.0f * t;
    y = direction[1] * distance + 10.0f * t * t;
    previous_x = direction[0] * old_distance + heading_x * 2.0f * previous_t;
    previous_y = direction[1] * old_distance + 10.0f * previous_t * previous_t;
    width = 0.44f + 0.34f * (1.0f - t);
    reach = 1.5f + 2.2f * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendMoltenRockLighting(ActionEffectGeometryWriter *writer,
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
  static const ActionEffectGlowStyle kSpill = {
      .radius_x = 25.0f,
      .radius_y = 23.0f,
      .ring_scale = {0.24f, 0.66f, 1.0f},
      .centre = {1.00f, 0.44f, 0.05f, 0.18f},
      .ring = {{1.00f, 0.28f, 0.02f, 0.13f},
               {0.78f, 0.08f, 0.00f, 0.05f},
               {0.42f, 0.01f, 0.00f, 0.00f}},
      .flare = 0.035f,
      .rise = 0.03f,
      .axis_x = 1.0f,
      .lift_y = 1.0f};
  spill = kSpill;
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 10.5f,
      .radius_y = 10.5f,
      .ring_scale = {0.18f, 0.62f, 1.0f},
      .centre = {1.00f, 1.00f, 0.70f, 0.86f},
      .ring = {{1.00f, 0.67f, 0.12f, 0.52f},
               {1.00f, 0.20f, 0.01f, 0.18f},
               {0.72f, 0.03f, 0.00f, 0.00f}},
      .flare = 0.025f,
      .rise = 0.02f,
      .axis_x = 1.0f,
      .lift_y = 1.0f};
  body = kBody;
  body.seed = (unsigned)effect->pulse_generation;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendWaterSplashParticles(ActionEffectGeometryWriter *writer,
                                       const ActionEffectInstance *effect,
                                       ActionEffectProjectPointFn project_point,
                                       void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {0.92f, 1.00f, 1.00f, 0.86f};
  const ArRenderColorF cool = {0.12f, 0.48f, 1.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float half_width = (rect->x1 - rect->x0) * 0.5f;
    const float birth_x =
        (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) * fmaxf(1.0f, half_width - 2.0f);
    const bool drip = (i & 1u) != 0;
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 5.0f;
    x = birth_x + drift * t;
    previous_x = birth_x + drift * previous_t;
    if (drip) {
      y = 2.0f + 6.0f * t + 25.0f * t * t;
      previous_y = 2.0f + 6.0f * previous_t + 25.0f * previous_t * previous_t;
    } else {
      y = -8.0f - 12.0f * t + 18.0f * t * t;
      previous_y = -8.0f - 12.0f * previous_t + 18.0f * previous_t * previous_t;
    }
    width = 0.38f + 0.30f * (1.0f - t);
    reach = 1.8f + 3.0f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWaterSplashLighting(ActionEffectGeometryWriter *writer,
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
  const float half_width = (rect->x1 - rect->x0) * 0.5f;
  static const ActionEffectGlowStyle kSpill = {
      .radius_y = 19.0f,
      .ring_scale = {0.22f, 0.70f, 1.0f},
      .centre = {0.52f, 0.90f, 1.00f, 0.12f},
      .ring = {{0.28f, 0.70f, 1.00f, 0.08f},
               {0.08f, 0.32f, 0.78f, 0.03f},
               {0.02f, 0.10f, 0.38f, 0.00f}},
      .flare = 0.025f,
      .rise = 0.04f,
      .axis_x = 1.0f,
      .lift_y = 1.0f};
  spill = kSpill;
  spill.radius_x = fmaxf(22.0f, half_width + 10.0f);
  spill.seed = (unsigned)effect->generation;
  static const ActionEffectGlowStyle kBody = {
      .radius_y = 6.0f,
      .ring_scale = {0.18f, 0.72f, 1.0f},
      .centre = {0.94f, 1.00f, 1.00f, 0.50f},
      .ring = {{0.54f, 0.92f, 1.00f, 0.28f},
               {0.16f, 0.58f, 1.00f, 0.08f},
               {0.03f, 0.18f, 0.54f, 0.00f}},
      .flare = 0.02f,
      .axis_x = 1.0f,
      .lift_y = 1.0f};
  body = kBody;
  body.radius_x = fmaxf(14.0f, half_width + 2.0f);
  body.seed = (unsigned)effect->pulse_generation;
  spill_y = 4.0f;
  body_y = -4.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendWaterfallParticles(ActionEffectGeometryWriter *writer,
                                     const ActionEffectInstance *effect,
                                     ActionEffectProjectPointFn project_point,
                                     void *userdata) {
  const unsigned count = kActionSceneEffectWaterfallParticleCount;
  const ArRenderColorF hot = {0.82f, 0.98f, 1.00f, 0.46f};
  const ArRenderColorF cool = {0.08f, 0.42f, 0.82f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    /* Stable lanes, staggered by identity, provide a slow translucent flow
     * over the fast two-frame source cycle. The varying alpha and length
     * break horizontal bands without blurring away the pixel art. */
    const unsigned columns = 16u;
    _Static_assert(kActionSceneEffectWaterfallParticleCount % 16u == 0u,
                   "waterfall veil must fill complete lane rows");
    const unsigned column = i % columns;
    const unsigned row = i / columns;
    const float lane = ((float)column + 0.5f) / (float)columns;
    const float left = rect->x0 + 10.0f;
    const float width_span = rect->x1 - rect->x0 - 20.0f;
    const float y_span = rect->y1 - rect->y0 + 48.0f;
    const float phase = (HashUnit(seed ^ 0x29u) +
                         (float)visual_ticks / (84.0f + (float)(row * 11u)));
    const float wrapped = phase - floorf(phase);
    x = left + width_span * lane + (HashUnit(seed ^ 0x53u) - 0.5f) * 10.0f;
    y = rect->y0 - 24.0f + y_span * wrapped;
    previous_x = x + (HashUnit(seed ^ 0x37u) - 0.5f) * 1.5f;
    previous_y = y - (8.0f + 7.0f * HashUnit(seed ^ 0xB5u));
    width = 0.45f + 0.42f * HashUnit(seed ^ 0x71u);
    reach = 6.0f + 10.0f * HashUnit(seed ^ 0xA7u);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWaterfallLighting(ActionEffectGeometryWriter *writer,
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
  /* Two broad, low-alpha meshes act as a soft water veil. The
   * underlying tiles remain the image; this only lowers the perceived
   * contrast of their short animation cycle and supplies cool depth. The
   * first live waterfall acceptance showed the original 0.01-alpha veil
   * disappeared under CRT scaling, so these remain restrained but no
   * longer sub-perceptual. */
  static const ActionEffectGlowStyle kSpill = {
      .radius_x = 330.0f,
      .radius_y = 282.0f,
      .ring_scale = {0.12f, 0.88f, 1.0f},
      .centre = {0.30f, 0.70f, 1.00f, 0.042f},
      .ring = {{0.22f, 0.62f, 1.00f, 0.034f},
               {0.10f, 0.42f, 0.78f, 0.014f},
               {0.03f, 0.14f, 0.30f, 0.00f}},
      .flare = 0.008f,
      .axis_x = 1.0f,
      .lift_y = 1.0f};
  spill = kSpill;
  spill.seed = (unsigned)effect->generation;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 282.0f,
      .radius_y = 230.0f,
      .ring_scale = {0.12f, 0.92f, 1.0f},
      .centre = {0.56f, 0.88f, 1.00f, 0.052f},
      .ring = {{0.38f, 0.78f, 1.00f, 0.040f},
               {0.16f, 0.52f, 0.92f, 0.017f},
               {0.04f, 0.18f, 0.42f, 0.00f}},
      .flare = 0.006f,
      .axis_x = 1.0f,
      .lift_y = 1.0f};
  body = kBody;
  body.seed = (unsigned)effect->pulse_generation;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendWaterfallMistParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectWaterfallMistParticleCount;
  const ArRenderColorF hot = {0.96f, 1.00f, 1.00f, 0.54f};
  const ArRenderColorF cool = {0.34f, 0.70f, 0.92f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    /* Foam boils along the lower waterfall edge while lighter droplets
     * drift upward into the fog banks. Horizontal phase offsets avoid a
     * static bright seam over the gap. */
    const float span = rect->x1 - rect->x0;
    const float lane = ((float)i + HashUnit(seed ^ 0x53u)) / (float)count;
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 18.0f;
    const float base_y = 1.0f + (HashUnit(seed ^ 0xB5u) - 0.5f) * 12.0f;
    x = rect->x0 + span * lane + drift * t;
    y = base_y - 5.0f * t - 17.0f * t * t;
    previous_x = rect->x0 + span * lane + drift * previous_t;
    previous_y = base_y - 5.0f * previous_t - 17.0f * previous_t * previous_t;
    width = 0.62f + 0.68f * (1.0f - t);
    reach = 2.4f + 4.4f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWallTorchParticles(ActionEffectGeometryWriter *writer,
                                     const ActionEffectInstance *effect,
                                     ActionEffectProjectPointFn project_point,
                                     void *userdata) {
  const unsigned count = 7;
  const ArRenderColorF hot = {1.00f, 0.92f, 0.55f, 0.88f};
  const ArRenderColorF cool = {0.95f, 0.18f, 0.01f, 0.00f};
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 7.0f;
    const float birth = (HashUnit(seed ^ 0x71u) - 0.5f) * 5.0f;
    x = birth + drift * t * t;
    y = -2.0f - 9.0f * t - 25.0f * t * t;
    previous_x = birth + drift * previous_t * previous_t;
    previous_y = -2.0f - 9.0f * previous_t - 25.0f * previous_t * previous_t;
    width = 0.45f + 0.35f * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWallTorchLighting(ActionEffectGeometryWriter *writer,
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
  static const ActionEffectGlowStyle kSpill = {
      .radius_x = 30.0f,
      .radius_y = 25.0f,
      .ring_scale = {0.25f, 0.62f, 1.0f},
      .centre = {1.00f, 0.48f, 0.10f, 0.14f},
      .ring = {{1.00f, 0.34f, 0.05f, 0.10f},
               {0.84f, 0.12f, 0.01f, 0.045f},
               {0.55f, 0.03f, 0.00f, 0.00f}},
      .flare = 0.12f,
      .rise = 0.16f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  spill = kSpill;
  spill.seed = (unsigned)effect->generation;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 8.5f,
      .radius_y = 14.0f,
      .ring_scale = {0.20f, 0.52f, 1.0f},
      .centre = {1.00f, 0.98f, 0.82f, 0.72f},
      .ring = {{1.00f, 0.72f, 0.22f, 0.40f},
               {1.00f, 0.27f, 0.02f, 0.16f},
               {0.78f, 0.07f, 0.00f, 0.00f}},
      .flare = 0.34f,
      .rise = 0.42f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  body = kBody;
  body.seed = (unsigned)effect->pulse_generation;
  body_y = -3.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendStatueFireParticles(ActionEffectGeometryWriter *writer,
                                      const ActionEffectInstance *effect,
                                      ActionEffectProjectPointFn project_point,
                                      void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {1.00f, 0.96f, 0.62f, 0.96f};
  const ArRenderColorF cool = {0.98f, 0.08f, 0.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    /* Seed throughout the authored horizontal plume, then let hot flecks
     * continue a little in the facing direction while buoyancy pulls them
     * upward. This reads as a sustained breath rather than a projectile
     * wake, and scales naturally across the $1C-$1F pillar frames. */
    const float direction =
        (effect->flags & kActionEffectFlag_FlipHorizontal) ? -1.0f : 1.0f;
    const float span = fmaxf(8.0f, rect->x1 - rect->x0);
    const float lane = HashUnit(seed ^ 0x71u);
    const float birth_x = rect->x0 + span * lane;
    const float source_y =
        (rect->y0 + rect->y1) * 0.5f + (HashUnit(seed ^ 0xB5u) - 0.5f) * 10.0f;
    const float forward = direction * (3.0f + 10.0f * HashUnit(seed ^ 0x53u));
    const float curl = (HashUnit(seed ^ 0x37u) - 0.5f) * 7.0f;
    x = birth_x + forward * t + curl * t * t;
    y = source_y - 5.0f * t - 17.0f * t * t;
    previous_x =
        birth_x + forward * previous_t + curl * previous_t * previous_t;
    previous_y = source_y - 5.0f * previous_t - 17.0f * previous_t * previous_t;
    width = 0.52f + 0.48f * (1.0f - t);
    reach = 2.0f + 4.0f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendStatueFireLighting(ActionEffectGeometryWriter *writer,
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
  const float half_width = (rect->x1 - rect->x0) * 0.5f;
  const float half_height = (rect->y1 - rect->y0) * 0.5f;
  static const ActionEffectGlowStyle kSpill = {
      .ring_scale = {0.24f, 0.66f, 1.0f},
      .centre = {1.00f, 0.48f, 0.07f, 0.23f},
      .ring = {{1.00f, 0.31f, 0.02f, 0.16f},
               {0.86f, 0.08f, 0.00f, 0.06f},
               {0.48f, 0.01f, 0.00f, 0.00f}},
      .flare = 0.15f,
      .rise = 0.18f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  spill = kSpill;
  spill.radius_x = fmaxf(28.0f, half_width + 18.0f);
  spill.radius_y = fmaxf(21.0f, half_height + 13.0f);
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .ring_scale = {0.18f, 0.56f, 1.0f},
      .centre = {1.00f, 1.00f, 0.76f, 0.84f},
      .ring = {{1.00f, 0.68f, 0.13f, 0.50f},
               {1.00f, 0.22f, 0.01f, 0.20f},
               {0.72f, 0.03f, 0.00f, 0.00f}},
      .flare = 0.28f,
      .rise = 0.30f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  body = kBody;
  body.radius_x = fmaxf(11.0f, half_width + 4.0f);
  body.radius_y = fmaxf(8.0f, half_height + 2.0f);
  body.seed = (unsigned)effect->pulse_generation;
  body_y -= 2.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendFireballParticles(ActionEffectGeometryWriter *writer,
                                    const ActionEffectInstance *effect,
                                    ActionEffectProjectPointFn project_point,
                                    void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {1.00f, 0.97f, 0.78f, 0.96f};
  const ArRenderColorF cool = {1.00f, 0.10f, 0.00f, 0.00f};
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneFireballHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float side = (HashUnit(seed ^ 0x53u) - 0.5f) * (4.0f + 14.0f * t);
    /* Start beyond the 16px source art instead of hiding the youngest
     * sparks inside its painted red tail. The longer cone makes the host
     * enhancement legible without bleaching the authentic projectile. */
    const float distance = 10.0f + 44.0f * t;
    const float previous_distance = 10.0f + 44.0f * previous_t;
    x = -heading_x * distance - heading_y * side;
    y = -heading_y * distance + heading_x * side;
    previous_x = -heading_x * previous_distance - heading_y * side;
    previous_y = -heading_y * previous_distance + heading_x * side;
    width = 0.80f + 0.60f * (1.0f - t);
    reach = 3.0f + 5.0f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendFireballLighting(ActionEffectGeometryWriter *writer,
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
  SceneFireballHeading(effect, &hx, &hy);
  static const ActionEffectGlowStyle kSpill = {
      .radius_x = 38.0f,
      .radius_y = 27.0f,
      .ring_scale = {0.28f, 0.65f, 1.0f},
      .centre = {1.00f, 0.52f, 0.09f, 0.24f},
      .ring = {{1.00f, 0.35f, 0.03f, 0.17f},
               {0.92f, 0.11f, 0.00f, 0.075f},
               {0.55f, 0.02f, 0.00f, 0.00f}},
      .flare = 0.13f,
      .rise = 0.18f};
  spill = kSpill;
  spill.axis_x = hx;
  spill.axis_y = hy;
  spill.lift_x = -hx;
  spill.lift_y = -hy;
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 25.0f,
      .radius_y = 11.0f,
      .ring_scale = {0.20f, 0.50f, 1.0f},
      .centre = {1.00f, 0.99f, 0.88f, 0.90f},
      .ring = {{1.00f, 0.78f, 0.28f, 0.60f},
               {1.00f, 0.30f, 0.02f, 0.30f},
               {0.82f, 0.07f, 0.00f, 0.00f}},
      .flare = 0.27f,
      .rise = 0.34f};
  body = kBody;
  body.axis_x = hx;
  body.axis_y = hy;
  body.lift_x = -hx;
  body.lift_y = -hy;
  body.seed = (unsigned)effect->pulse_generation;
  /* Pull the enhanced body into the wake so its brightest region remains
   * visible beside the painted core instead of disappearing underneath. */
  body_x = mid_x - hx * 6.0f;
  body_y = mid_y - hy * 6.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendFlamingWheelParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = FlamingWheelHasAuthoredFireballRing(effect)
                             ? kActionSceneEffectParticlesPerInstance
                             : 0;
  const ArRenderColorF hot = {1.00f, 0.96f, 0.58f, 0.96f};
  const ArRenderColorF cool = {0.96f, 0.08f, 0.00f, 0.00f};
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float *anchor =
        kFlamingWheelFireballAnchors[i %
                                     kActionSceneEffectFlamingWheelFireballs];
    const float birth_x = anchor[0] + (HashUnit(seed ^ 0x53u) - 0.5f) * 3.0f;
    const float base_y =
        anchor[1] - 1.0f + (HashUnit(seed ^ 0x71u) - 0.5f) * 2.0f;
    const float sway = (HashUnit(seed ^ 0x37u) - 0.5f) * 10.0f;
    x = birth_x + sway * t * t;
    y = base_y - 8.0f * t - 24.0f * t * t;
    previous_x = birth_x + sway * previous_t * previous_t;
    previous_y = base_y - 8.0f * previous_t - 24.0f * previous_t * previous_t;
    width = 0.65f + 0.50f * (1.0f - t);
    reach = 2.5f + 4.0f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendFlamingWheelLighting(ActionEffectGeometryWriter *writer,
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
  const float half_width = (rect->x1 - rect->x0) * 0.5f;
  const float half_height = (rect->y1 - rect->y0) * 0.5f;
  static const ActionEffectGlowStyle kSpill = {
      .ring_scale = {0.24f, 0.68f, 1.0f},
      .centre = {1.00f, 0.48f, 0.05f, 0.25f},
      .ring = {{1.00f, 0.29f, 0.02f, 0.17f},
               {0.86f, 0.08f, 0.00f, 0.065f},
               {0.48f, 0.01f, 0.00f, 0.00f}},
      .flare = 0.20f,
      .rise = 0.24f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  spill = kSpill;
  spill.radius_x = fmaxf(42.0f, half_width + 18.0f);
  spill.radius_y = fmaxf(38.0f, half_height + 18.0f);
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .ring_scale = {0.18f, 0.60f, 1.0f},
      .centre = {1.00f, 1.00f, 0.72f, 0.82f},
      .ring = {{1.00f, 0.67f, 0.12f, 0.52f},
               {1.00f, 0.19f, 0.01f, 0.20f},
               {0.70f, 0.03f, 0.00f, 0.00f}},
      .flare = 0.31f,
      .rise = 0.34f,
      .axis_x = 1.0f,
      .lift_y = -1.0f};
  body = kBody;
  body.radius_x = fmaxf(22.0f, half_width + 4.0f);
  body.radius_y = fmaxf(22.0f, half_height + 4.0f);
  body.seed = (unsigned)effect->pulse_generation;
  body_y -= 3.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return AppendFlamingWheelFireballLighting(writer, effect, pulse,
                                            project_point, userdata);
}

static bool AppendFlamingWheelProjectileParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  const ArRenderColorF hot = {0.88f, 1.00f, 1.00f, 0.96f};
  const ArRenderColorF cool = {0.02f, 0.58f, 0.86f, 0.00f};
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneActorHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float side = (HashUnit(seed ^ 0x53u) - 0.5f) * (4.0f + 13.0f * t);
    const float distance = 5.0f + 36.0f * t;
    const float old_distance = 5.0f + 36.0f * previous_t;
    x = -heading_x * distance - heading_y * side;
    y = -heading_y * distance + heading_x * side;
    previous_x = -heading_x * old_distance - heading_y * side;
    previous_y = -heading_y * old_distance + heading_x * side;
    width = 0.48f + 0.46f * (1.0f - t);
    reach = 2.0f + 3.8f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendFlamingWheelProjectileLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
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
      .radius_x = 29.0f,
      .radius_y = 18.0f,
      .ring_scale = {0.22f, 0.66f, 1.0f},
      .centre = {0.62f, 0.95f, 1.00f, 0.22f},
      .ring = {{0.22f, 0.76f, 1.00f, 0.14f},
               {0.03f, 0.36f, 0.82f, 0.05f},
               {0.01f, 0.12f, 0.36f, 0.00f}},
      .flare = 0.12f,
      .rise = 0.08f};
  spill = kSpill;
  spill.axis_x = hx;
  spill.axis_y = hy;
  spill.lift_x = -hx;
  spill.lift_y = -hy;
  spill.seed = (unsigned)effect->record_address;
  static const ActionEffectGlowStyle kBody = {
      .radius_x = 15.0f,
      .radius_y = 8.0f,
      .ring_scale = {0.16f, 0.58f, 1.0f},
      .centre = {0.94f, 1.00f, 1.00f, 0.88f},
      .ring = {{0.52f, 0.94f, 1.00f, 0.52f},
               {0.06f, 0.62f, 0.94f, 0.18f},
               {0.01f, 0.18f, 0.48f, 0.00f}},
      .flare = 0.18f,
      .rise = 0.12f};
  body = kBody;
  body.axis_x = hx;
  body.axis_y = hy;
  body.lift_x = -hx;
  body.lift_y = -hy;
  body.seed = (unsigned)effect->pulse_generation;
  spill_x = mid_x - hx * 5.0f;
  spill_y = mid_y - hy * 5.0f;
  body_x = mid_x - hx * 2.0f;
  body_y = mid_y - hy * 2.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

static bool AppendActorProjectileParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const unsigned count = kActionSceneEffectParticlesPerInstance;
  ArRenderColorF hot, cool;
  switch (effect->kind) {
    case kActionEffect_MinotaurAxe:
      hot = (ArRenderColorF){1.00f, 1.00f, 0.90f, 0.92f};
      cool = (ArRenderColorF){1.00f, 0.48f, 0.06f, 0.00f};
      break;
    case kActionEffect_IceDragonIceBall:
      hot = (ArRenderColorF){0.96f, 1.00f, 1.00f, 0.94f};
      cool = (ArRenderColorF){0.10f, 0.55f, 1.00f, 0.00f};
      break;
    case kActionEffect_TanzaraProjectile:
      hot = (ArRenderColorF){1.00f, 0.94f, 1.00f, 0.94f};
      cool = (ArRenderColorF){0.62f, 0.08f, 1.00f, 0.00f};
      break;
    default:
      return true;
  }
  const unsigned visual_ticks =
      EffectVisualTicks(effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneActorHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, kSceneEmberLifetime);
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float spread =
        effect->kind == kActionEffect_IceDragonIceBall ? 16.0f : 11.0f;
    const float length =
        effect->kind == kActionEffect_MinotaurAxe ? 32.0f : 42.0f;
    const float side = (HashUnit(seed ^ 0x53u) - 0.5f) * (4.0f + spread * t);
    const float distance = 5.0f + length * t;
    const float old_distance = 5.0f + length * previous_t;
    x = -heading_x * distance - heading_y * side;
    y = -heading_y * distance + heading_x * side;
    previous_x = -heading_x * old_distance - heading_y * side;
    previous_y = -heading_y * old_distance + heading_x * side;
    if (effect->kind == kActionEffect_IceDragonIceBall) {
      y += 5.0f * t * t;
      previous_y += 5.0f * previous_t * previous_t;
    }
    width = 0.50f + 0.48f * (1.0f - t);
    reach = 2.0f + 4.0f * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendActorProjectileLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  float hx = 1.0f, hy = 0.0f;
  SceneActorHeading(effect, &hx, &hy);
  const bool ice = effect->kind == kActionEffect_IceDragonIceBall;
  const bool axe = effect->kind == kActionEffect_MinotaurAxe;
  const ArRenderColorF spill_centre =
      ice ? (ArRenderColorF){0.42f, 0.84f, 1.00f, 0.22f}
          : (axe ? (ArRenderColorF){1.00f, 0.70f, 0.22f, 0.19f}
                 : (ArRenderColorF){0.76f, 0.28f, 1.00f, 0.22f});
  const ArRenderColorF spill_mid =
      ice ? (ArRenderColorF){0.18f, 0.55f, 1.00f, 0.13f}
          : (axe ? (ArRenderColorF){1.00f, 0.43f, 0.06f, 0.12f}
                 : (ArRenderColorF){0.54f, 0.12f, 1.00f, 0.14f});
  const ArRenderColorF body_mid =
      ice ? (ArRenderColorF){0.56f, 0.91f, 1.00f, 0.52f}
          : (axe ? (ArRenderColorF){1.00f, 0.78f, 0.31f, 0.48f}
                 : (ArRenderColorF){0.88f, 0.54f, 1.00f, 0.50f});
  spill = (ActionEffectGlowStyle){
      .radius_x = ice ? 35.0f : 31.0f,
      .radius_y = ice ? 22.0f : 20.0f,
      .ring_scale = {0.24f, 0.68f, 1.0f},
      .centre = spill_centre,
      .ring = {spill_mid,
               ice ? (ArRenderColorF){0.06f, 0.22f, 0.76f, 0.045f}
                   : (axe ? (ArRenderColorF){0.76f, 0.15f, 0.01f, 0.04f}
                          : (ArRenderColorF){0.28f, 0.04f, 0.70f, 0.05f}),
               {0.03f, 0.02f, 0.25f, 0.00f}},
      .flare = ice ? 0.07f : 0.12f,
      .rise = 0.06f,
      .axis_x = hx,
      .axis_y = hy,
      .lift_x = -hx,
      .lift_y = -hy,
      .seed = (unsigned)effect->record_address,
  };
  body = (ActionEffectGlowStyle){
      .radius_x = ice ? 19.0f : 17.0f,
      .radius_y = ice ? 9.0f : 10.0f,
      .ring_scale = {0.17f, 0.60f, 1.0f},
      .centre = {1.00f, 1.00f, 1.00f, ice ? 0.86f : 0.78f},
      .ring = {body_mid,
               ice ? (ArRenderColorF){0.15f, 0.55f, 1.00f, 0.17f}
                   : (axe ? (ArRenderColorF){1.00f, 0.30f, 0.03f, 0.15f}
                          : (ArRenderColorF){0.58f, 0.12f, 1.00f, 0.18f}),
               {0.05f, 0.02f, 0.30f, 0.00f}},
      .flare = ice ? 0.08f : 0.16f,
      .rise = 0.09f,
      .axis_x = hx,
      .axis_y = hy,
      .lift_x = -hx,
      .lift_y = -hy,
      .seed = (unsigned)effect->pulse_generation,
  };
  spill_x = mid_x - hx * 5.0f;
  spill_y = mid_y - hy * 5.0f;
  body_x = mid_x - hx * 2.0f;
  body_y = mid_y - hy * 2.0f;
  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata))
    return false;
  return true;
}

/* The Northwall boss embeds the charge in its body compositions. Follow
 * both palm orbs through $0A-$0D and their merged $0E release, rather than
 * illuminating the whole body or inventing a fixed facing-relative origin. */
static unsigned NorthwallMagicAnchors(const ActionEffectInstance *effect,
                                      ArRenderPointF anchors[2]) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  if (effect->phase != kActionEffectPhase_NorthwallMagicCharge) {
    anchors[0] = (ArRenderPointF){(rect->x0 + rect->x1) * .5f, (rect->y0 + rect->y1) * .5f};
    return 1;
  }
  static const ArRenderPointF kHands[5][2] = {
      {{-32, -9}, {16, -9}}, {{-36, -9}, {19, -9}}, {{-36, -12}, {19, -12}},
      {{-16, 7}, {8, 7}},    {{-8, 15}, {-8, 15}},
  };
  if (effect->visual < 0x0A || effect->visual > 0x0E) return 0;
  const unsigned count = effect->visual == 0x0E ? 1 : 2;
  for (unsigned i = 0; i < count; ++i) {
    anchors[i] = kHands[effect->visual - 0x0A][i];
    if (effect->flags & kActionEffectFlag_FlipHorizontal) anchors[i].x = -anchors[i].x;
  }
  return count;
}

/* The native impact child is the water contact, independent of the falling
 * projectile. Its bottom stays on the surface as the magic art grows/fades.
 * Emit once: even the shortest regional impact lasts through this burst. */
static bool AppendNorthwallWaterSplashParticles(ActionEffectGeometryWriter *writer,
                                               const ActionEffectInstance *effect,
                                               ActionEffectProjectPointFn project_point,
                                               void *userdata) {
  const unsigned ticks = effect->phase_ticks;
  const float surface_y = effect->geometry.data.rect.y1;
  for (unsigned i = 0; i < kActionSceneEffectParticlesPerInstance; ++i) {
    const uint32_t seed = DeterministicHash_Mix32(
        effect->generation * 0x9E3779B9u ^ (uint32_t)effect->record_address * 0x85EBCA6Bu ^
        i * 0xC2B2AE35u);
    const bool foam = i >= 8;
    const unsigned delay = (seed >> 4) % 3;
    const unsigned lifetime = (foam ? 10 : 15) + ((seed >> 8) & 3);
    if (ticks < delay || ticks >= delay + lifetime) continue;
    const float t = (float)(ticks - delay) / (float)(lifetime - 1);
    const float old_t = fmaxf(0, t - 1.0f / (float)(lifetime - 1));
    const float side = (i & 1) ? 1 : -1;
    const float distance = 16 + 24 * HashUnit(seed ^ 0x37u);
    const float height = foam ? 2 + 2 * HashUnit(seed ^ 0x71u)
                              : 14 + 15 * HashUnit(seed ^ 0x71u);
    const float x = side * (1.5f + distance * t);
    const float y = surface_y - 4 * height * t * (1 - t);
    const float old_x = side * (1.5f + distance * old_t);
    const float old_y = surface_y - 4 * height * old_t * (1 - old_t);
    ArRenderColorF color = MixColor((ArRenderColorF){.92f, 1, 1, 1},
                                    (ArRenderColorF){.18f, .55f, .9f, 1}, t);
    color.a = .9f * (1 - t * t);
    if (!AppendSceneParticle(writer, effect, x, y, old_x, old_y, .55f + .25f * (1 - t),
                             foam ? 2.4f : 1.6f, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendNorthwallMagicParticles(ActionEffectGeometryWriter *writer,
                                          const ActionEffectInstance *effect,
                                          ActionEffectProjectPointFn project_point,
                                          void *userdata) {
  if (effect->phase == kActionEffectPhase_NorthwallMagicImpact)
    return AppendNorthwallWaterSplashParticles(writer, effect, project_point, userdata);
  ArRenderPointF anchors[2];
  const unsigned count = NorthwallMagicAnchors(effect, anchors);
  if (!count) return true;
  const unsigned ticks = EffectVisualTicks(effect, effect->pulse_ticks);
  const bool falling = effect->phase == kActionEffectPhase_NorthwallMagicFall;
  for (unsigned i = 0; i < kActionSceneEffectParticlesPerInstance; ++i) {
    const SceneParticleClock clock = SceneParticleClockAt(effect, ticks, i, kSceneEmberLifetime);
    const float *direction = kCircle32[(i * 11 + (clock.seed >> 8)) & kActionEffectGlowSegmentMask];
    const ArRenderPointF anchor = anchors[i % count];
    const float travel = falling ? -30 : 0;
    const float x = anchor.x + direction[0] * (2 + 14 * clock.t);
    const float y = anchor.y + direction[1] * 8 * clock.t + travel * clock.t;
    const float old_x = anchor.x + direction[0] * (2 + 14 * clock.previous_t);
    const float old_y = anchor.y + direction[1] * 8 * clock.previous_t + travel * clock.previous_t;
    const ArRenderColorF color = SceneParticleColor((ArRenderColorF){.9f, 1, 1, .85f},
                                                    (ArRenderColorF){.1f, .45f, 1, 0}, clock);
    if (!AppendSceneParticle(writer, effect, x, y, old_x, old_y, .65f, 2.5f, color, project_point,
                             userdata))
      return false;
  }
  return true;
}

static bool AppendNorthwallMagicLighting(ActionEffectGeometryWriter *writer,
                                         const ActionEffectInstance *effect,
                                         ActionEffectProjectPointFn project_point, void *userdata) {
  ArRenderPointF anchors[2];
  const unsigned count = NorthwallMagicAnchors(effect, anchors);
  const bool charge = effect->phase == kActionEffectPhase_NorthwallMagicCharge;
  const bool impact = effect->phase == kActionEffectPhase_NorthwallMagicImpact;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  for (unsigned i = 0; i < count; ++i) {
    ActionEffectGlowStyle glow = {
        .radius_x = charge ? 20 : (impact ? fmaxf(32, (rect->x1 - rect->x0) * .7f + 8) : 24),
        .radius_y = charge ? 20 : (impact ? (rect->y1 - rect->y0) * .7f + 8 : 34),
        .ring_scale = {.2f, .65f, 1},
        .centre = {.8f, .95f, 1, .55f},
        .ring = {{.4f, .8f, 1, .28f}, {.1f, .35f, 1, .10f}, {.08f, .1f, .8f, 0}},
        .axis_x = 1,
        .lift_y = -1,
        .flare = .12f,
        .seed = effect->pulse_generation + i,
    };
    if (!AppendGlow(writer, effect, &glow, DeterministicPulse(effect), anchors[i].x, anchors[i].y,
                    project_point, userdata))
      return false;
  }
  return true;
}

enum { kForestVisibleRays = 6, kForestRayRows = 6, kForestRayColumns = 5,
       kForestMotes = 22, kForestLeaves = 16,
       kForestDustClusters = 4, kForestDustPerCluster = 8,
       kForestDustMotes = kForestVisibleRays * kForestDustClusters * kForestDustPerCluster };
enum {
  kForestRayTriangles = kForestVisibleRays * (kForestRayRows - 1) *
      (kForestRayColumns - 1) * 2,
  kForestClippedVertices = kForestRayTriangles * 7,
  kForestClippedIndices = kForestRayTriangles * 15,
};
_Static_assert(kForestClippedVertices + (kForestMotes + kForestDustMotes) * 4 <=
                   kActionSceneEffectRenderMaxVertices &&
               kForestClippedIndices + (kForestMotes + kForestDustMotes) * 6 <=
                   kActionSceneEffectRenderMaxIndices &&
               kForestLeaves * 9 <=
                   kActionSceneEffectGlowsPerInstance * kActionEffectGlowVertices,
               "clipped forest rays must fit the existing scene scratch batch");

typedef struct ForestCanopyOpening {
  float x; /* World X where the ray crosses Y=0, on the independent light layer. */
  float half_width;
  float strength;
  float shoulder; /* Relative brightness halfway from the core to either edge. */
  unsigned fan; /* Zero is an isolated shaft; other values share an offscreen origin. */
} ForestCanopyOpening;

static const ArRenderPointF kForestFanOrigins[] = {
  {0, 0}, {928, -1024}, {2400, -2048}, {3700, -2048}, {4700, -2560},
};
enum { kForestBossFan = 4 };

/* Authored across the full parallax layer, not relative to the camera. The
 * small groups alternate fine cracks and broad, softer canopy openings, with
 * long shaded gaps between them. Any seven sources span at least 1655 pixels,
 * exceeding the capture field's maximum 1476px reach over room heights,
 * including fan slopes (.50-.68), widest edges and sway. At most six can
 * intersect the 768x544 field; no extra geometry allocation is needed. */
static const ForestCanopyOpening kForestOpenings[] = {
  {240, 56, 1, .62f, 1},       {365, 12, .32f, .28f, 1},
  {1030, 96, .60f, .70f, 2},   {1245, 24, .96f, .36f, 2},
  {1610, 18, .40f, .32f, 0},   {1735, 62, .88f, .58f, 0},
  {2380, 112, .46f, .74f, 3},  {2630, 26, 1, .40f, 3},
  /* A soft approach opens onto three stronger shafts across the Centaur's
   * arena. Their gaps remain dark enough to read movement through the light. */
  {2910, 84, .56f, .72f, 0},
  {3110, 62, .96f, .55f, kForestBossFan},
  {3265, 76, 1, .68f, kForestBossFan},
  {3420, 54, .88f, .42f, kForestBossFan},
};

static float ForestSoftRamp(float value) {
  const float t = fmaxf(0, fminf(1, value));
  return t * t * (3 - 2 * t);
}

static float ForestRaySway(const ActionEffectInstance *effect, unsigned opening) {
  const unsigned fan = kForestOpenings[opening].fan;
  const unsigned phase = fan ? 16 + fan : opening;
  return 4 * sinf((effect->phase_ticks & 2047u) * (6.2831853f / 2048) + phase * .8f);
}

static float ForestRaySlope(const ForestCanopyOpening *opening) {
  if (!opening->fan) return .60f;
  const ArRenderPointF origin = kForestFanOrigins[opening->fan];
  return (origin.x - opening->x) / -origin.y;
}

enum { kForestOpeningCount = sizeof(kForestOpenings) / sizeof(kForestOpenings[0]) };
typedef struct ForestRayState {
  float slope, sway;
} ForestRayState;

static ForestRayState ForestRayAt(const ActionEffectInstance *effect, unsigned i) {
  return (ForestRayState){ForestRaySlope(&kForestOpenings[i]), ForestRaySway(effect, i)};
}

static void ForestRayStates(const ActionEffectInstance *effect, ForestRayState *rays) {
  /* A small per-build snapshot avoids repeating trig for every leaf/mote.
   * No persistent cache: retained frames and scene-clock wraps stay deterministic. */
  for (unsigned i = 0; i < kForestOpeningCount; i++) rays[i] = ForestRayAt(effect, i);
}

static float ForestRayWidth(const ForestCanopyOpening *opening, float world_y) {
  if (opening->fan) {
    const float source_y = kForestFanOrigins[opening->fan].y;
    /* Both edges and the centre converge at the same fixed source. Shared
     * sway moves the complete fan without separating its rays at the origin. */
    return opening->half_width * .8f * fmaxf(0, (world_y - source_y) / -source_y);
  }
  return opening->half_width * (.8f + .0005f * fmaxf(0, fminf(768, world_y)));
}

static float ForestLightAmount(
    const ActionEffectInstance *effect, const ForestRayState *rays, float x, float y) {
  const float world_x = effect->world_x + x;
  const float world_y = effect->world_y + y;
  float light = 0;
  for (unsigned i = 0; i < kForestOpeningCount; i++) {
    const ForestCanopyOpening *opening = &kForestOpenings[i];
    const float centre = opening->x - rays[i].slope * world_y + rays[i].sway;
    const float amount = opening->strength * ForestSoftRamp(
        1 - fabsf(world_x - centre) / ForestRayWidth(opening, world_y));
    light = fmaxf(light, amount);
  }
  return light;
}

static bool ForestRayIntersectsField(
    const ActionEffectInstance *effect, unsigned index, ForestRayState ray) {
  const ForestCanopyOpening *opening = &kForestOpenings[index];
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float top = effect->world_y + rect->y0, bottom = effect->world_y + rect->y1;
  const float left = opening->x - ray.slope * bottom + ray.sway -
      ForestRayWidth(opening, bottom) - effect->world_x;
  const float right = opening->x - ray.slope * top + ray.sway +
      ForestRayWidth(opening, top) - effect->world_x;
  return right > rect->x0 && left < rect->x1;
}

typedef struct ForestClipVertex {
  ArRenderVertex2D vertex; /* Position is still effect-local, before projection. */
  int source_index;
} ForestClipVertex;

static unsigned ForestClipCode(ArRenderPointF point, const ActionEffectLocalRect *clip) {
  return (point.x < clip->x0 ? 1u : 0) | (point.x > clip->x1 ? 2u : 0) |
      (point.y < clip->y0 ? 4u : 0) | (point.y > clip->y1 ? 8u : 0);
}

static float ForestClipDistance(
    ArRenderPointF point, const ActionEffectLocalRect *clip, int edge) {
  switch (edge) {
    case 0: return point.x - clip->x0;
    case 1: return clip->x1 - point.x;
    case 2: return point.y - clip->y0;
    default: return clip->y1 - point.y;
  }
}

static ForestClipVertex ForestClipIntersection(
    ForestClipVertex a, ForestClipVertex b, const ActionEffectLocalRect *clip, int edge) {
  /* Use the same endpoint order on shared edges, preventing cracks from
   * opposite-direction floating-point interpolation in adjacent triangles. */
  if (a.vertex.position.x > b.vertex.position.x ||
      (a.vertex.position.x == b.vertex.position.x && a.vertex.position.y > b.vertex.position.y)) {
    const ForestClipVertex swap = a;
    a = b;
    b = swap;
  }
  const float da = ForestClipDistance(a.vertex.position, clip, edge);
  const float db = ForestClipDistance(b.vertex.position, clip, edge);
  const float t = da / (da - db);
  ForestClipVertex result = {
    .vertex = {
      .position = {a.vertex.position.x + (b.vertex.position.x - a.vertex.position.x) * t,
                   a.vertex.position.y + (b.vertex.position.y - a.vertex.position.y) * t},
      .color = MixColor(a.vertex.color, b.vertex.color, t),
    },
    .source_index = t == 0 ? a.source_index : (t == 1 ? b.source_index : -1),
  };
  /* The clipped coordinate is exact; the other coordinate and color retain
   * the intersection with the original diagonal, rather than being clamped. */
  if (edge == 0) result.vertex.position.x = clip->x0;
  if (edge == 1) result.vertex.position.x = clip->x1;
  if (edge == 2) result.vertex.position.y = clip->y0;
  if (edge == 3) result.vertex.position.y = clip->y1;
  return result;
}

static bool ForestClipPush(ForestClipVertex *vertices, int *count, ForestClipVertex vertex) {
  if (*count && vertices[*count - 1].vertex.position.x == vertex.vertex.position.x &&
      vertices[*count - 1].vertex.position.y == vertex.vertex.position.y)
    return true;
  if (*count >= 8) return false;
  vertices[(*count)++] = vertex;
  return true;
}

static bool AppendForestClippedTriangle(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ArRenderVertex2D *source, int *mapped, const int *triangle,
    const ActionEffectLocalRect *clip, ActionEffectProjectPointFn project_point, void *userdata) {
  ForestClipVertex polygon[8], scratch[8];
  unsigned common = 15u, any = 0;
  int count = 3;
  for (int i = 0; i < 3; i++) {
    polygon[i] = (ForestClipVertex){source[triangle[i]], triangle[i]};
    const unsigned code = ForestClipCode(polygon[i].vertex.position, clip);
    common &= code;
    any |= code;
  }
  if (common) return true;
  /* Interior triangles keep their original vertices and shared indices.
   * Only boundary triangles need polygon clipping and color interpolation. */
  for (int edge = 0; any && edge < 4 && count; edge++) {
    int next_count = 0;
    for (int i = 0; i < count; i++) {
      const ForestClipVertex a = polygon[i], b = polygon[(i + 1) % count];
      const bool a_inside = ForestClipDistance(a.vertex.position, clip, edge) >= 0;
      const bool b_inside = ForestClipDistance(b.vertex.position, clip, edge) >= 0;
      if (a_inside != b_inside && !ForestClipPush(scratch, &next_count,
              ForestClipIntersection(a, b, clip, edge)))
        return false;
      if (b_inside && !ForestClipPush(scratch, &next_count, b)) return false;
    }
    if (next_count > 1 &&
        scratch[0].vertex.position.x == scratch[next_count - 1].vertex.position.x &&
        scratch[0].vertex.position.y == scratch[next_count - 1].vertex.position.y)
      next_count--;
    memcpy(polygon, scratch, (size_t)next_count * sizeof(polygon[0]));
    count = next_count;
  }
  if (count < 3) return true;
  /* A triangle intersected with a rectangle has at most seven vertices. */
  if (count > 7 || !Reserve(writer, count, (count - 2) * 3)) return false;
  for (int i = 0; i < count; i++) {
    const int source_index = polygon[i].source_index;
    if (source_index >= 0 && mapped[source_index] >= 0) continue;
    ArRenderPointF projected;
    if (!project_point(userdata, mesh, polygon[i].vertex.position.x,
            polygon[i].vertex.position.y, &projected))
      return true;
    polygon[i].vertex.position = projected;
  }
  int indices[7];
  for (int i = 0; i < count; i++) {
    const int source_index = polygon[i].source_index;
    if (source_index >= 0 && mapped[source_index] >= 0) {
      indices[i] = mapped[source_index];
    } else {
      indices[i] = writer->vertex_count;
      if (source_index >= 0) mapped[source_index] = writer->vertex_count;
      writer->vertices[writer->vertex_count++] = polygon[i].vertex;
    }
  }
  for (int i = 1; i < count - 1; i++) {
    writer->indices[writer->index_count++] = indices[0];
    writer->indices[writer->index_count++] = indices[i];
    writer->indices[writer->index_count++] = indices[i + 1];
  }
  return true;
}

static bool AppendForestRays(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, bool foreground,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  ActionEffectLocalRect clip = effect->geometry.data.rect;
  if (clip_bounds) {
    if (!clip_bounds(userdata, effect, &clip)) return true;
  } else if (effect->flags & kActionEffectFlag_ClipToRect) {
    if (!RectIsSane(&effect->clip_rect)) return true;
    clip.x0 = fmaxf(clip.x0, effect->clip_rect.x0);
    clip.y0 = fmaxf(clip.y0, effect->clip_rect.y0);
    clip.x1 = fminf(clip.x1, effect->clip_rect.x1);
    clip.y1 = fminf(clip.y1, effect->clip_rect.y1);
  }
  if (!RectIsSane(&clip) || clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
  static const float rows[kForestRayRows] = {0, 224, 272, 352, 448, 544};
  static const float boss_rows[kForestRayRows] = {0, 192, 224, 288, 416, 544};
  unsigned visible = 0;
  for (unsigned i = 0; i < kForestOpeningCount; i++) {
    const ForestCanopyOpening *opening = &kForestOpenings[i];
    const bool boss = opening->fan == kForestBossFan;
    const float across[kForestRayColumns] = {0, opening->shoulder, 1, opening->shoulder, 0};
    const ForestRayState ray = ForestRayAt(effect, i);
    if (!ForestRayIntersectsField(effect, i, ray)) continue;
    if (++visible > kForestVisibleRays) return false;
    ArRenderVertex2D source[kForestRayRows * kForestRayColumns];
    int mapped[kForestRayRows * kForestRayColumns];
    for (int row = 0; row < kForestRayRows; row++) {
      const float y = boss ? boss_rows[row] : rows[row];
      const float world_y = effect->world_y + y;
      const float centre = opening->x - ray.slope * world_y + ray.sway - effect->world_x;
      const float width = ForestRayWidth(opening, world_y);
      /* Only this HUD fade follows screen Y. Ray positions, widths and lower
       * attenuation belong to the world and scroll with the light layer. */
      /* The clearing's surface light reaches the tall rider above the horse,
       * while keeping the first 32 native screen rows free of foreground light. */
      const float fade = foreground ? ForestSoftRamp(boss ? (y - 192) / 32 : (y - 224) / 64) :
          1 - .50f * ForestSoftRamp((world_y - 224) / 560);
      for (int column = 0; column < kForestRayColumns; column++) {
        const float x = centre + (column * .5f - 1) * width;
        const float amount = opening->strength * fade * across[column];
        ArRenderColorF color = foreground
            ? (boss ? (ArRenderColorF){.88f, .79f, .53f, 1} :
                      (ArRenderColorF){.60f, .56f, .39f, 1})
            : (ArRenderColorF){1, .96f, .76f, boss ? .32f : .26f};
        if (foreground) {
          color.r *= amount;
          color.g *= amount;
          color.b *= amount;
        } else {
          color.a *= amount;
        }
        const int index = row * kForestRayColumns + column;
        source[index] = (ArRenderVertex2D){{x, y}, color, {0,0}};
        mapped[index] = -1;
      }
    }
    for (int row = 0; row < kForestRayRows - 1; row++) {
      for (int column = 0; column < kForestRayColumns - 1; column++) {
        const int a = row * kForestRayColumns + column, b = a + kForestRayColumns;
        const int cell[] = {a,a+1,b, a+1,b+1,b};
        for (int j = 0; j < 6; j += 3)
          if (!AppendForestClippedTriangle(writer, &mesh, source, mapped, &cell[j],
                  &clip, project_point, userdata))
            return false;
      }
    }
  }
  return true;
}

static bool AppendForestRayDust(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ForestRayState *rays, ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  /* Both the lifetime and ray sway divide the captured 16-bit scene clock.
   * Dust pockets are fixed in world space; the light can sway across them. */
  const SceneParticleLifetime lifetime = {1024, 0, 0};
  unsigned visible = 0;
  for (unsigned i = 0; i < kForestOpeningCount; i++) {
    if (!ForestRayIntersectsField(effect, i, rays[i])) continue;
    if (++visible > kForestVisibleRays) return false;
    const ForestCanopyOpening *opening = &kForestOpenings[i];
    const float slope = rays[i].slope, sway = rays[i].sway;
    for (unsigned cluster = 0; cluster < kForestDustClusters; cluster++) {
      const uint32_t seed = DeterministicHash_Mix32(effect->pulse_generation ^
          (i + 1u) * 0x9E3779B9u ^ (cluster + 1u) * 0x85EBCA6Bu);
      const float cloud_y = 64 + cluster * 144 + 56 * HashUnit(seed ^ 0x71u);
      const float cloud_width = ForestRayWidth(opening, cloud_y);
      const float cloud_x = opening->x - slope * cloud_y +
          (.8f * HashUnit(seed ^ 0x39u) - .4f) * cloud_width;
      ActionEffectInstance dust = *effect;
      dust.pulse_generation = seed;
      for (unsigned mote = 0; mote < kForestDustPerCluster; mote++) {
        const SceneParticleClock clock = SceneParticleClockAt(
            &dust, effect->phase_ticks, mote, lifetime);
        const float turn = clock.t * 6.2831853f + HashUnit(clock.seed) * 6.2831853f;
        const float world_x = cloud_x + (HashUnit(clock.seed ^ 0x93u) - .5f) *
            fminf(40, cloud_width * .9f) + 9 * (clock.t - .5f) + 4 * sinf(turn);
        const float world_y = cloud_y + (HashUnit(clock.seed ^ 0xB5u) - .5f) * 42 -
            12 * (clock.t - .5f) + 3 * cosf(turn);
        const float x = world_x - effect->world_x, y = world_y - effect->world_y;
        if (x < rect->x0 || x > rect->x1 || y < rect->y0 || y > rect->y1) continue;
        const float centre = opening->x - slope * world_y + sway;
        const float light = opening->strength * ForestSoftRamp(
            1 - fabsf(world_x - centre) / ForestRayWidth(opening, world_y));
        const float alpha = .82f * light * ForestSoftRamp(clock.t * 8) *
            ForestSoftRamp((1 - clock.t) * 8) * (.65f + .35f * HashUnit(clock.seed ^ 0xA7u));
        if (alpha < .015f) continue;
        const float size = .45f + .30f * HashUnit(clock.seed ^ 0xD3u);
        if (!AppendSceneParticle(writer, effect, x, y, x, y - .1f,
                size, size, (ArRenderColorF){1, .95f, .72f, alpha}, project_point, userdata))
          return false;
      }
    }
  }
  return true;
}

static bool AppendForestMotes(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  ForestRayState rays[kForestOpeningCount];
  ForestRayStates(effect, rays);
  const SceneParticleLifetime lifetime = {512, 0, 0};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const int first_cell = (int)floorf((effect->world_x + rect->x0) / 36);
  for (unsigned i = 0; i < kForestMotes; i++) {
    const int cell = first_cell + (int)i;
    ActionEffectInstance mote = *effect;
    mote.generation += (uint32_t)cell;
    mote.pulse_generation += (uint32_t)cell;
    const SceneParticleClock clock =
        SceneParticleClockAt(&mote, effect->phase_ticks, 0, lifetime);
    const float x = cell * 36 + 6 + 22 * HashUnit(clock.seed ^ 0x39u) +
        6 * clock.t - effect->world_x;
    const float y = 32 + 544 * HashUnit(clock.seed ^ 0x71u) -
        30 * clock.t - effect->world_y;
    if (x < rect->x0 || x > rect->x1 || y < rect->y0 || y > rect->y1)
      continue;
    const float light = sqrtf(ForestLightAmount(effect, rays, x, y));
    const float alpha = .94f * fmaxf(0, sinf(clock.t * 3.14159265f)) * (.55f + .45f * light);
    const float size = 1.30f + .40f * HashUnit(clock.seed ^ 0x93u);
    if (!AppendSceneParticle(writer, effect, x, y, x, y - .1f,
            size, size * 1.5f, (ArRenderColorF){1.0f, .97f, .75f, alpha},
            project_point, userdata))
      return false;
  }
  return AppendForestRayDust(writer, effect, rays, project_point, userdata);
}

static bool AppendForestLeaves(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  ForestRayState rays[kForestOpeningCount];
  ForestRayStates(effect, rays);
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const int first_cell = (int)floorf((effect->world_x + rect->x0) / 48);
  /* Divide the captured 16-bit clock's range so its wrap cannot jump leaves. */
  const SceneParticleLifetime lifetime = {2048, 0, 0};
  for (int i = 0; i < kForestLeaves; i++) {
    const int cell = first_cell + i;
    ActionEffectInstance leaf = *effect;
    leaf.pulse_generation += (uint32_t)cell * 31u + 0x7100u;
    const SceneParticleClock clock =
        SceneParticleClockAt(&leaf, effect->phase_ticks, 0, lifetime);
    const float turn = clock.t * 37.6991f + HashUnit(clock.seed) * 6.2831853f;
    const float x = cell * 48 + 12 + HashUnit(clock.seed ^ 0x38u) * 24 +
        12 * sinf(turn) - effect->world_x;
    const float y = -48 + clock.t * 720 - effect->world_y;
    if (x < rect->x0 || x > rect->x1 || y < rect->y0 || y > rect->y1) continue;
    const float light = sqrtf(ForestLightAmount(effect, rays, x, y));
    const float alpha = .90f *
        ForestSoftRamp(clock.t * 12) * ForestSoftRamp((1 - clock.t) * 12);
    const float angle = .65f * sinf(turn * .7f);
    const float c = cosf(angle), s = sinf(angle);
    const float twist = .30f + .70f * fabsf(cosf(turn));
    static const ArRenderPointF shape[] = {
      {0,-5}, {2.2f,-1.8f}, {2.6f,1.6f}, {0,5}, {-2.6f,1.6f}, {-2.2f,-1.8f},
    };
    ArRenderPointF points[6];
    bool visible = true;
    for (int j = 0; j < 6; j++) {
      const float lx = shape[j].x * twist, ly = shape[j].y;
      if (!project_point(userdata, effect, x + c * lx - s * ly,
              y + s * lx + c * ly, &points[j])) {
        visible = false;
        break;
      }
    }
    if (!visible) continue;
    if (!Reserve(writer, 9, 15)) return false;
    const int base = writer->vertex_count;
    for (int j = 0; j < 6; j++)
      writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
        points[j], {.055f, .11f, .035f, alpha}, {0,0},
      };
    for (int j = 1; j < 5; j++) {
      writer->indices[writer->index_count++] = base;
      writer->indices[writer->index_count++] = base + j;
      writer->indices[writer->index_count++] = base + j + 1;
    }
    /* A narrow lit edge keeps the dark leaf silhouette readable between rays.
     * It follows the same turning polygon, not a detached sparkle or halo. */
    const ArRenderPointF rim[] = {points[1], points[2],
      {points[1].x * .72f + points[5].x * .28f,
       points[1].y * .72f + points[5].y * .28f}};
    for (int j = 0; j < 3; j++) {
      writer->indices[writer->index_count++] = writer->vertex_count;
      writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
        rim[j], {.66f, .70f, .24f, alpha * (.45f + .55f * light)}, {0,0},
      };
    }
  }
  return true;
}

/* These dispatchers select complete family operations. Palettes, motion,
 * timing and phase-specific geometry stay together above. */
static bool AppendSceneParticles(ActionEffectGeometryWriter *writer,
                                 const ActionEffectInstance *effect,
                                 ActionEffectProjectPointFn project_point,
                                 void *userdata) {
  switch (effect->kind) {
  case kActionEffect_ForestLeaves:
    return AppendForestLeaves(writer, effect, project_point, userdata);
  case kActionEffect_ForestCanopyLight:
    return AppendForestMotes(writer, effect, project_point, userdata);
  case kActionEffect_AitosLavaPit:
    return AppendLavaPitParticles(writer, effect, project_point, userdata);
  case kActionEffect_AitosLavaReservoir:
    return AppendLavaReservoirParticles(writer, effect, project_point,
                                        userdata);
  case kActionEffect_AitosMoltenRock:
    return AppendMoltenRockParticles(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterSplash:
    return AppendWaterSplashParticles(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterfall:
    return AppendWaterfallParticles(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterfallMist:
    return AppendWaterfallMistParticles(writer, effect, project_point,
                                        userdata);
  case kActionEffect_WallTorch:
    return AppendWallTorchParticles(writer, effect, project_point, userdata);
  case kActionEffect_AitosStatueFire:
    return AppendStatueFireParticles(writer, effect, project_point, userdata);
  case kActionEffect_EnemyFireball:
  case kActionEffect_MarahnaFireball:
  case kActionEffect_AitosLavaFireball:
    return AppendFireballParticles(writer, effect, project_point, userdata);
  case kActionEffect_FlamingWheel:
    return AppendFlamingWheelParticles(writer, effect, project_point, userdata);
  case kActionEffect_FlamingWheelProjectile:
    return AppendFlamingWheelProjectileParticles(writer, effect, project_point,
                                                 userdata);
  case kActionEffect_MinotaurAxe:
  case kActionEffect_IceDragonIceBall:
  case kActionEffect_TanzaraProjectile:
    return AppendActorProjectileParticles(writer, effect, project_point,
                                          userdata);
  case kActionEffect_SwordBeam:
    return AppendSwordBeamParticles(writer, effect, project_point, userdata);
  case kActionEffect_MarahnaLightningLink:
    return AppendMarahnaLightningLinkParticles(writer, effect, project_point,
                                               userdata);
  case kActionEffect_MarahnaBossLightning:
    return AppendMarahnaBossLightningParticles(writer, effect, project_point,
                                               userdata);
  case kActionEffect_LightningTrap:
    return AppendLightningTrapParticles(writer, effect, project_point,
                                        userdata);
  case kActionEffect_NorthwallBossMagic:
    return AppendNorthwallMagicParticles(writer, effect, project_point, userdata);
  case kActionEffect_CentaurLightning:
  case kActionEffect_BloodpoolBossLightning:
    return AppendBossLightningParticles(writer, effect, project_point, userdata);
  default:
    return true;
  }
}

static bool AppendSceneLighting(ActionEffectGeometryWriter *writer,
                                const ActionEffectInstance *effect,
                                ActionEffectProjectPointFn project_point,
                                ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  switch (effect->kind) {
  case kActionEffect_ForestForwardLight:
    return AppendForestRays(writer, effect, true, project_point, clip_bounds, userdata);
  case kActionEffect_ForestCanopyLight:
    return AppendForestRays(writer, effect, false, project_point, clip_bounds, userdata);
  case kActionEffect_AitosLavaReservoir:
    return AppendLavaReservoirLighting(
        writer, effect, DeterministicPulse(effect), project_point, userdata);
  case kActionEffect_WallTorch:
    return AppendWallTorchLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosStatueFire:
    return AppendStatueFireLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosLavaPit:
    return AppendLavaPitLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosMoltenRock:
    return AppendMoltenRockLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterSplash:
    return AppendWaterSplashLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterfall:
    return AppendWaterfallLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterfallMist:
    return AppendWaterfallMistCloudVolume(
        writer, effect, DeterministicPulse(effect), project_point, userdata);
  case kActionEffect_EnemyFireball:
  case kActionEffect_MarahnaFireball:
  case kActionEffect_AitosLavaFireball:
    return AppendFireballLighting(writer, effect, project_point, userdata);
  case kActionEffect_FlamingWheel:
    return AppendFlamingWheelLighting(writer, effect, project_point, userdata);
  case kActionEffect_FlamingWheelProjectile:
    return AppendFlamingWheelProjectileLighting(writer, effect, project_point,
                                                userdata);
  case kActionEffect_MinotaurAxe:
  case kActionEffect_IceDragonIceBall:
  case kActionEffect_TanzaraProjectile:
    return AppendActorProjectileLighting(writer, effect, project_point,
                                         userdata);
  case kActionEffect_MarahnaLightningLink:
    return AppendMarahnaLightningLinkLighting(writer, effect, project_point,
                                              userdata);
  case kActionEffect_MarahnaBossLightning:
    return AppendMarahnaBossLightningLighting(writer, effect, project_point,
                                              userdata);
  case kActionEffect_SwordBeam:
    return AppendSwordBeamLighting(writer, effect, project_point, userdata);
  case kActionEffect_LightningTrap:
    return AppendLightningTrapLighting(writer, effect, project_point, userdata);
  case kActionEffect_NorthwallBossMagic:
    return AppendNorthwallMagicLighting(writer, effect, project_point, userdata);
  case kActionEffect_CentaurLightning:
  case kActionEffect_BloodpoolBossLightning:
    return AppendBossLightningLighting(writer, effect, project_point, userdata);
  default:
    return true;
  }
}

static bool SceneEffectStyleKnown(const ActionEffectInstance *effect) {
  if (!effect) return false;
  switch (effect->kind) {
    case kActionEffect_ForestForwardLight:
      return effect->phase == kActionEffectPhase_ForestCanopyLight &&
          effect->render_layer == kActionEffectRenderLayer_ForegroundLight &&
          effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds;
    case kActionEffect_ForestLeaves:
      return effect->phase == kActionEffectPhase_ForestCanopyLight &&
          effect->render_layer == kActionEffectRenderLayer_Bg2Foliage &&
          effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds;
    case kActionEffect_ForestCanopyLight:
      return effect->phase == kActionEffectPhase_ForestCanopyLight &&
          effect->render_layer == kActionEffectRenderLayer_Bg2Plane &&
          effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds &&
          effect->geometry.data.rect.x0 == -384 && effect->geometry.data.rect.x1 == 384 &&
          effect->geometry.data.rect.y0 == 0 && effect->geometry.data.rect.y1 == 544;
    case kActionEffect_WallTorch:
      return effect->phase == kActionEffectPhase_WallTorch;
    case kActionEffect_EnemyFireball:
      return effect->phase == kActionEffectPhase_EnemyFireballFlight;
    case kActionEffect_MarahnaFireball:
      return (effect->phase == kActionEffectPhase_MarahnaFireballOrb &&
              effect->visual >= 0x05u && effect->visual <= 0x08u) ||
             (effect->phase == kActionEffectPhase_MarahnaFireballSplit &&
              (effect->visual == 0x32u || effect->visual == 0x33u)) ||
             (effect->phase == kActionEffectPhase_MarahnaSnakeFireballShot &&
              (effect->visual == 0x1Du || effect->visual == 0x1Eu));
    case kActionEffect_AitosLavaPit:
      return effect->phase == kActionEffectPhase_AitosLavaPit;
    case kActionEffect_AitosLavaReservoir:
      return effect->phase == kActionEffectPhase_AitosLavaReservoir;
    case kActionEffect_AitosLavaFireball:
      return effect->phase == kActionEffectPhase_AitosLavaFireballFlight;
    case kActionEffect_AitosStatueFire:
      return effect->phase == kActionEffectPhase_AitosStatueFireBreath &&
             effect->visual >= 0x1Cu && effect->visual <= 0x1Fu;
    case kActionEffect_AitosMoltenRock:
      return effect->phase == kActionEffectPhase_AitosMoltenRockFlight &&
             effect->visual == 0x2Bu;
    case kActionEffect_AitosWaterSplash:
      return effect->phase == kActionEffectPhase_AitosWaterSplash;
    case kActionEffect_AitosWaterfall:
      return effect->phase == kActionEffectPhase_AitosWaterfallFlow;
    case kActionEffect_AitosWaterfallMist:
      return effect->phase == kActionEffectPhase_AitosWaterfallMist;
    case kActionEffect_MarahnaLightningLink:
      return effect->phase == kActionEffectPhase_MarahnaLightningActive &&
             ((effect->visual == 0x2Eu && effect->animation_state == 0x27u) ||
              (effect->visual == 0x31u && effect->animation_state == 0x28u));
    case kActionEffect_MarahnaBossLightning:
      return (effect->phase == kActionEffectPhase_MarahnaBossLightningCharge &&
              (effect->visual == 0x07u || effect->visual == 0x08u)) ||
             (effect->phase == kActionEffectPhase_MarahnaBossLightningOrb &&
              effect->visual == 0x0Au) ||
             (effect->phase == kActionEffectPhase_MarahnaBossLightningBolt &&
              effect->visual == 0x11u) ||
             (effect->phase ==
                  kActionEffectPhase_MarahnaBossLightningGroundCharge &&
              effect->visual >= 0x12u && effect->visual <= 0x14u);
    case kActionEffect_LightningTrap:
      return effect->phase == kActionEffectPhase_LightningActive;
    case kActionEffect_CentaurLightning:
      return (effect->phase == kActionEffectPhase_CentaurStaffCharge && effect->visual >= 8 &&
              effect->visual <= 11) ||
             (effect->phase == kActionEffectPhase_BossLightningStrike && effect->visual >= 0x19 &&
              effect->visual <= 0x20) ||
             (effect->phase == kActionEffectPhase_BossLightningImpact && effect->visual >= 0x21 &&
              effect->visual <= 0x23);
    case kActionEffect_NorthwallBossMagic:
      return (effect->phase == kActionEffectPhase_NorthwallMagicCharge && effect->visual >= 0x0A &&
              effect->visual <= 0x0E) ||
             (effect->phase == kActionEffectPhase_NorthwallMagicFall && effect->visual == 9) ||
             (effect->phase == kActionEffectPhase_NorthwallMagicImpact && effect->visual >= 3 &&
              effect->visual <= 8);
    case kActionEffect_BloodpoolBossLightning:
      return (effect->phase == kActionEffectPhase_BossLightningStrike &&
              effect->visual <= 5u) ||
             (effect->phase == kActionEffectPhase_BossLightningImpact &&
              effect->visual >= 8u && effect->visual <= 10u);
    case kActionEffect_SwordBeam:
      return effect->phase == kActionEffectPhase_SwordBeamFlight &&
             (effect->visual == 0x20u || effect->visual == 0x21u ||
              effect->visual == 0x30u || effect->visual == 0x31u);
    case kActionEffect_MinotaurAxe:
      return effect->phase == kActionEffectPhase_MinotaurAxeFlight &&
             (effect->visual <= 0x07u || effect->visual == 0x10u);
    case kActionEffect_FlamingWheel:
      return effect->phase == kActionEffectPhase_FlamingWheelBody;
    case kActionEffect_FlamingWheelProjectile:
      return effect->phase ==
                 kActionEffectPhase_FlamingWheelProjectileFlight &&
          effect->visual <= 0x03u;
    case kActionEffect_IceDragonIceBall:
      return effect->phase == kActionEffectPhase_IceDragonIceBallFlight &&
          effect->visual >= 0x12u && effect->visual <= 0x19u;
    case kActionEffect_TanzaraProjectile:
      return effect->phase == kActionEffectPhase_TanzaraProjectileFlight;
    default:
      return false;
  }
}

static bool BuildSceneEffectList(
    const ActionEffectInstance *effects, uint8_t effect_count,
    uint8_t capacity, bool overflow, uint8_t render_layer,
    bool lighting_enabled, bool particles_enabled,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *project_userdata,
    ActionSceneEffectRenderBatch *batch) {
  if (!batch) return false;
  /* Submitters consume only [0, count). The scene capacity is deliberately
   * large, so zeroing its unused tail would touch hundreds of KiB per build. */
  batch->vertex_count = 0;
  batch->index_count = 0;
  if (!effects || effect_count > capacity ||
      render_layer >= kActionEffectRenderLayer_Count)
    return false;
  if (overflow || (!lighting_enabled && !particles_enabled)) return true;
  if (!project_point) return false;
  ActionEffectGeometryWriter writer = GeometryWriter(
      batch->vertices, kActionSceneEffectRenderMaxVertices,
      batch->indices, kActionSceneEffectRenderMaxIndices);
  unsigned lightning_filaments = 0;
  unsigned marahna_lightning_links = 0;
  unsigned marahna_boss_lightning_bolts = 0;
  unsigned sword_streams = 0;
  unsigned waterfall_veils = 0;
  unsigned waterfall_mists = 0;
  unsigned lava_reservoirs = 0;
  unsigned lava_glow_segments = 0;
  unsigned flaming_wheels = 0;
  unsigned forest_rays = 0;

  for (uint8_t i = 0; i < effect_count; i++) {
    const ActionEffectInstance *effect = &effects[i];
    if (!(effect->flags & kActionEffectFlag_Visible) ||
        effect->geometry.kind != kActionEffectGeometry_Rect ||
        !RectIsSane(&effect->geometry.data.rect) ||
        !SceneEffectStyleKnown(effect) ||
        effect->obj_priority >= kActionEffectObjPriorityCount ||
        effect->render_layer != render_layer ||
        effect->projection_plane > kActionEffectProjectionPlane_BetweenBackgrounds)
      continue;
    if ((effect->kind == kActionEffect_BloodpoolBossLightning ||
         effect->kind == kActionEffect_CentaurLightning) &&
        effect->phase == kActionEffectPhase_BossLightningStrike &&
        ++lightning_filaments > kActionSceneEffectMaxLightningFilaments)
      return false;
    if (effect->kind == kActionEffect_MarahnaLightningLink &&
        ++marahna_lightning_links >
            kActionSceneEffectMaxMarahnaLightningLinks)
      return false;
    if (effect->kind == kActionEffect_MarahnaBossLightning &&
        effect->phase == kActionEffectPhase_MarahnaBossLightningBolt &&
        ++marahna_boss_lightning_bolts >
            kActionSceneEffectMaxMarahnaBossLightningBolts)
      return false;
    if (effect->kind == kActionEffect_SwordBeam &&
        ++sword_streams > kActionSceneEffectMaxSwordStreams)
      return false;
    if (effect->kind == kActionEffect_AitosWaterfall &&
        ++waterfall_veils > kActionSceneEffectMaxWaterfallVeils)
      return false;
    if (effect->kind == kActionEffect_AitosWaterfallMist &&
        ++waterfall_mists > kActionSceneEffectMaxWaterfallVeils)
      return false;
    if (effect->kind == kActionEffect_AitosLavaReservoir) {
      if (++lava_reservoirs > kActionSceneEffectMaxLavaReservoirs)
        return false;
      const unsigned segments = LavaReservoirGlowSegmentCount(effect);
      if (!segments ||
          segments > kActionSceneEffectMaxLavaGlowSegments -
              lava_glow_segments)
        return false;
      lava_glow_segments += segments;
    }
    if (effect->kind == kActionEffect_FlamingWheel &&
        ++flaming_wheels > kActionSceneEffectMaxFlamingWheels)
      return false;

    if ((effect->kind == kActionEffect_ForestCanopyLight ||
         effect->kind == kActionEffect_ForestForwardLight) && ++forest_rays > 1)
      return false;
    if (lighting_enabled &&
        !AppendSceneLighting(&writer, effect, project_point, clip_bounds,
                             project_userdata))
      return false;
    if (particles_enabled &&
        !AppendSceneParticles(&writer, effect, project_point,
                              project_userdata))
      return false;
  }
  batch->vertex_count = writer.vertex_count;
  batch->index_count = writer.index_count;
  return true;
}

bool ActionSceneEffectRender_Build(const ActionSceneEffectFrame *frame,
                                   bool lighting_enabled,
                                   bool particles_enabled,
                                   ActionEffectProjectPointFn project_point,
                                   void *project_userdata,
                                   ActionSceneEffectRenderBatch *batch) {
  if (!frame) {
    if (batch) {
      batch->vertex_count = 0;
      batch->index_count = 0;
    }
    return false;
  }
  return BuildSceneEffectList(
      frame->effects, frame->effect_count, kActionSceneEffectMaxInstances,
      frame->overflow, kActionEffectRenderLayer_WorldOverlay,
      lighting_enabled, particles_enabled, project_point, NULL, project_userdata,
      batch);
}

bool ActionSceneDecorationRender_Build(
    const ActionSceneEffectFrame *frame, uint8_t render_layer,
    bool lighting_enabled, bool particles_enabled,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *project_userdata,
    ActionSceneEffectRenderBatch *batch) {
  if (!frame) {
    if (batch) {
      batch->vertex_count = 0;
      batch->index_count = 0;
    }
    return false;
  }
  return BuildSceneEffectList(
      frame->decorations, frame->decoration_count,
      kActionSceneDecorationMaxInstances, frame->decoration_overflow,
      render_layer, lighting_enabled, particles_enabled, project_point, clip_bounds,
      project_userdata, batch);
}
