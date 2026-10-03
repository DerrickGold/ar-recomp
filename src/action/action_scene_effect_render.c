#include "action/action_effect_receivers.h"
/* ActionSceneEffectRender: geometry for the effects placed in action scenes
 * (lava pits and reservoirs, molten rock, water splashes, waterfalls and their
 * mist, wall torches, statue fire, fireballs, the flaming wheel, projectiles),
 * each as particles plus lighting, and the dispatch from a scene effect to its
 * family. Lightning lives in action_scene_lightning_render.c.
 * Phase: pure.
 * Tests: tests/action_effect_render_test.c */
#include "action/action_effect_render_internal.h"

static float SurfacePulse(const ActionEffectInstance *effect,const ActionSurfaceField *f){
  const unsigned ticks=(unsigned)f->Clock[0]*effect->phase_ticks;
  const unsigned seed=(unsigned)effect->visual*(unsigned)f->Clock[1];
  return f->Pulse[0]+f->Pulse[1]*TriangleWave(ticks+seed,(unsigned)f->Clock[2])+
    f->Pulse[2]*TriangleWave(ticks+seed*(unsigned)f->Clock[4],(unsigned)f->Clock[3]);
}
static ActionEffectGlowStyle SurfaceStyle(const ActionSurfaceField *f,unsigned index,float width,float height,unsigned seed){
  ActionEffectGlowStyle style=f->styles[index];const float *v=index?f->Body:f->Spill;
  style.radius_x=fmaxf(v[0],width*v[2]+v[4]);style.radius_y=fmaxf(v[1],height*v[3]+v[5]);style.seed=seed;return style;
}

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
bool AppendSceneSoftCloud(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,
    float local_x, float local_y, float local_radius_x, float local_radius_y,
    ArRenderColorF tint, float opacity, unsigned seed,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum {
    kSegments = kActionSceneEffectWaterfallMistCloudSegments,
    kRings = kActionEffectGlowRings,
  };
  if (writer->source) {
    float corners[kActionSceneEffectWaterfallMistCloudVertices][4] = {{0}};
    const ArRenderColorF white = {1, 1, 1, opacity};
    ArRenderColorF colors[4] = {MixColor(tint, white, .42f),
        MixColor(tint, white, .24f), tint, tint};
    colors[0].a = opacity; colors[1].a = opacity * .84f;
    colors[2].a = opacity * .34f; colors[3].a = 0;
    const float scales[3] = {.24f, .70f, 1};
    const unsigned ticks = EffectVisualTicks(effect, (unsigned)effect->pulse_ticks) / 4u;
    for (int r = 0; r < kRings; ++r) for (int s = 0; s < kSegments; ++s) {
      const float shape = 1 + (.035f + .055f * r) *
          FlameSilhouette(seed, ticks, s * kActionEffectGlowSegments / kSegments);
      corners[1 + r * kSegments + s][0] = kMistCircle12[s][0] * scales[r] * shape;
      corners[1 + r * kSegments + s][3] = kMistCircle12[s][1] * scales[r] * shape;
    }
    return AppendSourceBillboard(writer, effect, local_x, local_y,
        local_radius_x, local_radius_y, 4, 3, kSegments, corners, colors);
  }
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
    const ActionEffectInstance *effect, const ActionSurfaceField *f, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  const unsigned kColumns=(unsigned)f->CloudCounts[0],kTiers=(unsigned)f->CloudCounts[1];
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float span = rect->x1 - rect->x0;
  const unsigned ticks = (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;

  /* Back to front: broad cool banks establish depth, rising mid-tier puffs
   * break their upper silhouette, and the compact bright tier reads as fresh
   * foam boiling where the waterfall meets the cloud. */
  for (unsigned cloud = 0;
       cloud < kColumns*kTiers; cloud++) {
    const unsigned tier = cloud / kColumns;
    const unsigned column = cloud % kColumns;
    const unsigned seed = DeterministicHash_Mix32(
        (uint32_t)effect->pulse_generation ^
        ((uint32_t)cloud + 1u) * 0x9E3779B9u);
    const unsigned x_period = (unsigned)f->CloudPeriods[0] + tier * (unsigned)f->CloudPeriods[1] + (seed & (unsigned)f->CloudPeriods[2]);
    const unsigned y_period = (unsigned)f->CloudPeriods[3] + tier * (unsigned)f->CloudPeriods[4] + ((seed >> 4) & (unsigned)f->CloudPeriods[5]);
    float x_wave = TriangleWave(ticks + seed % x_period, x_period) * 2.0f - 1.0f;
    float y_wave = TriangleWave(
        ticks + (seed >> 8) % y_period, y_period);
    if (cloud & 1u) x_wave = -x_wave;
    const float lane = ((float)column + 0.5f) / (float)kColumns;
    const float jitter_x = (HashUnit(seed ^ 0x53u) - 0.5f) * f->CloudJitter[0];
    const float jitter_y = (HashUnit(seed ^ 0xB5u) - 0.5f) *
        (tier == 3u ? f->CloudJitter[2] : f->CloudJitter[1]);
    float x = rect->x0 + span * lane + f->tiers[tier][4] +
        jitter_x + x_wave * (f->CloudJitter[3] + f->CloudJitter[4] * (float)tier);
    float y = f->tiers[tier][0] + jitter_y -
        y_wave * (f->CloudJitter[5] + f->CloudJitter[6] * (float)tier);
    /* A fixed first anchor gives the production projection regression one
     * stable point while the other 23 puffs drift independently around it. */
    if (cloud == 0u && f->CloudPeriods[7]) {
      x = rect->x0 + span * lane;
      y = f->tiers[tier][0];
    }
    const float size_jitter = f->CloudShape[0] + f->CloudShape[1] * HashUnit(seed ^ 0x71u);
    const float breathe = f->CloudShape[2] + f->CloudShape[3] * TriangleWave(
        ticks + (seed >> 16) % (unsigned)f->CloudPeriods[6], (unsigned)f->CloudPeriods[6]);
    const float opacity = f->tiers[tier][3] *
        (f->CloudShape[4] + f->CloudShape[5] * pulse) *
        (f->CloudShape[6] + f->CloudShape[7] * HashUnit(seed ^ 0xA7u));
    if (!AppendSceneSoftCloud(
            writer, effect, x, y,
            f->tiers[tier][1] * size_jitter * breathe,
            f->tiers[tier][2] * size_jitter / breathe,
            ((ArRenderColorF){f->tiers[tier][5],f->tiers[tier][6],f->tiers[tier][7],f->tiers[tier][8]}), opacity, seed,
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

static void SceneFireballHeading(const ActionEffectInstance *effect,const ActionProjectileField *f,float *x,float *y){
  if(SceneActorHeading(effect,x,y))return;
  const float *rest=effect->kind==kActionEffect_MarahnaFireball&&effect->phase==kActionEffectPhase_MarahnaFireballOrb?f->OrbHeading:f->RestHeading;
  *x=rest[0];*y=rest[1];
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
    const ActionEffectInstance *effect,const ActionSurfaceField *f) {
  if (!effect || effect->kind != kActionEffect_AitosLavaReservoir ||
      effect->geometry.kind != kActionEffectGeometry_Rect)
    return 0;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float width = rect->x1 - rect->x0;
  if (!isfinite(width) || width <= 0.0f) return 0;
  if (width > (float)(kActionSceneEffectMaxLavaGlowSegments *
                      f->Segments[0]))
    return kActionSceneEffectMaxLavaGlowSegments + 1u;
  return (unsigned)ceilf(
      width / (float)f->Segments[0]);
}

/* Long Act-2 lakes cannot use one reservoir-wide radial gradient: its outer
 * ring is intentionally transparent, so the ends look unlit until the camera
 * approaches the lake centre. Overlapping bounded emitters keep every part of
 * the lip locally hot and also follow Diorama perspective more faithfully. */
static bool AppendLavaReservoirLighting(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionSurfaceField *f, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned segments = LavaReservoirGlowSegmentCount(effect,f);
  if (!segments || segments > kActionSceneEffectMaxLavaGlowSegments)
    return false;
  const float segment_width = (rect->x1 - rect->x0) / (float)segments;
  for (unsigned i = 0; i < segments; i++) {
    const float centre_x = rect->x0 + ((float)i + 0.5f) * segment_width;
    const ActionEffectGlowStyle spill=SurfaceStyle(f,0,segment_width,0,(unsigned)effect->generation+i*0x5BD1u);
    const ActionEffectGlowStyle body=SurfaceStyle(f,1,segment_width,0,(unsigned)effect->pulse_generation+i*0x7A4Du);
    if (!AppendGlowAtTicks(writer, effect, &spill, pulse, centre_x+f->Spill[31], f->Spill[32],
                    project_point, userdata,(unsigned)f->Clock[0]*effect->pulse_ticks) ||
        !AppendGlowAtTicks(writer, effect, &body, pulse, centre_x+f->Body[31], f->Body[32],
                    project_point, userdata,(unsigned)f->Clock[0]*effect->pulse_ticks))
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
  if (writer->source) {
    const bool ok = ActionEffectSource_Particle(writer->source, effect, x, y,
        previous_x, previous_y, width, reach, color);
    writer->vertex_count = (int)writer->source->count;
    return ok;
  }
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
  if (writer->source) {
    const bool ok = ActionEffectSource_Star(writer->source, effect, local_x, local_y, size, color);
    writer->vertex_count = (int)writer->source->count;
    return ok;
  }
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
                                   const ActionEffectInstance *effect, const ActionSurfaceField *f,
                                   ActionEffectProjectPointFn project_point,
                                   void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot={f->Hot[0],f->Hot[1],f->Hot[2],f->Hot[3]};
  const ArRenderColorF cool={f->Cool[0],f->Cool[1],f->Cool[2],f->Cool[3]};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width, reach = f->Shape[2] + f->Shape[3] * t;
    const float half_width = (rect->x1 - rect->x0) * 0.5f;
    const float birth =
        (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) * fmaxf(1.0f, half_width - f->Motion[0]);
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * f->Motion[1];
    x = birth + drift * t * t;
    /* The captured rectangle covers the full bubbly volume. Its geometric
     * centre still reads too low in the isometric mouth: the apparent
     * surface sits one quarter-height above it. Keep births in a narrow
     * band around that authored plane; the fraction scales correctly for
     * both one- and two-row pits. */
    const float source_surface_y =
        (rect->y0 + rect->y1) * 0.5f - (rect->y1 - rect->y0) * f->Motion[2];
    const float source_y =
        source_surface_y + (HashUnit(seed ^ 0xB5u) - 0.5f) * f->Motion[3];
    y = source_y - f->Motion[4] * t - f->Motion[5] * t * t;
    previous_x = birth + drift * previous_t * previous_t;
    previous_y = source_y - f->Motion[4] * previous_t - f->Motion[5] * previous_t * previous_t;
    width = f->Shape[0] + f->Shape[1] * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendLavaPitLighting(ActionEffectGeometryWriter *writer,
                                  const ActionEffectInstance *effect, const ActionSurfaceField *f,
                                  ActionEffectProjectPointFn project_point,
                                  void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float pulse = SurfacePulse(effect,f);
  const float half_width=(rect->x1-rect->x0)*0.5f,half_height=(rect->y1-rect->y0)*0.5f;
  ActionEffectGlowStyle spill=SurfaceStyle(f,0,half_width,half_height,effect->generation);
  ActionEffectGlowStyle body=SurfaceStyle(f,1,half_width,half_height,effect->pulse_generation);
  const float spill_x=mid_x+f->Spill[31],body_x=mid_x+f->Body[31];
  const float spill_y=half_height*f->Spill[32],body_y=f->Body[32];
  if (!AppendGlowAtTicks(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata,(unsigned)f->Clock[0]*effect->pulse_ticks))
    return false;
  if (!AppendGlowAtTicks(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata, (unsigned)f->Clock[0] * effect->pulse_ticks))
    return false;
  return true;
}

static bool AppendLavaReservoirParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionSurfaceField *f,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot={f->Hot[0],f->Hot[1],f->Hot[2],f->Hot[3]};
  const ArRenderColorF cool={f->Cool[0],f->Cool[1],f->Cool[2],f->Cool[3]};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width, reach = f->Shape[2] + f->Shape[3] * t;
    const float half_width = (rect->x1 - rect->x0) * 0.5f;
    const float birth_x =
        (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) * fmaxf(1.0f, half_width - f->Motion[0]);
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * f->Motion[1];
    const float source_y =
        rect->y0 + f->Motion[2] + (HashUnit(seed ^ 0xB5u) - 0.5f) * f->Motion[3];
    x = birth_x + drift * t * t;
    y = source_y - f->Motion[4] * t - f->Motion[5] * t * t;
    previous_x = birth_x + drift * previous_t * previous_t;
    previous_y = source_y - f->Motion[4] * previous_t - f->Motion[5] * previous_t * previous_t;
    width = f->Shape[0] + f->Shape[1] * (1.0f - t);
    reach = f->Shape[2] + f->Shape[3] * t;

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
                                       const ActionEffectInstance *effect, const ActionSurfaceField *f,
                                       ActionEffectProjectPointFn project_point,
                                       void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot={f->Hot[0],f->Hot[1],f->Hot[2],f->Hot[3]};
  const ArRenderColorF cool={f->Cool[0],f->Cool[1],f->Cool[2],f->Cool[3]};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width, reach = f->Shape[2] + f->Shape[3] * t;
    const float half_width = (rect->x1 - rect->x0) * 0.5f;
    const float birth_x =
        (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) * fmaxf(1.0f, half_width - f->Motion[0]);
    const bool drip = (i & 1u) != 0;
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * f->Motion[1];
    x = birth_x + drift * t;
    previous_x = birth_x + drift * previous_t;
    if (drip) {
      y = f->Motion[2] + f->Motion[3] * t + f->Motion[4] * t * t;
      previous_y = f->Motion[2] + f->Motion[3] * previous_t + f->Motion[4] * previous_t * previous_t;
    } else {
      y = f->Motion[5] - f->Motion[6] * t + f->Motion[7] * t * t;
      previous_y = f->Motion[5] - f->Motion[6] * previous_t + f->Motion[7] * previous_t * previous_t;
    }
    width = f->Shape[0] + f->Shape[1] * (1.0f - t);
    reach = f->Shape[2] + f->Shape[3] * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWaterSplashLighting(ActionEffectGeometryWriter *writer,
                                      const ActionEffectInstance *effect, const ActionSurfaceField *f,
                                      ActionEffectProjectPointFn project_point,
                                      void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float pulse = SurfacePulse(effect,f);
  const float half_width=(rect->x1-rect->x0)*0.5f,half_height=(rect->y1-rect->y0)*0.5f;
  ActionEffectGlowStyle spill=SurfaceStyle(f,0,half_width,half_height,effect->generation);
  ActionEffectGlowStyle body=SurfaceStyle(f,1,half_width,half_height,effect->pulse_generation);
  const float spill_x=mid_x+f->Spill[31],body_x=mid_x+f->Body[31];
  const float spill_y=f->Spill[32],body_y=f->Body[32];
  if (!AppendGlowAtTicks(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata,(unsigned)f->Clock[0]*effect->pulse_ticks))
    return false;
  if (!AppendGlowAtTicks(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata, (unsigned)f->Clock[0] * effect->pulse_ticks))
    return false;
  return true;
}

static bool AppendWaterfallParticles(ActionEffectGeometryWriter *writer,
                                     const ActionEffectInstance *effect, const ActionSurfaceField *f,
                                     ActionEffectProjectPointFn project_point,
                                     void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot={f->Hot[0],f->Hot[1],f->Hot[2],f->Hot[3]};
  const ArRenderColorF cool={f->Cool[0],f->Cool[1],f->Cool[2],f->Cool[3]};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width, reach = f->Shape[2] + f->Shape[3] * t;
    /* Stable lanes, staggered by identity, provide a slow translucent flow
     * over the fast two-frame source cycle. The varying alpha and length
     * break horizontal bands without blurring away the pixel art. */
    const unsigned columns = (unsigned)f->Motion[0];
    _Static_assert(kActionSceneEffectWaterfallParticleCount % 16u == 0u,
                   "waterfall veil must fill complete lane rows");
    const unsigned column = i % columns;
    const unsigned row = i / columns;
    const float lane = ((float)column + 0.5f) / (float)columns;
    const float left = rect->x0 + f->Motion[1];
    const float width_span = rect->x1 - rect->x0 - f->Motion[2];
    const float y_span = rect->y1 - rect->y0 + f->Motion[3];
    const float phase = (HashUnit(seed ^ 0x29u) +
                         (float)visual_ticks / (f->Motion[4] + (float)row * f->Motion[5]));
    const float wrapped = phase - floorf(phase);
    x = left + width_span * lane + (HashUnit(seed ^ 0x53u) - 0.5f) * f->Motion[6];
    y = rect->y0 - f->Motion[7] + y_span * wrapped;
    previous_x = x + (HashUnit(seed ^ 0x37u) - 0.5f) * f->Motion[8];
    previous_y = y - (f->Motion[9] + f->Motion[10] * HashUnit(seed ^ 0xB5u));
    width = f->Shape[0] + f->Shape[1] * HashUnit(seed ^ 0x71u);
    reach = f->Shape[2] + f->Shape[3] * HashUnit(seed ^ 0xA7u);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWaterfallLighting(ActionEffectGeometryWriter *writer,
                                    const ActionEffectInstance *effect, const ActionSurfaceField *f,
                                    ActionEffectProjectPointFn project_point,
                                    void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = SurfacePulse(effect,f);
  const float half_width=(rect->x1-rect->x0)*0.5f,half_height=(rect->y1-rect->y0)*0.5f;
  ActionEffectGlowStyle spill=SurfaceStyle(f,0,half_width,half_height,effect->generation);
  ActionEffectGlowStyle body=SurfaceStyle(f,1,half_width,half_height,effect->pulse_generation);
  const float spill_x=mid_x+f->Spill[31],body_x=mid_x+f->Body[31];
  const float spill_y=mid_y+f->Spill[32],body_y=mid_y+f->Body[32];
  if (!AppendGlowAtTicks(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata,(unsigned)f->Clock[0]*effect->pulse_ticks))
    return false;
  if (!AppendGlowAtTicks(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata, (unsigned)f->Clock[0] * effect->pulse_ticks))
    return false;
  return true;
}

static bool AppendWaterfallMistParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionSurfaceField *f,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot={f->Hot[0],f->Hot[1],f->Hot[2],f->Hot[3]};
  const ArRenderColorF cool={f->Cool[0],f->Cool[1],f->Cool[2],f->Cool[3]};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width, reach = f->Shape[2] + f->Shape[3] * t;
    /* Foam boils along the lower waterfall edge while lighter droplets
     * drift upward into the fog banks. Horizontal phase offsets avoid a
     * static bright seam over the gap. */
    const float span = rect->x1 - rect->x0;
    const float lane = ((float)i + HashUnit(seed ^ 0x53u)) / (float)count;
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * f->Motion[0];
    const float base_y = f->Motion[1] + (HashUnit(seed ^ 0xB5u) - 0.5f) * f->Motion[2];
    x = rect->x0 + span * lane + drift * t;
    y = base_y - f->Motion[3] * t - f->Motion[4] * t * t;
    previous_x = rect->x0 + span * lane + drift * previous_t;
    previous_y = base_y - f->Motion[3] * previous_t - f->Motion[4] * previous_t * previous_t;
    width = f->Shape[0] + f->Shape[1] * (1.0f - t);
    reach = f->Shape[2] + f->Shape[3] * t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWallTorchParticles(ActionEffectGeometryWriter *writer,
                                     const ActionEffectInstance *effect, const ActionGlowField *f,
                                     ActionEffectProjectPointFn project_point,
                                     void *userdata) {
  if (!((unsigned)f->Components[0]&2)) return true;
  const unsigned count = (unsigned)f->Particles[0];
  const ArRenderColorF hot = {f->ParticleHot[0],f->ParticleHot[1],f->ParticleHot[2],f->ParticleHot[3]};
  const ArRenderColorF cool = {f->ParticleCool[0],f->ParticleCool[1],f->ParticleCool[2],f->ParticleCool[3]};
  const unsigned visual_ticks =
      (unsigned)f->Clock[0] * (unsigned)effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = f->ParticleShape[2] + f->ParticleShape[3] * t;
    const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * f->ParticleMotion[0];
    const float birth = (HashUnit(seed ^ 0x71u) - 0.5f) * f->ParticleMotion[1];
    x = birth + drift * t * t;
    y = f->ParticleMotion[2] + f->ParticleMotion[3] * t + f->ParticleMotion[4] * t * t;
    previous_x = birth + drift * previous_t * previous_t;
    previous_y = f->ParticleMotion[2] + f->ParticleMotion[3] * previous_t + f->ParticleMotion[4] * previous_t * previous_t;
    width = f->ParticleShape[0] + f->ParticleShape[1] * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendWallTorchLighting(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionGlowField *f,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!((unsigned)f->Components[0]&1)) return true;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const unsigned ticks=(unsigned)f->Clock[0]*effect->phase_ticks;
  const unsigned glow_ticks=(unsigned)f->Clock[0]*effect->pulse_ticks;
  const unsigned seed=(unsigned)f->Clock[1]*effect->visual;
  const float pulse=f->Pulse[0]+f->Pulse[1]*TriangleWave(ticks+seed,(unsigned)f->Clock[2])+
      f->Pulse[2]*TriangleWave(ticks+seed*(unsigned)f->Clock[4],(unsigned)f->Clock[3]);
  ActionEffectGlowStyle spill=f->spill,body=f->body;
  if (effect->tuning.active) {
    spill.radius_x *= effect->tuning.reach;
    spill.radius_y *= effect->tuning.reach;
  }
  spill.seed=effect->generation;body.seed=effect->pulse_generation;
  return AppendGlowAtTicks(writer,effect,&spill,pulse,mid_x,mid_y,project_point,userdata,glow_ticks)&&
      AppendGlowAtTicks(writer,effect,&body,pulse,mid_x+f->BodyOffset[0],f->BodyOffset[1],project_point,userdata,glow_ticks);
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
                                    const ActionEffectInstance *effect,const ActionProjectileField *f,
                                    ActionEffectProjectPointFn project_point,
                                    void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot={f->Hot[0],f->Hot[1],f->Hot[2],f->Hot[3]},cool={f->Cool[0],f->Cool[1],f->Cool[2],f->Cool[3]};
  const unsigned visual_ticks =
      (unsigned)f->Clock[0]*effect->pulse_ticks;
  float heading_x = 1.0f, heading_y = 0.0f;
  SceneFireballHeading(effect,f, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    const float side = (HashUnit(seed ^ 0x53u) - 0.5f) * (f->Motion[2] + f->Motion[3] * t);
    /* Start beyond the 16px source art instead of hiding the youngest
     * sparks inside its painted red tail. The longer cone makes the host
     * enhancement legible without bleaching the authentic projectile. */
    const float wake=f->Motion[1];
    const float distance = f->Motion[0] + wake*t;
    const float previous_distance = f->Motion[0] + wake*previous_t;
    x = -heading_x * distance - heading_y * side;
    y = -heading_y * distance + heading_x * side;
    previous_x = -heading_x * previous_distance - heading_y * side;
    previous_y = -heading_y * previous_distance + heading_x * side;
    width = f->Shape[0] + f->Shape[1] * (1.0f - t);
    reach = f->Shape[2]+f->Shape[3]*t;

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendFireballLighting(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,const ActionProjectileField *f,
    ActionEffectProjectPointFn project_point,void *userdata){
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *r=&effect->geometry.data.rect;
  const float x=(r->x0+r->x1)*.5f,y=(r->y0+r->y1)*.5f;
  const unsigned ticks=(unsigned)f->Clock[0]*effect->phase_ticks,seed=(unsigned)f->Clock[1]*effect->visual;
  const float pulse=f->Pulse[0]+f->Pulse[1]*TriangleWave(ticks+seed,(unsigned)f->Clock[2])+
      f->Pulse[2]*TriangleWave(ticks+seed*(unsigned)f->Clock[4],(unsigned)f->Clock[3]);
  float hx,hy;SceneFireballHeading(effect,f,&hx,&hy);
  ActionEffectGlowStyle spill=f->spill,body=f->body;
  spill.axis_x=body.axis_x=hx;spill.axis_y=body.axis_y=hy;
  spill.lift_x=body.lift_x=-hx;spill.lift_y=body.lift_y=-hy;
  spill.seed=effect->record_address;body.seed=effect->pulse_generation;
  const float body_x=x+hx*f->BodyOffset[0]-hy*f->BodyOffset[1];
  const float body_y=y+hy*f->BodyOffset[0]+hx*f->BodyOffset[1];
  const unsigned glow_ticks=(unsigned)f->Clock[0]*effect->pulse_ticks;
  return AppendGlowAtTicks(writer,effect,&spill,pulse,x,y,project_point,userdata,glow_ticks)&&
      AppendGlowAtTicks(writer,effect,&body,pulse,body_x,body_y,project_point,userdata,glow_ticks);
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

/* These dispatchers select complete family operations. Palettes, motion,
 * timing and phase-specific geometry stay together above. */
static bool AppendSceneParticles(ActionEffectGeometryWriter *writer,
                                 const ActionEffectInstance *effect,
                                 const ActionRayField *ray_field, const ActionWaterField *water_field, const ActionAtmosphereField *atmosphere_field, const ActionMoonField *moon_field, const ActionMarshField *marsh_field, const ActionGlowField *glow_field, const ActionArcField *arc_field, const ActionProjectileField *projectile_field, const ActionSurfaceField *surface_field, const ActionNativeMembers *members,
                                 ActionEffectProjectPointFn project_point,
                                 ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  switch (effect->kind) {
  case kActionEffect_BloodpoolWater:
  case kActionEffect_BloodpoolMist:
  case kActionEffect_BloodpoolMoonReflection:
    return AppendBloodpoolEnvironment(moon_field, marsh_field, writer, effect, project_point, clip_bounds, userdata);
  case kActionEffect_CaveMist:
  case kActionEffect_TempleGrit:
  case kActionEffect_TempleGroundMist:
  case kActionEffect_CaveWater:
  case kActionEffect_CaveDrips:
  case kActionEffect_TempleDust:
  case kActionEffect_LandingDust:
    return AppendCaveEnvironment(writer, effect, water_field, atmosphere_field, members, project_point, clip_bounds, userdata);
  case kActionEffect_ForestLeaves:
    return AppendForestLeaves(writer, effect, ray_field, members, project_point, userdata);
  case kActionEffect_ForestCanopyLight:
    return AppendForestMotes(writer, effect, ray_field, members, project_point, userdata);
  case kActionEffect_AitosLavaPit:
    return AppendLavaPitParticles(writer, effect, surface_field, project_point, userdata);
  case kActionEffect_AitosLavaReservoir:
    return AppendLavaReservoirParticles(writer, effect, surface_field, project_point,
                                        userdata);
  case kActionEffect_AitosMoltenRock:
    return AppendMoltenRockParticles(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterSplash:
    return AppendWaterSplashParticles(writer, effect, surface_field, project_point, userdata);
  case kActionEffect_AitosWaterfall:
    return AppendWaterfallParticles(writer, effect, surface_field, project_point, userdata);
  case kActionEffect_AitosWaterfallMist:
    return AppendWaterfallMistParticles(writer, effect, surface_field, project_point,
                                        userdata);
  case kActionEffect_WallTorch:
    return AppendWallTorchParticles(writer, effect, glow_field?glow_field:ActionGlowField_Bundled(2,3), project_point, userdata);
  case kActionEffect_AitosStatueFire:
    return AppendStatueFireParticles(writer, effect, project_point, userdata);
  case kActionEffect_EnemyFireball:
  case kActionEffect_FillmoreStatueOrb:
  case kActionEffect_MarahnaFireball:
  case kActionEffect_AitosLavaFireball:
    return AppendFireballParticles(writer, effect, projectile_field, project_point, userdata);
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
    return AppendLightningTrapParticles(writer, effect, arc_field, project_point,
                                        userdata);
  case kActionEffect_NorthwallBossMagic:
    return AppendNorthwallMagicParticles(writer, effect, project_point, userdata);
  case kActionEffect_CentaurLightning:
  case kActionEffect_BloodpoolBossLightning:
    return AppendBossLightningParticles(writer, effect, arc_field, project_point, userdata);
  default:
    return true;
  }
}

static bool AppendSceneLighting(ActionEffectGeometryWriter *writer,
                                const ActionEffectInstance *effect,
                                const ActionRayField *ray_field, const ActionWaterField *water_field, const ActionAtmosphereField *atmosphere_field, const ActionGlowField *glow_field, const ActionArcField *arc_field, const ActionProjectileField *projectile_field, const ActionSurfaceField *surface_field, const ActionNativeMembers *members,
                                ActionEffectProjectPointFn project_point,
                                ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  switch (effect->kind) {
  case kActionEffect_CaveSheen:
  case kActionEffect_CaveAmbientLight:
  case kActionEffect_TowerWindowLight:
    return AppendCaveEnvironment(writer, effect, water_field, atmosphere_field, members, project_point, clip_bounds, userdata);
  case kActionEffect_ForestForwardLight:
    return AppendForestRays(writer, effect, ray_field, members, true, project_point, clip_bounds, userdata);
  case kActionEffect_ForestCanopyLight:
    return AppendForestRays(writer, effect, ray_field, members, false, project_point, clip_bounds, userdata);
  case kActionEffect_AitosLavaReservoir:
    return AppendLavaReservoirLighting(
        writer, effect, surface_field, SurfacePulse(effect,surface_field), project_point, userdata);
  case kActionEffect_WallTorch:
    return AppendWallTorchLighting(writer, effect, glow_field?glow_field:ActionGlowField_Bundled(2,3), project_point, userdata);
  case kActionEffect_AitosStatueFire:
    return AppendStatueFireLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosLavaPit:
    return AppendLavaPitLighting(writer, effect, surface_field, project_point, userdata);
  case kActionEffect_AitosMoltenRock:
    return AppendMoltenRockLighting(writer, effect, project_point, userdata);
  case kActionEffect_AitosWaterSplash:
    return AppendWaterSplashLighting(writer, effect, surface_field, project_point, userdata);
  case kActionEffect_AitosWaterfall:
    return AppendWaterfallLighting(writer, effect, surface_field, project_point, userdata);
  case kActionEffect_AitosWaterfallMist:
    return AppendWaterfallMistCloudVolume(
        writer, effect, surface_field, SurfacePulse(effect,surface_field), project_point, userdata);
  case kActionEffect_EnemyFireball:
  case kActionEffect_FillmoreStatueOrb:
  case kActionEffect_MarahnaFireball:
  case kActionEffect_AitosLavaFireball:
    return AppendFireballLighting(writer, effect, projectile_field, project_point, userdata);
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
    return AppendLightningTrapLighting(writer, effect, arc_field, project_point, userdata);
  case kActionEffect_NorthwallBossMagic:
    return AppendNorthwallMagicLighting(writer, effect, project_point, userdata);
  case kActionEffect_CentaurLightning:
  case kActionEffect_BloodpoolBossLightning:
    return AppendBossLightningLighting(writer, effect, arc_field, project_point, userdata);
  default:
    return true;
  }
}

static bool SceneEffectStyleKnown(const ActionEffectInstance *effect) {
  if (!effect) return false;
  switch (effect->kind) {
    case kActionEffect_CastleWater:
      return effect->phase == kActionEffectPhase_CastleEnvironment && effect->environment_room == 5 &&
          effect->render_layer == kActionEffectRenderLayer_Bg1Plane &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_CastleLight:
    case kActionEffect_CastleSky:
    case kActionEffect_CastleMist:
      return effect->phase == kActionEffectPhase_CastleEnvironment &&
          effect->environment_room >= 2 && effect->environment_room <= 8 &&
          effect->render_layer == (effect->kind == kActionEffect_CastleLight ?
              kActionEffectRenderLayer_Bg1Plane : effect->kind == kActionEffect_CastleSky ?
              kActionEffectRenderLayer_Bg2Plane : kActionEffectRenderLayer_Bg1Mist) &&
          effect->projection_plane == (effect->kind == kActionEffect_CastleSky ?
              kActionEffectProjectionPlane_Bg2 : kActionEffectProjectionPlane_Bg1);
    case kActionEffect_BloodpoolTimber:
    case kActionEffect_BloodpoolAir:
    case kActionEffect_BloodpoolCloud:
      return effect->phase == kActionEffectPhase_BloodpoolEnvironment &&
          effect->render_layer == (effect->kind == kActionEffect_BloodpoolTimber ?
              kActionEffectRenderLayer_Bg1Plane : effect->kind == kActionEffect_BloodpoolAir ?
              kActionEffectRenderLayer_Bg2HighAlpha : kActionEffectRenderLayer_Bg2Alpha) &&
          effect->projection_plane == (effect->kind == kActionEffect_BloodpoolCloud ?
              kActionEffectProjectionPlane_Bg2 : kActionEffectProjectionPlane_Bg1);
    case kActionEffect_BloodpoolMoonlight:
    case kActionEffect_BloodpoolMoonReflection:
      return effect->phase == kActionEffectPhase_BloodpoolEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_Bg2Plane &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg2;
    case kActionEffect_BloodpoolWater:
    case kActionEffect_BloodpoolMist:
      return effect->phase == kActionEffectPhase_BloodpoolEnvironment &&
          effect->render_layer == (effect->kind == kActionEffect_BloodpoolWater ?
              kActionEffectRenderLayer_Bg1HighPlane :
              kActionEffectRenderLayer_Bg2HighAlpha) &&
          effect->projection_plane == (effect->kind == kActionEffect_BloodpoolWater ?
              kActionEffectProjectionPlane_Bg1High : kActionEffectProjectionPlane_Bg1);
    case kActionEffect_LandingDust:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->dust_strength >= 1 && effect->dust_strength <= 3 &&
          effect->phase_ticks < kActionLandingDustLifetime &&
          effect->render_layer == kActionEffectRenderLayer_WorldDust &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_CaveMist:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_WorldDust &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg2High;
    case kActionEffect_CaveSheen:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_Bg1Plane &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_TempleGroundMist:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_Bg1Mist &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1 &&
          effect->geometry.data.rect.x0 == 0 && effect->geometry.data.rect.x1 >= 16 &&
          effect->geometry.data.rect.x1 <= 880 &&
          effect->geometry.data.rect.y0 <= -1 && effect->geometry.data.rect.y0 >= -64 && effect->geometry.data.rect.y1 == 0;
    case kActionEffect_CaveAmbientLight:
    case kActionEffect_TempleGrit:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == (effect->kind == kActionEffect_CaveAmbientLight ?
              kActionEffectRenderLayer_ForegroundLight : kActionEffectRenderLayer_WorldDust) &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_CaveWater:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_Bg2HighPlane &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg2High;
    case kActionEffect_CaveDrips:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_WorldOverlay &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_TempleDust:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_WorldOverlay &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_TowerWindowLight:
      return effect->phase == kActionEffectPhase_CaveEnvironment &&
          effect->render_layer == kActionEffectRenderLayer_ForegroundLight &&
          effect->projection_plane == kActionEffectProjectionPlane_Bg1;
    case kActionEffect_ForestForwardLight:
      return effect->phase == kActionEffectPhase_ForestCanopyLight &&
          effect->render_layer == kActionEffectRenderLayer_ForegroundLight &&
          effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds;
    case kActionEffect_ForestLeaves:
      return effect->phase == kActionEffectPhase_ForestCanopyLight &&
          effect->render_layer == kActionEffectRenderLayer_Bg2Alpha &&
          effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds;
    case kActionEffect_ForestCanopyLight:
      return effect->phase == kActionEffectPhase_ForestCanopyLight &&
          effect->render_layer == kActionEffectRenderLayer_Bg2Plane &&
          effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds &&
          effect->geometry.data.rect.x1 - effect->geometry.data.rect.x0 <= 768 &&
          effect->geometry.data.rect.y1 - effect->geometry.data.rect.y0 <= 544;
    case kActionEffect_WallTorch:
      return effect->phase == kActionEffectPhase_WallTorch;
    case kActionEffect_EnemyFireball:
      return effect->phase == kActionEffectPhase_EnemyFireballFlight;
    case kActionEffect_FillmoreStatueOrb:
      return effect->phase == kActionEffectPhase_EnemyFireballFlight &&
          effect->visual >= 0x1B && effect->visual <= 0x1E;
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

/* Family membership is explicit: inserting an enum value must not silently
 * change duplicate budgets or select a different stage renderer. Multi-instance
 * dust and floor spans retain their own limits below. */
typedef enum SceneEnvironmentFamily {
  kSceneEnvironment_None, kSceneEnvironment_Cave,
  kSceneEnvironment_Bloodpool, kSceneEnvironment_Castle,
} SceneEnvironmentFamily;

static SceneEnvironmentFamily SceneEnvironmentFamilyFor(uint8_t kind) {
  switch (kind) {
    case kActionEffect_CaveWater: case kActionEffect_CaveDrips:
    case kActionEffect_TempleDust: case kActionEffect_TowerWindowLight:
    case kActionEffect_CaveMist: case kActionEffect_CaveSheen:
    case kActionEffect_CaveAmbientLight: case kActionEffect_TempleGrit:
      return kSceneEnvironment_Cave;
    case kActionEffect_BloodpoolWater: case kActionEffect_BloodpoolMist:
    case kActionEffect_BloodpoolMoonlight: case kActionEffect_BloodpoolMoonReflection:
    case kActionEffect_BloodpoolTimber: case kActionEffect_BloodpoolAir:
    case kActionEffect_BloodpoolCloud:
      return kSceneEnvironment_Bloodpool;
    case kActionEffect_CastleLight: case kActionEffect_CastleSky:
    case kActionEffect_CastleMist: case kActionEffect_CastleWater:
      return kSceneEnvironment_Castle;
    default: return kSceneEnvironment_None;
  }
}

static bool
BuildSceneEffectList(const ActionEffectInstance *effects, uint8_t effect_count,
                     const ActionMoonlightOcclusion *moonlight,
                     const ActionBloodpoolDetails *bloodpool, const ActionRayField *ray_field, const ActionWaterField *water_field, const ActionAtmosphereField *atmosphere_field, const ActionMoonField *moon_field, const ActionCastleField *castle_field, const ActionGlowField *glow_field, const ActionSceneEffectFrame *field_frame, const ActionNativeMembers *members,
                     const ActionMoonlightOcclusion *scenery, uint8_t capacity, bool overflow,
                     bool append, uint8_t render_layer, bool want_lighting, bool want_particles,
                     ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
                     void *project_userdata, ActionSceneEffectRenderBatch *batch) {
  if (!batch) return false;
  /* Submitters consume only [0, count). The scene capacity is deliberately
   * large, so zeroing its unused tail would touch hundreds of KiB per build. */
  if (!append) batch->vertex_count = batch->index_count = 0;
  if (!effects || effect_count > capacity ||
      render_layer >= kActionEffectRenderLayer_Count)
    return false;
  if (overflow || (!want_lighting && !want_particles)) return true;
  if (!project_point) return false;
  ActionEffectGeometryWriter writer = GeometryWriter(
      batch->vertices, kActionSceneEffectRenderMaxVertices,
      batch->indices, kActionSceneEffectRenderMaxIndices);
  if (project_point == ActionEffectSource_ProjectPoint) writer.source = project_userdata;
  writer.vertex_count = batch->vertex_count;
  writer.index_count = batch->index_count;
  const ActionMarshField *marsh_field=bloodpool&&bloodpool->field_valid?&bloodpool->field:NULL;
  ActionSceneryShadow shadow = {0};
  bool shadow_prepared = false;
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
  bool environment_seen[kActionEffect_KindCount] = {0};
  const ActionEffectInstance *moon = NULL;
  bool moon_resolved = false;
  unsigned landing_puffs = 0;
  unsigned temple_mist_spans = 0;

  for (uint8_t i = 0; i < effect_count; i++) {
    const ActionEffectInstance *effect = &effects[i];
    const int surface_index=ActionSurfaceField_Index(effect->kind);
    const ActionSurfaceField *surface_field=surface_index<0?NULL:
      (field_frame->surface_fields_valid&(1u<<surface_index))?&field_frame->surface_fields[surface_index]:ActionSurfaceField_Bundled((unsigned)surface_index);
    if(surface_index>=0&&!surface_field)return false;
    const bool lighting_enabled=want_lighting && !(effect->flags&kActionEffectFlag_LightingOff) && ActionEffectReceivers_Layer(effect)==render_layer &&
        (!effect->tuning.light_receivers_set || !ActionEffectReceivers_IsLight(effect->kind) ||
         (effect->tuning.light_receivers&kActionReceiver_Scenery));
    const bool particles_enabled=want_particles && effect->render_layer==render_layer;
    const int first_vertex = writer.vertex_count;
    if (!(effect->flags & kActionEffectFlag_Visible) ||
        effect->geometry.kind != kActionEffectGeometry_Rect ||
        !RectIsSane(&effect->geometry.data.rect) ||
        !SceneEffectStyleKnown(effect) ||
        effect->obj_priority >= kActionEffectObjPriorityCount ||
        (!lighting_enabled && !particles_enabled) ||
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
      const unsigned segments = LavaReservoirGlowSegmentCount(effect,surface_field);
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
    const SceneEnvironmentFamily family = SceneEnvironmentFamilyFor(effect->kind);
    if(family==kSceneEnvironment_Bloodpool) {
      if(!moon_field)moon_field=ActionMoonField_Bundled();
      if(!marsh_field)marsh_field=ActionMarshField_Bundled();
    }
    if (family != kSceneEnvironment_None) {
      if (environment_seen[effect->kind]) return false;
      environment_seen[effect->kind] = true;
    }
    if (!writer.source && lighting_enabled && effect->kind == kActionEffect_ForestForwardLight &&
        !shadow_prepared) {
      if (!ActionSceneryShadow_Prepare(&shadow, &batch->shadow, scenery, project_point,
                                       clip_bounds, project_userdata))
        return false;
      writer.shadow = &shadow;
      shadow_prepared = true;
    }
    if (family == kSceneEnvironment_Castle &&
        !AppendCastleEnvironment(castle_field?castle_field:ActionCastleField_Bundled(effect->environment_room), &writer, effect, members, lighting_enabled, particles_enabled,
                                 project_point, clip_bounds, project_userdata))
      return false;
    if (effect->kind == kActionEffect_LandingDust &&
        ++landing_puffs > kActionLandingDustMaxPuffs)
      return false;
    if (effect->kind == kActionEffect_TempleGroundMist) {
      if (++temple_mist_spans > kActionTempleMistMaxSpans) return false;
      for (unsigned j = 0; j < i; j++)
        if (effects[j].kind == effect->kind && effects[j].generation == effect->generation)
          return false;
    }
    if (lighting_enabled &&
        effect->kind == kActionEffect_BloodpoolMoonlight &&
        !AppendBloodpoolMoonlight(moon_field, &writer,effect,moonlight,&batch->moonlight,project_point,
            clip_bounds,project_userdata))
      return false;
    if (family == kSceneEnvironment_Bloodpool) {
      /* Resolve once, lazily: unrelated passes must not fail because a moon
       * they never consume is malformed or duplicated. */
      if (!moon_resolved) {
        for (unsigned j = 0; j < effect_count; j++) {
          if (effects[j].kind != kActionEffect_BloodpoolMoonlight ||
              !(effects[j].flags & kActionEffectFlag_Visible) ||
              effects[j].geometry.kind != kActionEffectGeometry_Rect ||
              !RectIsSane(&effects[j].geometry.data.rect) ||
              !SceneEffectStyleKnown(&effects[j])) continue;
          if (moon) return false;
          moon = &effects[j];
        }
        moon_resolved = true;
      }
      if (lighting_enabled && effect->kind == kActionEffect_BloodpoolWater &&
          !AppendBloodpoolWaterMoonlight(moon_field, marsh_field, &writer,effect,moon,moonlight,&batch->moonlight,
              project_point,clip_bounds,project_userdata)) return false;
      if (lighting_enabled && effect->kind == kActionEffect_BloodpoolTimber &&
          !AppendBloodpoolTimberMoonlight(moon_field, marsh_field, &writer,effect,moon,bloodpool,moonlight,&batch->moonlight,
              project_point,clip_bounds,project_userdata)) return false;
      if (lighting_enabled && effect->kind == kActionEffect_BloodpoolCloud &&
          !AppendBloodpoolCloud(moon_field, &writer,effect,project_point,clip_bounds,project_userdata))
        return false;
      if (particles_enabled && effect->kind == kActionEffect_BloodpoolMoonReflection &&
          !AppendBloodpoolWaveCaps(moon_field, &writer,effect,bloodpool,project_point,clip_bounds,
              project_userdata)) return false;
      if (particles_enabled &&
          !AppendBloodpoolDetailParticles(moon_field, marsh_field, &writer,effect,moon,bloodpool,
              project_point,clip_bounds,project_userdata)) return false;
    }
    const int projectile_index=ActionProjectileField_Index(effect->kind);
    const ActionProjectileField *projectile_field=projectile_index<0?NULL:
      (field_frame->projectile_fields_valid&(1u<<projectile_index))?&field_frame->projectile_fields[projectile_index]:ActionProjectileField_Bundled((unsigned)projectile_index);
    const int arc_index=ActionArcField_Index(effect->kind);
    const ActionArcField *arc_field=arc_index<0?NULL:
      (field_frame->arc_fields_valid&(1u<<arc_index))?&field_frame->arc_fields[arc_index]:ActionArcField_Bundled((unsigned)arc_index);
    if (lighting_enabled && !AppendSceneLighting(&writer, effect, ray_field, water_field, atmosphere_field, glow_field, arc_field, projectile_field, surface_field, members, project_point,
                                                 clip_bounds, project_userdata))
      return false;
    if (particles_enabled && !AppendSceneParticles(&writer, effect, ray_field, water_field, atmosphere_field, moon_field, marsh_field, glow_field, arc_field, projectile_field, surface_field, members, project_point,
                                                   clip_bounds, project_userdata))
      return false;
    if (effect->tuning.active) {
      const ActionEffectTuning *t = &effect->tuning;
      if (!isfinite(t->intensity) || t->intensity < 0 || t->intensity > 4 ||
          !isfinite(t->reach) || t->reach < .25f || t->reach > 4) return false;
      const float red = ((t->color >> 16) & 255) / 255.0f;
      const float green = ((t->color >> 8) & 255) / 255.0f;
      const float blue = (t->color & 255) / 255.0f;
      if (writer.source) ActionEffectSource_Tint(writer.source, first_vertex, t->color, t->intensity, false);
      else for (int v = first_vertex; v < writer.vertex_count; ++v) {
        ArRenderColorF *c = &writer.vertices[v].color;
        c->r *= red; c->g *= green; c->b *= blue;
        c->a = fminf(1,c->a * t->intensity);
      }
    }
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
  return BuildSceneEffectList(frame->effects, frame->effect_count, NULL, NULL, frame->ray_field_valid?&frame->ray_field:NULL, frame->water_field_valid?&frame->water_field:NULL, frame->atmosphere_field_valid?&frame->atmosphere_field:NULL, frame->moon_field_valid?&frame->moon_field:NULL, frame->castle_field_valid?&frame->castle_field:NULL, frame->glow_field_valid?&frame->glow_field:NULL, frame, &frame->members,
                              &frame->scenery, kActionSceneEffectMaxInstances, frame->overflow,
                              false, kActionEffectRenderLayer_WorldOverlay, lighting_enabled,
                              particles_enabled, project_point, NULL, project_userdata, batch);
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
  if (!BuildSceneEffectList(frame->decorations, frame->decoration_count, &frame->moonlight,
                            &frame->bloodpool, frame->ray_field_valid?&frame->ray_field:NULL, frame->water_field_valid?&frame->water_field:NULL, frame->atmosphere_field_valid?&frame->atmosphere_field:NULL, frame->moon_field_valid?&frame->moon_field:NULL, frame->castle_field_valid?&frame->castle_field:NULL, frame->glow_field_valid?&frame->glow_field:NULL, frame, &frame->members, &frame->scenery,
                            kActionSceneDecorationMaxInstances, frame->decoration_overflow, false,
                            render_layer, lighting_enabled, particles_enabled, project_point,
                            clip_bounds, project_userdata, batch))
    return false;
  /* Append actor light directly to the caller's retained batch. Each caller
   * owns its geometry, so game-thread receiver sampling and render-thread
   * presentation cannot race through a shared actor scratch buffer. */
  if (render_layer == kActionEffectRenderLayer_Bg1Plane && lighting_enabled &&
      !BuildSceneEffectList(frame->effects, frame->effect_count, NULL, NULL, frame->ray_field_valid?&frame->ray_field:NULL, frame->water_field_valid?&frame->water_field:NULL, frame->atmosphere_field_valid?&frame->atmosphere_field:NULL, frame->moon_field_valid?&frame->moon_field:NULL, frame->castle_field_valid?&frame->castle_field:NULL, frame->glow_field_valid?&frame->glow_field:NULL, frame, &frame->members,
                            &frame->scenery, kActionSceneEffectMaxInstances, frame->overflow, true,
                            render_layer, true, false, project_point, clip_bounds, project_userdata,
                            batch)) {
    batch->vertex_count = batch->index_count = 0;
    return false;
  }
  if (frame->authored_count > kActionAuthoredMaxInstances) {
    batch->vertex_count = batch->index_count = 0;
    return false;
  }
  ActionEffectGeometryWriter writer = GeometryWriter(batch->vertices,kActionSceneEffectRenderMaxVertices,
      batch->indices,kActionSceneEffectRenderMaxIndices);
  if (project_point == ActionEffectSource_ProjectPoint) writer.source = project_userdata;
  writer.vertex_count=batch->vertex_count;writer.index_count=batch->index_count;
  const int base_vertices=writer.vertex_count, base_indices=writer.index_count;
  ActionSceneryShadow shadow = {0};
  bool shadow_prepared = false;
  for (unsigned i=0;i<frame->authored_count;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    const bool light=lighting_enabled&&ActionEffectReceivers_Layer(e)==render_layer&&
      (!e->tuning.light_receivers_set||!ActionEffectReceivers_IsLight(e->kind)||(e->tuning.light_receivers&kActionReceiver_Scenery));
    const bool particles=particles_enabled&&e->render_layer==render_layer;
    if(!(e->flags&kActionEffectFlag_Visible)||(!light&&!particles))continue;
    if (!writer.source && e->kind == kActionEffect_AuthoredFan && light && !shadow_prepared) {
      if (!ActionSceneryShadow_Prepare(&shadow, &batch->shadow, &frame->scenery,
                                       project_point, clip_bounds, project_userdata))
        return false;
      writer.shadow = &shadow;
      shadow_prepared = true;
    }
    bool ok;
    if(e->kind==kActionEffect_AuthoredTorch) {
      const ActionGlowField *field=frame->glow_field_valid?&frame->glow_field:ActionGlowField_Bundled(2,3);
      ActionEffectInstance torch=*e;torch.kind=kActionEffect_WallTorch;torch.phase=kActionEffectPhase_WallTorch;
      torch.geometry.data.rect=(ActionEffectLocalRect){field->Geometry[0],field->Geometry[1],field->Geometry[2],field->Geometry[3]};
      const int begin=writer.vertex_count;
      ok=(!light||AppendWallTorchLighting(&writer,&torch,field,project_point,project_userdata))&&
         (!particles||AppendWallTorchParticles(&writer,&torch,field,project_point,project_userdata));
      if (writer.source) ActionEffectSource_Tint(writer.source, begin, e->tuning.color, e->tuning.intensity, false);
      else for(int v=begin;v<writer.vertex_count;++v){ArRenderColorF *c=&writer.vertices[v].color;
        c->r*=((e->tuning.color>>16)&255)/255.f;c->g*=((e->tuning.color>>8)&255)/255.f;c->b*=(e->tuning.color&255)/255.f;
        c->a=fminf(1,c->a*e->tuning.intensity);}
    }else ok=AppendAuthoredEnvironment(&writer,e,&frame->authored_floor[i],light,particles,project_point,clip_bounds,project_userdata);
    if (!ok || (!writer.source &&
        (writer.vertex_count-base_vertices>kActionAuthoredMaxVertices ||
         writer.index_count-base_indices>kActionAuthoredMaxIndices))) {
      batch->vertex_count=batch->index_count=0;return false;
    }
  }
  batch->vertex_count=writer.vertex_count;batch->index_count=writer.index_count;
  return true;
}
