/* Configured grouped rays and light-responsive particles. Phase: pure. */
#include "action/action_effect_render_internal.h"
#include "action_ray_field.h"
#define P(name) field->values[kActionRay_##name]
#include "action_effect_members.h"

enum {
  kForestVisibleRays = kActionRayFieldMaxRays,
  kForestRayRows = 6,
  kForestRayColumns = 5,
  kForestMotes = 22,
  kForestLeaves = 16,
  kForestDustClusters = 4,
  kForestDustPerCluster = 8,
  kForestDustMotes = kForestVisibleRays * kForestDustClusters * kForestDustPerCluster
};
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

static float ForestSoftRamp(float value) {
  const float t = fmaxf(0, fminf(1, value));
  return t * t * (3 - 2 * t);
}

static float ForestRaySway(const ActionEffectInstance *effect, const ActionRayField *field, unsigned opening) {
  const unsigned fan = field->rays[opening].fan;
  const unsigned phase = fan ? 16 + fan : opening;
  return P(SwayAmplitude) * sinf((effect->phase_ticks & ((unsigned)P(SwayPeriod)-1u)) * (6.2831853f / P(SwayPeriod)) + phase * P(SwayPhase));
}

static float ForestRaySlope(const ActionRayField *field, const ActionRayOpening *opening) {
  if (!opening->fan) return P(IsolatedSlope);
  const ArRenderPointF origin = field->fans[opening->fan];
  return (origin.x - opening->x) / -origin.y;
}

typedef struct ForestRayState {
  float slope, sway, root_y, reach;
  ActionRayOpening opening;
  const ActionNativeMember *member;
} ForestRayState;

static ForestRayState ForestRayAt(const ActionEffectInstance *effect, const ActionRayField *field,
                                  const ActionNativeMembers *members, unsigned i, float sway) {
  const unsigned kind = effect->kind == kActionEffect_ForestForwardLight
                            ? effect->kind
                            : kActionEffect_ForestCanopyLight;
  const ActionNativeMember *m = ActionEffectMembers_Find(members, kind, i);
  ForestRayState ray = {.slope = ForestRaySlope(field, &field->rays[i]),
                        .sway = sway,
                        .root_y = P(OriginY),
                        .reach = 1,
                        .opening = field->rays[i],
                        .member = m};
  if (m) {
    ray.opening.x += m->offset_x;
    ray.root_y += m->offset_y;
    ray.reach = m->length_scale;
    ray.opening.half_width *= m->width_scale;
    ray.opening.strength *= m->enabled ? m->intensity : 0;
    ray.slope = tanf(atanf(ray.slope) + m->angle * .01745329252f);
  }
  return ray;
}

static void ForestRayStates(const ActionEffectInstance *effect, const ActionRayField *field, const ActionNativeMembers *members,
                            ForestRayState *rays) {
  float fan_sway[kActionRayFieldMaxFans + 1] = {0};
  bool fan_seen[kActionRayFieldMaxFans + 1] = {false};
  for (unsigned i = 0; i < field->ray_count; ++i) {
    const unsigned fan = field->rays[i].fan;
    float sway;
    /* Rays sharing an origin also share the sway phase. Evaluate it once per
     * group; independent openings retain their individual deterministic phase. */
    if (fan) {
      if (!fan_seen[fan]) {
        fan_sway[fan] = ForestRaySway(effect, field, i);
        fan_seen[fan] = true;
      }
      sway = fan_sway[fan];
    } else sway = ForestRaySway(effect, field, i);
    rays[i] = ForestRayAt(effect, field, members, i, sway);
  }
}

static float ForestRayWidth(const ActionRayField *field, const ActionRayOpening *opening, float world_y) {
  if (opening->fan) {
    const float source_y = field->fans[opening->fan].y;
    /* Both edges and the centre converge at the same fixed source. Shared
     * sway moves the complete fan without separating its rays at the origin. */
    return opening->half_width * P(WidthBase) * fmaxf(0, (world_y - source_y) / -source_y);
  }
  return opening->half_width * (P(WidthBase) + P(WidthGrowth) * fmaxf(0, fminf(P(WidthGrowthLimit), world_y)));
}

static float ForestLightAmount(
    const ActionEffectInstance *effect, const ActionRayField *field, const ForestRayState *rays, float x, float y) {
  const float world_x = effect->world_x + x;
  const float world_y = effect->world_y + y;
  float light = 0;
  for (unsigned i = 0; i < field->ray_count; i++) {
    const ActionRayOpening *opening = &rays[i].opening;
    const float centre = opening->x - rays[i].slope * (world_y - rays[i].root_y) + rays[i].sway;
    const float width = ForestRayWidth(field, opening, (world_y - rays[i].root_y) / rays[i].reach);
    if (width <= 0) continue;
    const float amount =
        opening->strength *
        ForestSoftRamp(1 - fabsf(world_x - centre) / width);
    light = fmaxf(light, amount);
  }
  return light;
}

static bool ForestRayIntersectsField(const ActionEffectInstance *effect, const ActionRayField *field, ForestRayState ray) {
  const ActionRayOpening *opening = &ray.opening;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float top = effect->world_y + rect->y0, bottom = effect->world_y + rect->y1;
  const float upper = opening->x - ray.slope * (top - ray.root_y) + ray.sway - effect->world_x;
  const float lower = opening->x - ray.slope * (bottom - ray.root_y) + ray.sway - effect->world_x;
  const float upper_width = ForestRayWidth(field, opening, (top - ray.root_y) / ray.reach);
  const float lower_width = ForestRayWidth(field, opening, (bottom - ray.root_y) / ray.reach);
  const float left = fminf(upper - upper_width, lower - lower_width);
  const float right = fmaxf(upper + upper_width, lower + lower_width);
  return opening->strength > 0 && right > rect->x0 && left < rect->x1;
}

bool AppendForestRays(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                      const ActionRayField *field, const ActionNativeMembers *members, bool foreground,
                      ActionEffectProjectPointFn project_point,
                      ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  if(!field)field=ActionRayField_Bundled();
  if(!field)return false;
  if(!(field->components&(foreground?8u:1u)))return true;
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
  ForestRayState rays[kActionRayFieldMaxRays];
  ForestRayStates(effect, field, members, rays);
  unsigned visible = 0;
  for (unsigned i = 0; i < field->ray_count; i++) {
    const ActionRayOpening *opening = &rays[i].opening;
    const ActionRayProfile *profile=&field->profiles[opening->profile];
    const float across[kForestRayColumns] = {0, opening->shoulder, 1, opening->shoulder, 0};
    const ForestRayState ray = rays[i];
    const int first_vertex = writer->vertex_count;
    if (!ForestRayIntersectsField(effect, field, ray)) continue;
    if (++visible > kForestVisibleRays) return false;
    ActionEffectLocalRect ray_clip = clip;
    if (ray.member)
      ray_clip.y1 = fminf(ray_clip.y1, ray.root_y + P(WidthGrowthLimit) * ray.reach - effect->world_y);
    ArRenderVertex2D source[kForestRayRows * kForestRayColumns];
    int mapped[kForestRayRows * kForestRayColumns];
    for (int row = 0; row < kForestRayRows; row++) {
      const float y = profile->rows[row];
      const float world_y = effect->world_y + y;
      const float centre =
          opening->x - ray.slope * (world_y - ray.root_y) + ray.sway - effect->world_x;
      const float width = ForestRayWidth(field, opening, (world_y - rays[i].root_y) / rays[i].reach);
      /* Only this HUD fade follows screen Y. Ray positions, widths and lower
       * attenuation belong to the world and scroll with the light layer. */
      /* The clearing's surface light reaches the tall rider above the horse,
       * while keeping the first 32 native screen rows free of foreground light. */
      const float fade = foreground ? ForestSoftRamp((y-profile->front_start)/profile->front_range) :
          1 - P(RearFadeAmount) * ForestSoftRamp((world_y-P(OriginY)-P(RearFadeStart))/P(RearFadeRange));
      for (int column = 0; column < kForestRayColumns; column++) {
        const float x = centre + (column * .5f - 1) * width;
        const float amount = opening->strength * fade * across[column];
        ArRenderColorF color = foreground ? profile->front : profile->rear;
        if (foreground) {
          color.r = fminf(1, color.r * amount);
          color.g = fminf(1, color.g * amount);
          color.b = fminf(1, color.b * amount);
        } else {
          color.a = fminf(1, color.a * amount);
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
          if (!AppendSceneClippedTriangle(writer, &mesh, source, mapped, &cell[j], &ray_clip,
                                          project_point, userdata))
            return false;
      }
    }
    if (foreground) {
      const float source_y = opening->fan ? field->fans[opening->fan].y : P(IsolatedOriginY);
      const float root_x = opening->x - ray.slope * source_y + ray.sway;
      ActionSceneryShadow_Apply(writer, first_vertex, effect, root_x - effect->world_x,
                                source_y + ray.root_y - effect->world_y, true, project_point,
                                userdata);
    }
    if (ray.member) {
      ActionNativeMember tint = *ray.member;
      tint.intensity = 1;
      if (writer->source) ActionEffectSource_Tint(writer->source, first_vertex, tint.color, tint.intensity, foreground);
      else ActionEffectMembers_Tint(&tint, writer->vertices, first_vertex, writer->vertex_count,
                               foreground);
    }
  }
  return true;
}

static bool AppendForestRayDust(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionRayField *field,
    const ForestRayState *rays, ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  /* Both the lifetime and ray sway divide the captured 16-bit scene clock.
   * Dust pockets are fixed in world space; the light can sway across them. */
  const SceneParticleLifetime lifetime = {(unsigned)P(ClusterPeriod), 0, 0};
  unsigned visible = 0;
  for (unsigned i = 0; i < field->ray_count; i++) {
    if (!ForestRayIntersectsField(effect, field, rays[i])) continue;
    if (++visible > kForestVisibleRays) return false;
    const ActionRayOpening *opening = &rays[i].opening;
    const float slope = rays[i].slope, sway = rays[i].sway;
    for (unsigned cluster = 0; cluster < (unsigned)P(ClusterCount); cluster++) {
      const uint32_t seed = DeterministicHash_Mix32(effect->pulse_generation ^
          (i + 1u) * 0x9E3779B9u ^ (cluster + 1u) * 0x85EBCA6Bu);
      const float cloud_y = P(OriginY) + P(ClusterOriginY) + cluster * P(ClusterPitchY) + P(ClusterJitterY) * HashUnit(seed ^ 0x71u);
      const float cloud_width = ForestRayWidth(field, opening, (cloud_y - rays[i].root_y) / rays[i].reach);
      const float cloud_x = opening->x - slope * (cloud_y - rays[i].root_y) +
                            (P(ClusterAcross) * HashUnit(seed ^ 0x39u) - P(ClusterAcross)*.5f) * cloud_width;
      ActionEffectInstance dust = *effect;
      dust.pulse_generation = seed;
      for (unsigned mote = 0; mote < (unsigned)P(ClusterParticles); mote++) {
        const SceneParticleClock clock = SceneParticleClockAt(
            &dust, effect->phase_ticks, mote, lifetime);
        const float turn = clock.t * 6.2831853f + HashUnit(clock.seed) * 6.2831853f;
        const float world_x = cloud_x + (HashUnit(clock.seed ^ 0x93u) - .5f) *
            fminf(P(ClusterWidthLimit), cloud_width * P(ClusterWidth)) + P(DustDriftX) * (clock.t - .5f) + P(DustWanderX) * sinf(turn);
        const float world_y = cloud_y + (HashUnit(clock.seed ^ 0xB5u) - .5f) * P(DustRangeY) -
            P(DustDriftY) * (clock.t - .5f) + P(DustWanderY) * cosf(turn);
        const float x = world_x - effect->world_x, y = world_y - effect->world_y;
        if (x < rect->x0 || x > rect->x1 || y < rect->y0 || y > rect->y1) continue;
        const float centre = opening->x - slope * (world_y - rays[i].root_y) + sway;
        const float width = ForestRayWidth(field, opening, (world_y - rays[i].root_y) / rays[i].reach);
        if (width <= 0) continue;
        const float light =
            opening->strength *
            ForestSoftRamp(1 -
                           fabsf(world_x - centre) / width);
        const float alpha = P(DustAlpha) * light * ForestSoftRamp(clock.t * P(DustFade)) *
            ForestSoftRamp((1 - clock.t) * P(DustFade)) * (P(DustVariationBase) + P(DustVariationRange) * HashUnit(clock.seed ^ 0xA7u));
        if (alpha < P(DustThreshold)) continue;
        const float size = P(DustSize) + P(DustSizeRange) * HashUnit(clock.seed ^ 0xD3u);
        if (!AppendSceneParticle(writer, effect, x, y, x, y - P(DustTail),
                size, size, (ArRenderColorF){field->cluster_color.r, field->cluster_color.g, field->cluster_color.b, fminf(1, alpha * field->cluster_color.a)}, project_point, userdata))
          return false;
      }
    }
  }
  return true;
}

bool AppendForestMotes(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                       const ActionRayField *field, const ActionNativeMembers *members, ActionEffectProjectPointFn project_point,
                       void *userdata) {
  if(!field)field=ActionRayField_Bundled();
  if(!field)return false;
  if(!(field->components&2))return true;
  ForestRayState rays[kActionRayFieldMaxRays];
  ForestRayStates(effect, field, members, rays);
  const SceneParticleLifetime lifetime = {(unsigned)P(MotePeriod), 0, 0};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const int first_cell = (int)floorf((effect->world_x + rect->x0) / P(MotePitch));
  for (unsigned i = 0; i < (unsigned)P(MoteCount); i++) {
    const int cell = first_cell + (int)i;
    ActionEffectInstance mote = *effect;
    mote.generation += (uint32_t)cell;
    mote.pulse_generation += (uint32_t)cell;
    const SceneParticleClock clock =
        SceneParticleClockAt(&mote, effect->phase_ticks, 0, lifetime);
    const float x = cell * P(MotePitch) + P(MoteOffsetX) + P(MoteJitterX) * HashUnit(clock.seed ^ 0x39u) +
        P(MoteDriftX) * clock.t - effect->world_x;
    const float y = P(OriginY) + P(MoteOriginY) + P(MoteRangeY) * HashUnit(clock.seed ^ 0x71u) -
        P(MoteDriftY) * clock.t - effect->world_y;
    if (x < rect->x0 || x > rect->x1 || y < rect->y0 || y > rect->y1)
      continue;
    const float light = sqrtf(ForestLightAmount(effect, field, rays, x, y));
    const float alpha = P(MoteAlpha) * fmaxf(0, sinf(clock.t * 3.14159265f)) * (P(MoteAmbient) + P(MoteResponse) * light);
    const float size = P(MoteSize) + P(MoteSizeRange) * HashUnit(clock.seed ^ 0x93u);
    if (!AppendSceneParticle(writer, effect, x, y, x, y - P(MoteTail),
            size, size * P(MoteAspect), (ArRenderColorF){field->mote_color.r,field->mote_color.g,field->mote_color.b,fminf(1,alpha*field->mote_color.a)},
            project_point, userdata))
      return false;
  }
  return AppendForestRayDust(writer, effect, field, rays, project_point, userdata);
}

bool AppendForestLeaves(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                        const ActionRayField *field, const ActionNativeMembers *members,
                        ActionEffectProjectPointFn project_point, void *userdata) {
  if(!field)field=ActionRayField_Bundled();
  if(!field)return false;
  if(!(field->components&4))return true;
  ForestRayState rays[kActionRayFieldMaxRays];
  ForestRayStates(effect, field, members, rays);
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const int first_cell = (int)floorf((effect->world_x + rect->x0) / P(LeafPitch));
  /* Divide the captured 16-bit clock's range so its wrap cannot jump leaves. */
  const SceneParticleLifetime lifetime = {(unsigned)P(LeafPeriod), 0, 0};
  for (int i = 0; i < (int)P(LeafCount); i++) {
    const int cell = first_cell + i;
    ActionEffectInstance leaf = *effect;
    leaf.pulse_generation += (uint32_t)cell * 31u + 0x7100u;
    const SceneParticleClock clock =
        SceneParticleClockAt(&leaf, effect->phase_ticks, 0, lifetime);
    const float turn = clock.t * P(LeafTurn) + HashUnit(clock.seed) * 6.2831853f;
    const float x = cell * P(LeafPitch) + P(LeafOffsetX) + HashUnit(clock.seed ^ 0x38u) * P(LeafJitterX) +
        P(LeafWanderX) * sinf(turn) - effect->world_x;
    const float y = P(OriginY) + P(LeafOriginY) + clock.t * P(LeafTravelY) - effect->world_y;
    if (x < rect->x0 || x > rect->x1 || y < rect->y0 || y > rect->y1) continue;
    const float light = sqrtf(ForestLightAmount(effect, field, rays, x, y));
    const float alpha = P(LeafAlpha) *
        ForestSoftRamp(clock.t * P(LeafFade)) * ForestSoftRamp((1 - clock.t) * P(LeafFade));
    const float angle = P(LeafAngle) * sinf(turn * P(LeafAngleRate));
    const float c = cosf(angle), s = sinf(angle);
    const float twist = P(LeafTwistBase) + P(LeafTwistRange) * fabsf(cosf(turn));
    ArRenderPointF points[6];
    bool visible = true;
    for (int j = 0; j < 6; j++) {
      const float lx = field->leaf_shape[j].x * twist, ly = field->leaf_shape[j].y;
      if (writer->source) {
        points[j] = (ArRenderPointF){x + c * lx - s * ly, y + s * lx + c * ly};
        continue;
      }
      if (!project_point(userdata, effect, x + c * lx - s * ly,
              y + s * lx + c * ly, &points[j])) {
        visible = false;
        break;
      }
    }
    if (!visible) continue;
    if (writer->source) {
      const ArRenderColorF color = {field->leaf_color.r, field->leaf_color.g,
          field->leaf_color.b, alpha * field->leaf_color.a};
      const ArRenderColorF rim = {field->leaf_rim_color.r, field->leaf_rim_color.g,
          field->leaf_rim_color.b, fminf(1, alpha * (P(LeafRimBase) + P(LeafRimResponse) * light) * field->leaf_rim_color.a)};
      if (!ActionEffectSource_Leaf(writer->source, effect, points, color, rim)) return false;
      writer->vertex_count = (int)writer->source->count;
      continue;
    }
    if (!Reserve(writer, 9, 15)) return false;
    const int base = writer->vertex_count;
    for (int j = 0; j < 6; j++)
      writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){
        points[j], {field->leaf_color.r,field->leaf_color.g,field->leaf_color.b,alpha*field->leaf_color.a}, {0,0},
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
        rim[j], {field->leaf_rim_color.r,field->leaf_rim_color.g,field->leaf_rim_color.b,fminf(1,alpha * (P(LeafRimBase) + P(LeafRimResponse) * light)*field->leaf_rim_color.a)}, {0,0},
      };
    }
  }
  return true;
}
