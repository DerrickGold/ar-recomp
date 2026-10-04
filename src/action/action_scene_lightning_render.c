/* ActionSceneLightningRender: lightning and beam effects in action scenes: the
 * projected ribbons behind boss, Marahna and trap lightning, the Bloodpool
 * and Centaur bosses' bolts, and sword-beam trails, each as particles plus
 * lighting.
 * Phase: pure.
 * Tests: tests/action_effect_render_test.c */
#include "action/action_effect_render_internal.h"

static ArRenderColorF ArcColor(const float *v){return (ArRenderColorF){v[0],v[1],v[2],v[3]};}
static float ArcPulse(const ActionEffectInstance *e,const ActionArcField *f){
  const unsigned ticks=(unsigned)f->Clock[0]*e->phase_ticks,seed=(unsigned)f->Clock[1]*e->visual;
  return f->Pulse[0]+f->Pulse[1]*TriangleWave(ticks+seed,(unsigned)f->Clock[2])+
    f->Pulse[2]*TriangleWave(ticks+seed*(unsigned)f->Clock[4],(unsigned)f->Clock[3]);
}
/* Native visuals select the observed pose. The response owns its path,
 * colors, radius and animation, independently of attack state/timing. */
static bool BossLightningPathFor(const ActionEffectInstance *effect,const ActionArcField *f,
    const float **path,unsigned *count){
  if(!effect||!f||!count)return false;
  unsigned index=effect->visual;
  if(effect->kind==kActionEffect_CentaurLightning){if(index<0x19||index>0x20)return false;index-=0x19;}
  else if(index>5)return false;
  *count=(unsigned)f->PathCounts[index];if(*count<2||*count>25)return false;
  if(path)*path=f->paths[index];return true;
}

static bool BossLightningPathPoint(const ActionEffectInstance *effect, const ActionArcField *f,
                                   unsigned joint, float *x, float *y) {
  const float *path_x = NULL;
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect,f, &path_x, &joint_count) ||
      joint >= joint_count || !x || !y)
    return false;
  *x = path_x[3+joint]+path_x[0];
  if (effect->flags & kActionEffectFlag_FlipHorizontal) *x = -*x;
  /* `$8D68`'s action-OBJ emitter stores Y with one extra draw-bias pixel
   * after the camera-origin bias cancels. Subtract it here so the filament
   * runs through the emitted tile centres, not one row below them. */
  *y = path_x[1]+(float)joint*path_x[2];
  return true;
}

static bool BossLightningPathSample(const ActionEffectInstance *effect, const ActionArcField *f,
                                    float along, float *x, float *y) {
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect,f, NULL, &joint_count) || !x || !y ||
      !isfinite(along))
    return false;
  along = fmaxf(0.0f, fminf(1.0f, along));
  const float scaled = along * (float)(joint_count - 1u);
  unsigned segment = (unsigned)scaled;
  if (segment >= joint_count - 1u)
    return BossLightningPathPoint(effect,f, joint_count - 1u, x, y);
  float x0, y0, x1, y1;
  if (!BossLightningPathPoint(effect,f, segment, &x0, &y0) ||
      !BossLightningPathPoint(effect,f, segment + 1u, &x1, &y1))
    return false;
  const float t = scaled - (float)segment;
  *x = x0 + (x1 - x0) * t;
  *y = y0 + (y1 - y0) * t;
  return true;
}

static bool AppendProjectedRibbonSegments(
    ActionEffectGeometryWriter *writer, const ArRenderPointF *points,
    const float *scales, unsigned joint_count, float half_width,
    ArRenderColorF color, float end_taper) {
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
    const float taper0 = i == 0 ? end_taper : 1.0f;
    const float taper1 = i + 1 == segment_count
        ? end_taper : 1.0f;
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
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionArcField *f,
    float half_width, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectLightningSegments + 1 };
  ArRenderPointF points[kJoints];
  float scales[kJoints];
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect,f, NULL, &joint_count)) return true;
  for (unsigned i = 0; i < joint_count; i++) {
    float local_x, local_y, scale_x, scale_y;
    if (!BossLightningPathPoint(effect,f, i, &local_x, &local_y)) return true;
    if (writer->source) { points[i] = (ArRenderPointF){local_x, local_y}; continue; }
    if (!ProjectWithScale(effect, project_point, userdata, local_x, local_y,
                          &points[i], &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  if (writer->source) {
    const bool ok = ActionEffectSource_Ribbon(writer->source, effect, points, joint_count,
        half_width, f->Ribbon[2], color);
    writer->vertex_count = (int)writer->source->count;
    return ok;
  }
  return AppendProjectedRibbonSegments(writer, points, scales, joint_count,
                                       half_width, color, f->Ribbon[2]);
}

static bool AppendBossLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionArcField *f,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->phase != kActionEffectPhase_BossLightningStrike) return true;
  const float pulse = ArcPulse(effect,f);
  ArRenderColorF corona=ArcColor(f->Corona);corona.a*=pulse;
  ArRenderColorF filament=ArcColor(f->Filament);filament.a*=pulse;
  return AppendBossLightningRibbonLayer(writer, effect, f, f->Ribbon[0], corona,
                                        project_point, userdata) &&
         AppendBossLightningRibbonLayer(writer, effect, f, f->Ribbon[1], filament,
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
    if (writer->source) { points[i] = (ArRenderPointF){x, y}; continue; }
    float scale_x, scale_y;
    if (!ProjectWithScale(effect, project_point, userdata, x, y, &points[i],
                          &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  if (writer->source) {
    const bool ok = ActionEffectSource_Ribbon(writer->source, effect, points, kJoints,
        half_width, .55f, color);
    writer->vertex_count = (int)writer->source->count;
    return ok;
  }
  return AppendProjectedRibbonSegments(writer, points, scales, kJoints,
                                       half_width, color, .55f);
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
    if (writer->source) { points[i] = (ArRenderPointF){x, y}; continue; }
    float scale_x, scale_y;
    if (!ProjectWithScale(effect, project_point, userdata, x, y, &points[i],
                          &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  if (writer->source) {
    const bool ok = ActionEffectSource_Ribbon(writer->source, effect, points, kJoints,
        half_width, .55f, color);
    writer->vertex_count = (int)writer->source->count;
    return ok;
  }
  return AppendProjectedRibbonSegments(writer, points, scales, kJoints,
                                       half_width, color, .55f);
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

typedef enum SwordBeamStrip {
  kSwordBeamStrip_Haze,
  kSwordBeamStrip_UpperWake,
  kSwordBeamStrip_LowerWake,
  kSwordBeamStrip_ArcCorona,
  kSwordBeamStrip_ArcCore,
  kSwordBeamStrip_Count,
} SwordBeamStrip;

_Static_assert((int)kSwordBeamStrip_Count == (int)kActionSceneEffectSwordStripLayers,
               "sword strip styles must match the geometry budget");
_Static_assert(kActionSceneEffectSwordStripAcross == 5,
               "sword strip feathering needs two edges, two shoulders and a centre");
_Static_assert(kActionEffectGlowSegments % kActionSceneEffectSwordMoteSegments == 0,
               "sword motes must sample evenly around the circle table");

static float SwordSmoothStep(float t) {
  t = fmaxf(0.0f, fminf(1.0f, t));
  return t * t * (3.0f - 2.0f * t);
}

static float SwordBeamPulse(const ActionEffectInstance *effect) {
  return 0.94f + 0.06f * sinf((float)effect->age_ticks * 0.21f);
}

/* Keep the same local mesh in the reference and resident-GPU paths. All edges
 * are transparent; additive strips must never expose a solid quad boundary. */
static bool AppendSwordMesh(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ArRenderVertex2D *vertices, unsigned vertex_count, const int *indices,
    unsigned index_count, ActionEffectProjectPointFn project, void *userdata) {
  if (writer->source) {
    ActionEffectLocalRect bounds = {vertices[0].position.x, vertices[0].position.y,
                                   vertices[0].position.x, vertices[0].position.y};
    for (unsigned i = 1; i < vertex_count; ++i) {
      bounds.x0 = fminf(bounds.x0, vertices[i].position.x);
      bounds.y0 = fminf(bounds.y0, vertices[i].position.y);
      bounds.x1 = fmaxf(bounds.x1, vertices[i].position.x);
      bounds.y1 = fmaxf(bounds.y1, vertices[i].position.y);
    }
    for (unsigned i = 0; i < index_count; i += 3)
      if (!ActionEffectSource_Triangle(writer->source, effect, vertices,
                                       indices + i, &bounds)) return false;
    writer->vertex_count = (int)writer->source->count;
    return true;
  }
  for (unsigned i = 0; i < vertex_count; ++i)
    if (!project(userdata, effect, vertices[i].position.x, vertices[i].position.y,
                 &vertices[i].position)) return true;
  if (!Reserve(writer, (int)vertex_count, (int)index_count)) return false;
  const int base = writer->vertex_count;
  memcpy(writer->vertices + base, vertices, vertex_count * sizeof(*vertices));
  writer->vertex_count += (int)vertex_count;
  for (unsigned i = 0; i < index_count; ++i)
    writer->indices[writer->index_count++] = base + indices[i];
  return true;
}

/* Three flowing wake ribbons, then a corona/core pair wrapped around the
 * native crescent. Five samples across each strip feather both edges. */
static bool AppendSwordBeamStrip(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    SwordBeamStrip layer, ActionEffectProjectPointFn project, void *userdata) {
  enum { kSegments = kActionSceneEffectSwordStripSegments,
         kAcross = kActionSceneEffectSwordStripAcross,
         kVertices = (kSegments + 1) * kAcross };
  ArRenderVertex2D vertices[kVertices];
  int indices[kSegments * (kAcross - 1) * 6];
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float cx = (rect->x0 + rect->x1) * 0.5f;
  const float cy = (rect->y0 + rect->y1) * 0.5f;
  const float half_height = (rect->y1 - rect->y0) * 0.5f;
  const float half_width = (rect->x1 - rect->x0) * 0.5f;
  float hx = 1.0f, hy = 0.0f;
  SceneActorHeading(effect, &hx, &hy);
  const unsigned ticks = effect->age_ticks;
  const float clock = (float)ticks;
  const float speed = hypotf((float)effect->velocity_x, (float)effect->velocity_y);
  const bool arc = layer == kSwordBeamStrip_ArcCorona || layer == kSwordBeamStrip_ArcCore;
  const bool haze = layer == kSwordBeamStrip_Haze;
  const bool corona = layer == kSwordBeamStrip_ArcCorona;
  const float lane = layer == kSwordBeamStrip_UpperWake ? -1.0f : 1.0f;
  const float length = fminf(haze ? 96.0f : 78.0f,
                            clock * fmaxf(2.0f, speed) + 2.0f);
  const float pulse = SwordBeamPulse(effect);
  const uint32_t seed = DeterministicHash_Mix32(effect->generation ^ effect->record_address);
  const float noise_blend = SwordSmoothStep((float)(ticks % 3u) / 3.0f);
  static const float cross_alpha[kAcross] = {0.0f, 0.32f, 1.0f, 0.32f, 0.0f};
  for (unsigned i = 0; i <= kSegments; ++i) {
    const float t = (float)i / kSegments;
    const float remain = 1.0f - t;
    float along, side, width, alpha;
    ArRenderColorF color;
    if (arc) {
      /* Smooth interpolation between three-tick noise samples keeps the bolt
       * lively without teleporting or detaching from the original sprite. */
      const float noise0 = HashUnit(seed ^ (i * 0x85EBCA6Bu) ^ ((ticks / 3u) * 0x9E3779B9u));
      const float noise1 = HashUnit(seed ^ (i * 0x85EBCA6Bu) ^ ((ticks / 3u + 1u) * 0x9E3779B9u));
      const float taper = sinf(t * 3.14159265f);
      const float bend = (noise0 + (noise1 - noise0) * noise_blend - 0.5f) * 3.2f * taper;
      /* Skim the outer edge: a white filament inside the white sprite would
       * disappear under additive blending. Let small bends peek outside it. */
      along = half_width * (0.95f - 1.3f * (2.0f * t - 1.0f) * (2.0f * t - 1.0f)) + bend;
      side = (2.0f * t - 1.0f) * half_height * 0.94f;
      width = (corona ? 2.4f : 0.65f) * (0.35f + 0.65f * taper);
      alpha = SwordSmoothStep(t * 9.0f) * SwordSmoothStep(remain * 9.0f) * pulse;
      color = corona ? (ArRenderColorF){0.20f, 0.62f, 1.0f, 0.45f}
                     : (ArRenderColorF){0.79f, 0.97f, 1.0f, 0.95f};
    } else {
      along = -length * t;
      const float wave = sinf(t * 8.0f - clock * 0.17f + lane * 0.8f);
      side = haze ? wave * 1.4f * t * remain
          : lane * half_height * 0.62f + wave * 3.2f * t * remain;
      /* Carry the blade's width and energy through the wake. Dissolve the
       * final quarter in alpha instead of pinching it into a triangular cone. */
      width = haze ? half_height * 0.92f + 2.0f : 4.1f;
      alpha = (1.0f - 0.15f * t) * SwordSmoothStep(remain * 4.0f) *
              SwordSmoothStep((t + 0.035f) * 9.0f) * pulse;
      color = MixColor((ArRenderColorF){0.52f, 0.90f, 1.0f, haze ? 0.36f : 0.48f},
                       (ArRenderColorF){0.36f, 0.48f, 1.0f, haze ? 0.30f : 0.40f}, t);
    }
    for (unsigned j = 0; j < kAcross; ++j) {
      const float offset = ((float)j / (kAcross - 1) * 2.0f - 1.0f) * width;
      const float x = along + (arc ? offset : 0.0f);
      const float y = side + (arc ? 0.0f : offset);
      ArRenderColorF c = color;
      c.a *= alpha * cross_alpha[j];
      vertices[i * kAcross + j] = (ArRenderVertex2D){
          {cx + hx * x - hy * y, cy + hy * x + hx * y}, c, {0.0f, 0.0f}};
    }
  }
  unsigned count = 0;
  for (unsigned i = 0; i < kSegments; ++i) {
    for (unsigned j = 0; j + 1 < kAcross; ++j) {
      const int a = (int)(i * kAcross + j), b = a + kAcross;
      const int quad[] = {a, b, b + 1, a, b + 1, a + 1};
      memcpy(indices + count, quad, sizeof(quad));
      count += 6;
    }
  }
  return AppendSwordMesh(writer, effect, vertices, kVertices, indices, count, project, userdata);
}

bool AppendSwordBeamParticles(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, ActionEffectProjectPointFn project, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float cx = (rect->x0 + rect->x1) * 0.5f;
  const float cy = (rect->y0 + rect->y1) * 0.5f;
  const float half_height = (rect->y1 - rect->y0) * 0.5f;
  float hx = 1.0f, hy = 0.0f;
  SceneActorHeading(effect, &hx, &hy);
  const float speed = hypotf((float)effect->velocity_x, (float)effect->velocity_y);
  /* Use actor age/identity so animation-pose changes cannot restart the wake.
   * Each slot is born near the crescent, advects backward in actor space, then
   * becomes transparent before reseeding. No pre-aged tail appears on spawn. */
  for (unsigned i = 0; i < kActionSceneEffectSwordMoteCount; ++i) {
    const uint32_t identity = DeterministicHash_Mix32(effect->generation * 0x9E3779B9u ^
        (uint32_t)effect->record_address * 0x85EBCA6Bu ^ i * 0xC2B2AE35u);
    const unsigned lifetime = 14u + ((identity >> 8) % 9u);
    const unsigned delay = identity % lifetime;
    const unsigned elapsed = effect->age_ticks >= delay ? effect->age_ticks - delay : 0;
    const unsigned age = elapsed % lifetime;
    const uint32_t seed = DeterministicHash_Mix32(identity ^ (elapsed / lifetime) * 0x27D4EB2Du);
    const float t = (float)age / (float)(lifetime - 1);
    const float birth = SwordSmoothStep(t / 0.18f);
    const float fade = 1.0f - SwordSmoothStep((t - 0.18f) / 0.82f);
    const float spread = (HashUnit(seed ^ 0x53u) * 2.0f - 1.0f);
    const float distance = 2.0f + (float)age * (speed * 0.82f + 0.45f);
    const float side = spread * half_height * 0.78f + spread * 4.0f * t +
        sinf(t * 5.0f + HashUnit(seed ^ 0x97u) * 6.2831853f) * 2.0f * t;
    const float x = cx - hx * distance - hy * side;
    const float y = cy - hy * distance + hx * side;
    const bool glint = i % 6u == 0;
    const float size = (0.85f + HashUnit(seed ^ 0xD3u) * 0.9f) *
        (0.65f + 0.35f * fade) * (glint ? 1.65f : 1.0f);
    ArRenderColorF color = MixColor((ArRenderColorF){0.88f, 0.99f, 1.0f, 0.98f},
                                   (ArRenderColorF){0.38f, 0.48f, 1.0f, 0.65f}, t);
    color.a *= birth * fade * (0.7f + HashUnit(seed ^ 0xA7u) * 0.3f);
    ArRenderVertex2D vertices[kActionSceneEffectSwordMoteVertices];
    int indices[kActionSceneEffectSwordMoteIndices];
    vertices[0] = (ArRenderVertex2D){{x, y}, color, {0.0f, 0.0f}};
    color.a = 0.0f;
    for (unsigned j = 0; j < kActionSceneEffectSwordMoteSegments; ++j) {
      const float radius = glint && (j & 1u) ? size * 0.22f : size;
      const unsigned circle_index = j * (kActionEffectGlowSegments / kActionSceneEffectSwordMoteSegments);
      vertices[j + 1] = (ArRenderVertex2D){
          {x + kCircle32[circle_index][0] * radius, y + kCircle32[circle_index][1] * radius},
          color, {0.0f, 0.0f}};
      indices[j * 3] = 0;
      indices[j * 3 + 1] = (int)j + 1;
      indices[j * 3 + 2] = (int)((j + 1u) % kActionSceneEffectSwordMoteSegments) + 1;
    }
    if (!AppendSwordMesh(writer, effect, vertices, kActionSceneEffectSwordMoteVertices,
                          indices, kActionSceneEffectSwordMoteIndices, project, userdata)) return false;
  }
  return true;
}

bool AppendSwordBeamLighting(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, ActionEffectProjectPointFn project, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float cx = (rect->x0 + rect->x1) * 0.5f;
  const float cy = (rect->y0 + rect->y1) * 0.5f;
  float hx = 1.0f, hy = 0.0f;
  SceneActorHeading(effect, &hx, &hy);
  const float pulse = SwordBeamPulse(effect);
  /* A smooth, restrained pool of light leaves the authored crescent readable.
   * The brighter arc follows its curve instead of filling its bounding box. */
  const ActionEffectGlowStyle spill = {
      .radius_x = 23.0f, .radius_y = 25.0f, .axis_x = hx, .axis_y = hy,
      .ring_scale = {0.22f, 0.55f, 1.0f},
      .centre = {0.40f, 0.78f, 1.0f, 0.20f},
      .ring = {{0.25f, 0.60f, 1.0f, 0.13f}, {0.18f, 0.26f, 0.85f, 0.035f},
               {0.10f, 0.14f, 0.55f, 0.0f}}};
  const ActionEffectGlowStyle body = {
      .radius_x = 9.0f, .radius_y = (rect->y1 - rect->y0) * 0.57f,
      .axis_x = hx, .axis_y = hy, .ring_scale = {0.20f, 0.52f, 1.0f},
      .centre = {0.75f, 0.97f, 1.0f, 0.30f},
      .ring = {{0.32f, 0.81f, 1.0f, 0.19f}, {0.12f, 0.46f, 1.0f, 0.06f},
               {0.04f, 0.16f, 0.70f, 0.0f}}};
  if (!AppendGlow(writer, effect, &spill, pulse, cx - hx * 2.0f, cy - hy * 2.0f,
                  project, userdata) ||
      !AppendGlow(writer, effect, &body, pulse, cx, cy, project, userdata)) return false;
  for (int layer = 0; layer < kSwordBeamStrip_Count; ++layer)
    if (!AppendSwordBeamStrip(writer, effect, (SwordBeamStrip)layer, project, userdata)) return false;
  return true;
}

static const SceneParticleLifetime kSceneLightningLifetime = {11, 6, 7};

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
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionArcField *f,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot = ArcColor(f->Hot);
  const ArRenderColorF cool = ArcColor(f->Cool);
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks =
      (unsigned)f->Clock[0]*effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    /* Most sparks crawl across the full bolt; the last quarter burst away
     * from its lower impact so the strike has both a shaft and a contact. */
    if (i >= (unsigned)((float)count*f->StrikeMotion[0])) {
      const float *direction =
          kCircle32[(i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
      const float distance = f->StrikeMotion[1] + f->StrikeMotion[2] * t;
      const float old_distance = f->StrikeMotion[1] + f->StrikeMotion[2] * previous_t;
      x = (rect->x0 + rect->x1) * 0.5f + direction[0] * distance;
      y = rect->y1 + direction[1] * distance * f->StrikeMotion[3];
      previous_x = (rect->x0 + rect->x1) * 0.5f + direction[0] * old_distance;
      previous_y = rect->y1 + direction[1] * old_distance * f->StrikeMotion[3];
    } else {
      const float along = HashUnit(seed ^ 0x29u);
      const float base_y = rect->y0 + (rect->y1 - rect->y0) * along;
      const float jitter =
          (HashUnit(seed ^ (visual_ticks * 0x27D4EB2Du)) - 0.5f) * f->ShaftMotion[0];
      const float old_jitter = -jitter * f->ShaftMotion[1];
      x = (rect->x0 + rect->x1) * 0.5f + jitter;
      y = base_y + (t - 0.5f) * f->ShaftMotion[2];
      previous_x = (rect->x0 + rect->x1) * 0.5f + old_jitter;
      previous_y = base_y + (previous_t - 0.5f) * f->ShaftMotion[2];
    }
    width = f->StrikeShape[0] + f->StrikeShape[1] * (1.0f - t);
    reach = f->StrikeShape[2] + f->StrikeShape[3] * (1.0f - t);

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

bool AppendLightningTrapLighting(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,const ActionArcField *f,ActionEffectProjectPointFn project_point,void *userdata){
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *r=&effect->geometry.data.rect;
  const float x=(r->x0+r->x1)*.5f,y=(r->y0+r->y1)*.5f,pulse=ArcPulse(effect,f);
  ActionEffectGlowStyle spill=f->styles[0],body=f->styles[1];
  spill.radius_x=fmaxf(spill.radius_x,(r->x1-r->x0)*f->StrikeSpill[2]);
  spill.radius_y=fmaxf(spill.radius_y,(r->y1-r->y0)*f->StrikeSpill[3]);
  body.radius_x=fmaxf(body.radius_x,(r->x1-r->x0)*f->StrikeBody[2]);
  body.radius_y=fmaxf(body.radius_y,(r->y1-r->y0)*f->StrikeBody[3]);
  spill.seed=effect->record_address;body.seed=effect->pulse_generation;
  const unsigned ticks=(unsigned)f->Clock[0]*effect->pulse_ticks;
  return AppendGlowAtTicks(writer,effect,&spill,pulse,x,y,project_point,userdata,ticks)&&
      AppendGlowAtTicks(writer,effect,&body,pulse,x,y,project_point,userdata,ticks);
}

bool AppendBossLightningParticles(ActionEffectGeometryWriter *writer,
                                  const ActionEffectInstance *effect, const ActionArcField *f,
                                  ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  const unsigned count=(unsigned)f->Particles[0];
  const ArRenderColorF hot =
      ArcColor(f->Hot);
  const ArRenderColorF cool =
      ArcColor(f->Cool);
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float emission_y = effect->kind == kActionEffect_CentaurLightning
                               ? (rect->y0 + rect->y1) * .5f
                               : rect->y1 + f->BurstMotion[3];
  const unsigned visual_ticks =
      (unsigned)f->Clock[0]*effect->pulse_ticks;
  for (unsigned i = 0; i < count; ++i) {
    const SceneParticleClock clock =
        SceneParticleClockAt(effect, visual_ticks, i, (SceneParticleLifetime){(unsigned)f->Particles[1],(unsigned)f->Particles[2],(unsigned)f->Particles[3]});
    const uint32_t seed = clock.seed;
    const float t = clock.t, previous_t = clock.previous_t;
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    if (effect->phase == kActionEffectPhase_BossLightningStrike) {
      float endpoint_x = (rect->x0 + rect->x1) * 0.5f;
      float endpoint_y = rect->y1;
      BossLightningPathSample(effect,f, 1.0f, &endpoint_x, &endpoint_y);
      if (i >= (unsigned)((float)count*f->StrikeMotion[0])) {
        const float *direction =
            kCircle32[(i * 9u + (seed >> 11)) & kActionEffectGlowSegmentMask];
        const float distance = f->StrikeMotion[1] + f->StrikeMotion[2] * t;
        const float old_distance = f->StrikeMotion[1] + f->StrikeMotion[2] * previous_t;
        x = endpoint_x + direction[0] * distance;
        y = endpoint_y + direction[1] * distance * f->StrikeMotion[3];
        previous_x = endpoint_x + direction[0] * old_distance;
        previous_y = endpoint_y + direction[1] * old_distance * f->StrikeMotion[3];
      } else {
        const float birth = HashUnit(seed ^ 0x29u);
        float along = birth + f->StrikeMotion[4] * t;
        float previous_along = birth + f->StrikeMotion[4] * previous_t;
        if (along > 1.0f) along -= 1.0f;
        if (previous_along > 1.0f) previous_along -= 1.0f;
        if (!BossLightningPathSample(effect,f, along, &x, &y) ||
            !BossLightningPathSample(effect,f, previous_along, &previous_x,
                                     &previous_y))
          continue;
      }
      width = f->StrikeShape[0] + f->StrikeShape[1] * (1.0f - t);
      reach = f->StrikeShape[2] + f->StrikeShape[3] * (1.0f - t);
    } else {
      /* Charges and floor bursts expand around their own captured artwork. */
      const float *direction =
          kCircle32[(i * 11u + (seed >> 13)) & kActionEffectGlowSegmentMask];
      const float distance = f->BurstMotion[0] + f->BurstMotion[1] * t;
      const float old_distance = f->BurstMotion[0] + f->BurstMotion[1] * previous_t;
      x = (rect->x0 + rect->x1) * 0.5f + direction[0] * distance;
      y = emission_y + direction[1] * distance * f->BurstMotion[2];
      previous_x = (rect->x0 + rect->x1) * 0.5f + direction[0] * old_distance;
      previous_y = emission_y + direction[1] * old_distance * f->BurstMotion[2];
      width = f->BurstShape[0] + f->BurstShape[1] * (1.0f - t);
      reach = f->BurstShape[2] + f->BurstShape[3] * (1.0f - t);
    }

    const ArRenderColorF color = SceneParticleColor(hot, cool, clock);
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

bool AppendBossLightningLighting(ActionEffectGeometryWriter *writer,
                                 const ActionEffectInstance *effect, const ActionArcField *f,
                                 ActionEffectProjectPointFn project_point, void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = ArcPulse(effect,f);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;
  if (effect->phase == kActionEffectPhase_BossLightningStrike) {
    BossLightningPathSample(effect,f, 0.5f, &spill_x, &spill_y);
    body_x = spill_x;
    body_y = spill_y;
    spill = f->styles[0];
    spill.radius_x = fmaxf(spill.radius_x,(rect->x1-rect->x0)*f->StrikeSpill[2]);
    spill.radius_y = fmaxf(spill.radius_y,(rect->y1-rect->y0)*f->StrikeSpill[3]);
    spill.seed = (unsigned)effect->record_address;
    body = f->styles[1];
    body.radius_x=fmaxf(body.radius_x,(rect->x1-rect->x0)*f->StrikeBody[2]);
    body.radius_y = fmaxf(body.radius_y,(rect->y1-rect->y0)*f->StrikeBody[3]);
    body.seed = (unsigned)effect->pulse_generation;
    float start_x, start_y, end_x, end_y;
    if (BossLightningPathSample(effect,f, 0.0f, &start_x, &start_y) &&
        BossLightningPathSample(effect,f, 1.0f, &end_x, &end_y)) {
      const float path_x = end_x - start_x;
      const float path_y = end_y - start_y;
      const float path_length = hypotf(path_x, path_y);
      if (path_length > 0.001f) {
        /* Local +Y is the ellipse's long axis; rotate it onto the
         * authored start-to-end chord. The ribbon retains the individual
         * OAM bends while its surrounding body agrees with their angle. */
        const float ax=path_y/path_length,ay=-path_x/path_length;
        const float spill_x=spill.axis_x,body_x=body.axis_x;
        spill.axis_x=spill_x*ax-spill.axis_y*ay;spill.axis_y=spill_x*ay+spill.axis_y*ax;
        body.axis_x=body_x*ax-body.axis_y*ay;body.axis_y=body_x*ay+body.axis_y*ax;
      }
    }
  } else {
    spill = f->styles[2];
    spill.seed = (unsigned)effect->record_address;
    body = f->styles[3];
    spill.radius_x=fmaxf(spill.radius_x,(rect->x1-rect->x0)*f->BurstSpill[2]);
    spill.radius_y=fmaxf(spill.radius_y,(rect->y1-rect->y0)*f->BurstSpill[3]);
    body.radius_x=fmaxf(body.radius_x,(rect->x1-rect->x0)*f->BurstBody[2]);
    body.radius_y=fmaxf(body.radius_y,(rect->y1-rect->y0)*f->BurstBody[3]);
    body.seed = (unsigned)effect->pulse_generation;
    spill_y = body_y = effect->kind == kActionEffect_CentaurLightning ? mid_y : rect->y1 + f->BurstMotion[3];
    if (effect->phase == kActionEffectPhase_CentaurStaffCharge) {
      spill.radius_x = spill.radius_y = f->StaffRadius[0];
      body.radius_x = body.radius_y = f->StaffRadius[1];
    }
  }
  if (!AppendGlowAtTicks(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata,(unsigned)f->Clock[0]*effect->pulse_ticks))
    return false;
  if (!AppendGlowAtTicks(writer, effect, &body, pulse, body_x, body_y, project_point,
                  userdata,(unsigned)f->Clock[0]*effect->pulse_ticks))
    return false;
  return AppendBossLightningRibbon(writer, effect, f, project_point, userdata);
}
