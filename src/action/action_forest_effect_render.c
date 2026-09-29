/* Fillmore forest geometry. Phase: pure. */
#include "action/action_effect_render_internal.h"

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

bool AppendForestRays(
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
          if (!AppendSceneClippedTriangle(writer, &mesh, source, mapped, &cell[j],
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

bool AppendForestMotes(
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

bool AppendForestLeaves(
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
