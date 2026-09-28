/* Fillmore's cave water, authored drips and stone-interior atmosphere.
 * All positions belong to decoded room layers; no native actors, live WRAM,
 * persistent particle state, or platform-specific rendering is used here.
 * Phase: pure. Tests: tests/action_effect_render_test.c. */
#include "action/action_effect_render_internal.h"
#include "action_cave_surface.h"

/* One aggregate record per family; all bounds include the entire source map,
 * even though camera culling normally emits a small fraction of this work. */
_Static_assert((82 + 48) * 4 + 3 * 64 + 4 * kActionEffectGlowVertices <=
                   kActionSceneEffectRenderMaxVertices &&
               (82 + 48) * 6 + 3 * 96 + 4 * kActionEffectGlowIndices <=
                   kActionSceneEffectRenderMaxIndices,
               "cave water must fit the shared geometry scratch");
_Static_assert(15 * 64 + 25 * 13 * 4 <= kActionSceneEffectRenderMaxVertices &&
               15 * 96 + 25 * 13 * 6 <= kActionSceneEffectRenderMaxIndices &&
               96 * 7 <= kActionSceneEffectRenderMaxVertices &&
               96 * 15 <= kActionSceneEffectRenderMaxIndices,
               "cave atmosphere and clipped tower light must fit their layers");
_Static_assert(kActionLandingDustMaxPuffs *
                   (5 * kActionSceneEffectWaterfallMistCloudVertices + 6 * 4) +
                   22 * kActionSceneEffectWaterfallMistCloudVertices + 18 * 4 <=
                   kActionSceneEffectRenderMaxVertices &&
               kActionLandingDustMaxPuffs *
                   (5 * kActionSceneEffectWaterfallMistCloudIndices + 6 * 6) +
                   22 * kActionSceneEffectWaterfallMistCloudIndices + 18 * 6 <=
                   kActionSceneEffectRenderMaxIndices,
               "combined contact clouds, cave mist and grit must fit the geometry scratch");

static bool InCaveField(const ActionEffectInstance *effect, float x, float y, float margin) {
  const ActionEffectLocalRect *r = &effect->geometry.data.rect;
  x -= effect->world_x;
  y -= effect->world_y;
  return x >= r->x0 - margin && x <= r->x1 + margin &&
      y >= r->y0 - margin && y <= r->y1 + margin;
}

static bool CaveClip(const ActionEffectInstance *effect, ActionEffectClipBoundsFn clip_bounds,
    void *userdata, ActionEffectLocalRect *clip) {
  *clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata, effect, clip)) return false;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    clip->x0 = fmaxf(clip->x0, effect->clip_rect.x0);
    clip->y0 = fmaxf(clip->y0, effect->clip_rect.y0);
    clip->x1 = fminf(clip->x1, effect->clip_rect.x1);
    clip->y1 = fminf(clip->y1, effect->clip_rect.y1);
  }
  return RectIsSane(clip) && clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

static bool CaveQuad(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ArRenderPointF *world, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  ArRenderPointF points[4];
  for (int i = 0; i < 4; i++)
    if (!project_point(userdata, effect, world[i].x - effect->world_x,
            world[i].y - effect->world_y, &points[i]))
      return true;
  if (!Reserve(writer, 4, 6)) return false;
  const int base = writer->vertex_count;
  for (int i = 0; i < 4; i++)
    writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){points[i], color, {0,0}};
  const int corners[] = {0,1,2,0,2,3};
  for (int i = 0; i < 6; i++) writer->indices[writer->index_count++] = base + corners[i];
  return true;
}

static bool CaveDiamond(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float x, float y, float width, float height, float alpha,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!InCaveField(effect, x, y, 4) || alpha <= .005f) return true;
  const ArRenderPointF points[] = {{x-width,y}, {x,y-height}, {x+width,y}, {x,y+height}};
  return CaveQuad(writer, effect, points, (ArRenderColorF){.58f,.82f,1,alpha},
      project_point, userdata);
}

static bool CaveRipple(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float x, float y, float age, float strength,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!InCaveField(effect, x, y, 16) || age < 0 || age > 1) return true;
  const float rx = 1 + age * 12, ry = .35f + age * 1.5f;
  const float alpha = strength * (1-age) * fminf(1, age * 8);
  for (int i = 0; i < 32; i += 2) {
    const int j = (i + 2) & 31;
    const ArRenderPointF points[] = {
      {x+kCircle32[i][0]*rx, y+kCircle32[i][1]*ry},
      {x+kCircle32[j][0]*rx, y+kCircle32[j][1]*ry},
      {x+kCircle32[j][0]*(rx+.6f), y+kCircle32[j][1]*(ry+.3f)},
      {x+kCircle32[i][0]*(rx+.6f), y+kCircle32[i][1]*(ry+.3f)},
    };
    if (!CaveQuad(writer, effect, points, (ArRenderColorF){.42f,.76f,1,alpha},
            project_point, userdata))
      return false;
  }
  return true;
}

static bool CaveWater(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned pool = 0; pool < kActionCavePoolCount; pool++) {
    const ActionCaveWaterRegion *water = &kActionCavePools[pool];
    if (!InCaveField(effect, effect->world_x, water->surface_y, 12)) continue;
    for (int x = (int)water->left + 8; x < (int)water->right - 8; x += 16) {
      if (!InCaveField(effect, (float)x, water->surface_y, 12)) continue;
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)x ^ (pool + 1) * 0x9E3779B9u);
      const float t = ((effect->phase_ticks + seed) & 255u) / 256.0f;
      const float shimmer = sinf(t * 3.14159265f);
      const float y = water->surface_y + 3 + 5 * HashUnit(seed ^ 0x32u);
      if (!CaveDiamond(writer, effect, x + 3 * sinf(t * 6.2831853f), y,
              2 + 4 * HashUnit(seed), .45f, .62f * shimmer * shimmer,
              project_point, userdata))
        return false;
    }
    const uint32_t seed = DeterministicHash_Mix32((pool + 1) * 0x85EBCA6Bu);
    const float age = ((effect->phase_ticks + seed) & 255u) / 64.0f;
    const float x = water->left + 20 + (water->right - water->left - 40) * HashUnit(seed);
    if (!CaveRipple(writer, effect, x, water->surface_y + 5, age, .45f,
            project_point, userdata))
      return false;
  }
  for (unsigned fall = 0; fall < kActionCaveFallCount; fall++) {
    const ActionCaveWaterfall end = kActionCaveFalls[fall];
    for (int row = 0; row < (int)end.y; row += 48) {
      if (!InCaveField(effect, end.x, row + 24, 28)) continue;
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)row ^ (fall + 1) * 0xC2B2AE35u);
      const float t = ((effect->phase_ticks + seed) & 63u) / 64.0f;
      const float x = end.x - 9 + 18 * HashUnit(seed);
      const float y = row + t * 48;
      if (y >= end.y - 2) continue;
      if (!CaveDiamond(writer, effect, x, y, .6f, 2.5f,
              .40f * sinf(t * 3.14159265f), project_point, userdata))
        return false;
    }
    if (!InCaveField(effect, end.x, end.y, 24)) continue;
    const float pulse = .8f + .2f * sinf((effect->phase_ticks & 255u) * .024543693f + fall);
    const ActionEffectGlowStyle glow = {
      .radius_x = 23, .radius_y = 9, .ring_scale = {.12f,.5f,1}, .axis_x = 1,
      .centre = {.4f,.75f,1,.26f}, .ring = {{.35f,.7f,1,.20f},{.25f,.55f,1,.07f},{0,0,0,0}},
    };
    if (!AppendGlow(writer, effect, &glow, pulse, end.x - effect->world_x,
            end.y + 3 - effect->world_y, project_point, userdata))
      return false;
  }
  return true;
}

static bool CaveDrips(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < kActionCaveWetSourceCount; i++) {
    const ActionCaveWetSource *source = &kActionCaveWetSources[i];
    if (!(effect->source_mask & (1u << i))) continue;
    const uint32_t seed = DeterministicHash_Mix32((i + 1) * 0x85EBCA6Bu);
    const unsigned mask = (seed & 1u) ? 511u : 255u;
    const unsigned age = (effect->phase_ticks + seed) & mask;
    if (age < 24) {
      if (!CaveDiamond(writer, effect, source->x, source->ceiling_y,
              .25f + age * .02f, .4f + age * .045f, .55f,
              project_point, userdata)) return false;
    } else if (age < 56) {
      const float t = (age - 24) / 32.0f;
      const float y = source->ceiling_y + t * t * (source->landing_y - source->ceiling_y);
      if (!CaveDiamond(writer, effect, source->x, y, .7f, 1.2f + 2*t, .74f,
              project_point, userdata)) return false;
    } else if (age < 80) {
      const float t = (age - 56) / 24.0f;
      if (source->water) {
        if (!CaveRipple(writer, effect, source->x, source->landing_y + 3, t, .64f,
                project_point, userdata)) return false;
      } else {
        for (int side = -1; side <= 1; side += 2)
          if (!CaveDiamond(writer, effect, source->x + side * t * 4,
                  source->landing_y - 6*t*(1-t), .55f, .6f, .50f*(1-t),
                  project_point, userdata)) return false;
      }
    }
  }
  return true;
}

static bool TempleAir(unsigned room, float x, float y) {
  if (room == 2) return x > 1160 && y > 350;
  if (room == 3) return (x > 840 && x < 968) || (y > 80 && y < 190) || y > 1570;
  return x > 80 && x < 344 && y > 96 && y < 206;
}

typedef struct CaveAmbientSource {
  unsigned room;
  float x, y, radius_x, radius_y, lean, strength;
} CaveAmbientSource;

/* The entrance, waterfall fissure and upper cavern are open at the map top.
 * Those gaps motivate stronger upper light, with softer, weaker fill deeper
 * in the cave/temple. These broad world pools suggest offscreen illumination;
 * surface blending lights stone and actors while preserving black recesses. */
static const CaveAmbientSource kCaveAmbientSources[] = {
  {2,64,160,96,248,-.10f,.44f}, {2,760,192,118,232,-.06f,.46f},
  {2,1110,176,232,202,-.15f,.68f},
  {2,656,836,220,132,-.18f,.30f}, {2,1030,646,156,158,-.08f,.28f},
  {2,1500,166,180,150,-.18f,.74f}, {2,1856,438,184,212,-.24f,.64f},
  {2,1560,1055,185,220,.12f,.46f},
  /* Broad reflected light against the temple's dimmed stone. Keep the soft
   * falloff and neutral tint; these do not imply openings or hard beams. */
  {3,640,1548,600,430,0,.82f}, {3,900,1120,260,360,0,.68f},
  {3,872,640,270,350,0,.76f}, {3,576,144,540,240,0,.86f},
};

static float AmbientFalloff(float offset) {
  const float q = fmaxf(0, 1-offset*offset);
  return q*q*q;
}

static float TempleExposure(unsigned room, float x, float y) {
  float light = 0;
  for (unsigned i = 0; i < sizeof(kCaveAmbientSources)/sizeof(kCaveAmbientSources[0]); i++) {
    const CaveAmbientSource *s = &kCaveAmbientSources[i];
    if (s->room != room) continue;
    const float dy = y-s->y;
    const float along = AmbientFalloff(dy/s->radius_y);
    const float across = AmbientFalloff((x-s->x-dy*s->lean)/s->radius_x);
    light = fmaxf(light, along*across*s->strength);
  }
  return light;
}

_Static_assert(kActionTempleMistMaxSpans*2*48*7 <= kActionSceneEffectRenderMaxVertices &&
               kActionTempleMistMaxSpans*2*48*15 <= kActionSceneEffectRenderMaxIndices,
               "clipped temple ground mist must fit the shared alpha batch");

static bool TempleGroundMist(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect, clip_bounds, userdata, &clip)) return true;
  /* Capture publishes one flat, safe collision run per record. Split at
   * steps and pits before rendering, so no triangle bridges a height change.
   * Animate the upper edge while keeping the dense base on the actual floor. */
  const float half_width = effect->geometry.data.rect.x1*.5f;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  for (unsigned puff = 0; puff < 2; puff++) {
    const uint32_t seed = DeterministicHash_Mix32(
        ((uint32_t)(uint16_t)effect->world_x*2+puff+1)*0x9E3779B9u);
    const float phase = ((effect->phase_ticks + seed) & 1023u)*.006135923f;
    const float cx = half_width*(1 + (puff ? .08f : -.08f) + .08f*sinf(phase));
    const float width = half_width*(.72f+.10f*sinf(phase-.6f));
    const float height = 9 + 3*sinf(phase+.8f);
    if (cx+width <= clip.x0 || cx-width >= clip.x1 ||
        clip.y0 >= 0 || clip.y1 <= -2*height) continue;
    const float opacity = (.34f+.08f*HashUnit(seed))*(.90f+.10f*sinf(phase));
    const float light = .20f*TempleExposure(3, effect->world_x+cx, effect->world_y-height);
    ArRenderVertex2D vertices[35];
    int mapped[35];
    for (int row = 0; row < 5; row++) {
      const float rise = 1-row*.25f;
      for (int col = 0; col < 7; col++) {
        const float u = (col-3)/3.0f;
        const int at = row*7+col;
        vertices[at] = (ArRenderVertex2D){
          {cx+u*width, -2*height*rise},
          {.47f+light,.65f+light,.82f+light,
           opacity*AmbientFalloff(u)*AmbientFalloff(rise)}, {0,0},
        };
        mapped[at] = -1;
      }
    }
    for (int row = 0; row < 4; row++) {
      for (int col = 0; col < 6; col++) {
        const int a = row*7+col, b = a+7;
        const int triangles[] = {a,a+1,b,a+1,b+1,b};
        for (int t = 0; t < 6; t += 3)
          if (!AppendSceneClippedTriangle(writer, &mesh, vertices, mapped, &triangles[t],
                  &clip, project_point, userdata)) return false;
      }
    }
  }
  return true;
}

static bool CaveMist(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < kActionCaveFallCount; i++) {
    const ActionCaveWaterfall p = kActionCaveFalls[i];
    if (!InCaveField(effect, p.x, p.y, 72)) continue;
    for (unsigned puff = 0; puff < 4; puff++) {
      const uint32_t seed = DeterministicHash_Mix32((i*4+puff+1)*0x9E3779B9u);
      const float t = ((effect->phase_ticks + puff*64 + (seed & 31u)) & 255u)/256.0f;
      const float drift = (puff & 1u ? 1 : -1) * (4 + 25*t) * (.7f+.5f*HashUnit(seed));
      const float height = 3 + 5*t;
      const float fade = sinf(t*3.14159265f);
      if (!AppendSceneSoftCloud(writer, effect, p.x+drift-effect->world_x,
              p.y+3-2*t-height*.25f-effect->world_y, 8+15*t, height,
              (ArRenderColorF){.44f,.66f,.84f,1}, (.52f+.16f*HashUnit(seed))*fade*fade,
              seed, project_point, userdata)) return false;
    }
  }
  return true;
}

static bool CaveSheen(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < kActionCaveWetSourceCount; i++) {
    const ActionCaveWetSource *s = &kActionCaveWetSources[i];
    if (!(effect->source_mask & (1u << i)) || s->water ||
        !InCaveField(effect, s->x, s->landing_y, 16)) continue;
    const uint32_t seed = DeterministicHash_Mix32((i+1)*0xC2B2AE35u);
    const float phase = ((effect->phase_ticks + seed) & 511u)/512.0f;
    const float shimmer = sinf(phase*3.14159265f);
    for (unsigned column = 0; column < 13; column++) {
      if (s->contour[column] == 127) continue;
      const float x = s->x + (int)column - 6;
      const float y = s->landing_y + s->contour[column] + .6f;
      const float grain = HashUnit(seed ^ (column+1)*0x85EBCA6Bu);
      /* Subpixel highlights sit INSIDE each opaque edge column. Never bridge
       * a stair step or open column with a horizontal diamond/line segment. */
      if (!CaveDiamond(writer, effect, x+.5f, y, .45f, .35f,
              (.20f+.40f*shimmer*shimmer)*(.35f+.65f*grain),
              project_point, userdata)) return false;
    }
  }
  return true;
}

typedef struct TempleGritSource {
  unsigned room;
  float x, ceiling_y, landing_y;
} TempleGritSource;

/* Outside the central column's solid artwork, with a clear drop onto the
 * capital below. No path ends on the blue spike clusters. */
static const TempleGritSource kTempleGrit[] = {
  {2,1710,655,768},
  {3,921,1598,1664}, {3,921,1344,1440}, {3,919,1040,1216}, {3,919,384,480},
};

static bool TempleGrit(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < sizeof(kTempleGrit)/sizeof(kTempleGrit[0]); i++) {
    const TempleGritSource *s = &kTempleGrit[i];
    if (s->room != effect->visual) continue;
    const uint32_t seed = DeterministicHash_Mix32((i+1)*0x85EBCA6Bu);
    const unsigned age = (effect->phase_ticks + seed) & 511u;
    for (unsigned grain = 0; grain < 3; grain++) {
      if (age < grain*5 || age >= grain*5+48) continue;
      const float t = (age-grain*5)/48.0f;
      const float x = s->x + (int)grain - 1;
      const float y = s->ceiling_y + (s->landing_y-s->ceiling_y)*t*t;
      if (!InCaveField(effect, x, y, 3)) continue;
      const float r = .5f + grain*.13f;
      const ArRenderPointF points[] = {{x-r,y}, {x,y-1.3f}, {x+r,y}, {x,y+.7f}};
      if (!CaveQuad(writer, effect, points, (ArRenderColorF){.72f,.52f,.32f,.78f},
              project_point, userdata)) return false;
    }
    if (age < 48 || age >= 78 || !InCaveField(effect, s->x, s->landing_y, 12)) continue;
    const float t = (age-48)/30.0f;
    const float h = 3+2*t;
    if (!AppendSceneSoftCloud(writer, effect, s->x-effect->world_x,
            s->landing_y-h*1.15f-t-effect->world_y, 3+7*t, h,
            (ArRenderColorF){.62f,.43f,.26f,1}, .6f*sinf(t*3.14159265f),
            seed, project_point, userdata)) return false;
  }
  return true;
}

_Static_assert(8*72*7 <= kActionSceneEffectRenderMaxVertices &&
               8*72*15 <= kActionSceneEffectRenderMaxIndices,
               "all clipped ambient pools must fit the shared surface-light batch");

static bool CaveAmbientLight(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect, clip_bounds, userdata, &clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float phase = (effect->phase_ticks & 1023u)*.006135923f;
  /* The temple's dark stone needs contrast, but its authored sprite palettes
   * are already bright. Keep surface gain restrained; mist exposure uses the
   * original field separately so its visibility does not require blown highlights. */
  const float surface_response = effect->visual == 3 ? .40f : 1;
  for (unsigned i = 0; i < sizeof(kCaveAmbientSources)/sizeof(kCaveAmbientSources[0]); i++) {
    const CaveAmbientSource *s = &kCaveAmbientSources[i];
    const float cx = s->x-effect->world_x, cy = s->y-effect->world_y;
    const float reach_x = s->radius_x + fabsf(s->lean)*s->radius_y;
    if (s->room != effect->visual || cx+reach_x <= clip.x0 || cx-reach_x >= clip.x1 ||
        cy+s->radius_y <= clip.y0 || cy-s->radius_y >= clip.y1) continue;
    /* Nearly steady light; only a very slow, small intensity variation. */
    const float drift = effect->visual == 3 ? 1 : .98f + .02f*sinf(phase+i*1.7f);
    const ArRenderColorF tint = effect->visual == 3 ?
        (ArRenderColorF){.72f,.75f,.79f,1} : (ArRenderColorF){.62f,.74f,.88f,1};
    ArRenderVertex2D vertices[49];
    int mapped[49];
    for (int row = 0; row < 7; row++) {
      const float v = (row-3)/3.0f, dy = v*s->radius_y;
      const float along = AmbientFalloff(v)*s->strength*drift*surface_response;
      for (int col = 0; col < 7; col++) {
        const float u = (col-3)/3.0f;
        const float amount = along*AmbientFalloff(u);
        const int at = row*7+col;
        vertices[at] = (ArRenderVertex2D){
          {cx + dy*s->lean + u*s->radius_x, cy + dy},
          {tint.r*amount,tint.g*amount,tint.b*amount,1}, {0,0},
        };
        mapped[at] = -1;
      }
    }
    for (int row = 0; row < 6; row++) {
      for (int col = 0; col < 6; col++) {
        const int a = row*7+col, b = a+7;
        const int triangles[] = {a,a+1,b,a+1,b+1,b};
        for (int t = 0; t < 6; t += 3)
          if (!AppendSceneClippedTriangle(writer, &mesh, vertices, mapped, &triangles[t],
                  &clip, project_point, userdata)) return false;
      }
    }
  }
  return true;
}

static bool TempleDust(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  /* A narrow shaft needs several readable flecks at once. Jittered cells
   * retain world anchoring while varied size/opacity avoid a uniform snowfield.
   * One quad per cell keeps the denser field inside the existing batch. */
  const int first_x = (int)floorf((effect->world_x - 384) / 32.0f);
  const int first_y = (int)floorf(effect->world_y / 48.0f);
  for (int iy = 0; iy < 13; iy++) {
    for (int ix = 0; ix < 25; ix++) {
      const int cx = first_x + ix, cy = first_y + iy;
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)cx * 0x9E3779B9u ^
          (uint32_t)cy * 0x85EBCA6Bu ^ effect->pulse_generation);
      if ((seed & 3u) == 0) continue;
      const float t = ((effect->phase_ticks + seed) & 1023u) / 1024.0f;
      const float x = cx * 32 + 8 + 16 * HashUnit(seed) + 4 * sinf(t * 6.2831853f);
      const float y = cy * 48 + 12 + 28 * HashUnit(seed ^ 0x17u) - 8*t;
      if (!TempleAir(effect->visual, x, y)) continue;
      const float fade = sinf(t * 3.14159265f);
      const float radius = .75f + .55f * HashUnit(seed ^ 0x59u);
      const float exposure = TempleExposure(effect->visual, x, y);
      const float alpha = fminf(1, .50f + .28f*HashUnit(seed ^ 0xA3u) + .30f*exposure) *
          fade * fade;
      if (effect->visual == 3) {
        if (!InCaveField(effect, x, y, 4)) continue;
        const ArRenderPointF points[] = {
          {x-radius,y}, {x,y-radius*.8f}, {x+radius,y}, {x,y+radius*.8f},
        };
        /* Warm mineral dust reads against the cool masonry; the damp lower hall
         * has fewer bright flecks competing with its ground mist. */
        const float density = y > 1570 ? .62f : 1;
        if (!CaveQuad(writer, effect, points, (ArRenderColorF){.80f,.64f,.43f,alpha*density},
                project_point, userdata)) return false;
        continue;
      }
      const ArRenderPointF points[] = {
        {x-radius,y}, {x,y-radius*.8f}, {x+radius,y}, {x,y+radius*.8f},
      };
      if (!CaveQuad(writer, effect, points, (ArRenderColorF){.80f,.64f,.43f,alpha},
              project_point, userdata)) return false;
    }
  }
  return true;
}

static bool LandingDust(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const uint32_t burst = DeterministicHash_Mix32(effect->generation ^
      ((uint32_t)(uint16_t)effect->world_x << 16) ^ (uint16_t)effect->world_y);
  const float lifetime = kActionLandingDustLifetime - (burst & 15u);
  const float t = effect->phase_ticks / lifetime;
  if (t >= 1) return true;
  const float scale = .75f + effect->visual * .35f;
  const float breeze = (HashUnit(burst ^ 0xC7u)-.5f)*18;
  const unsigned count = 3 + (burst % 3u);
  for (unsigned i = 0; i < count; i++) {
    const uint32_t seed = DeterministicHash_Mix32(burst ^ (i+1)*0x9E3779B9u);
    const float delay = i ? .16f*HashUnit(seed ^ 0xD3u) : 0;
    const float age = (t-delay)/(1-delay);
    if (age <= 0) continue;
    const float expand = age*(2-age);
    const float fade = fminf(1, age*(12+18*HashUnit(seed))) * (1-age*age);
    const float direction = 2*HashUnit(seed ^ 0x93u)-1;
    const float shape = .70f+.55f*HashUnit(seed ^ 0x51u);
    const float spread = (direction*(2+23*expand) + breeze*age*age)*scale;
    const float height = fmaxf(2.5f, (1.2f+(4+5*HashUnit(seed ^ 0xB7u))*age)*scale*shape);
    const float lift = (1+4*HashUnit(seed ^ 0xAFu))*age*age;
    if (!AppendSceneSoftCloud(writer, effect, spread, -height*1.15f-lift,
            (5+(8+7*HashUnit(seed ^ 0x29u))*expand)*scale*shape, height,
            (ArRenderColorF){.62f,.43f,.26f,1},
            (.70f+.20f*HashUnit(seed))*fade, seed, project_point, userdata)) return false;
  }
  for (unsigned i = 0; i < 6; i++) {
    const uint32_t seed = DeterministicHash_Mix32(burst ^ (i+9)*0x85EBCA6Bu);
    const float life = .45f+.45f*HashUnit(seed ^ 0xB5u);
    const float age = t/life;
    if (age >= 1) continue;
    const float x = (2*HashUnit(seed)-1)*(3+24*age)*scale;
    const float y = -(1+(8+12*HashUnit(seed ^ 0x31u))*age*(1-age))*scale;
    const float r = .4f+.4f*HashUnit(seed ^ 0x61u);
    const ArRenderPointF points[] = {
      {effect->world_x+x-r,effect->world_y+y},
      {effect->world_x+x,effect->world_y+y-r},
      {effect->world_x+x+r,effect->world_y+y},
      {effect->world_x+x,effect->world_y+y+r},
    };
    if (!CaveQuad(writer, effect, points,
            (ArRenderColorF){.76f,.55f,.32f,.6f*(1-age)*fminf(1,age*24)},
            project_point, userdata)) return false;
  }
  return true;
}

static bool TowerLight(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect, clip_bounds, userdata, &clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  /* Star-facing arch openings in 01/04. Narrow at the arch, broader at the
   * battle floor, then fade within the actual stone apron. Surface blending
   * preserves black voids and lets the moving Minotaur catch the same light. */
  static const float rows[] = {88,112,180,207,232};
  static const float widths[] = {12,17,30,39,44};
  static const float strengths[] = {0,.65f,1,.8f,0};
  const float phase = (effect->phase_ticks & 1023u)*.006135923f;
  for (int window = 0; window < 3; window++) {
    /* Slow common cloud shadow, with a smaller staggered window response.
     * Neither the ray position nor the battle-floor footprint moves. */
    const float clouds = .82f + .13f*sinf(phase) + .05f*sinf(2*phase+window*.8f);
    ArRenderVertex2D source[25];
    int mapped[25];
    for (int row = 0; row < 5; row++) {
      for (int col = 0; col < 5; col++) {
        const float across = col == 2 ? 1 : (col == 0 || col == 4 ? 0 : .55f);
        const float amount = strengths[row] * across * clouds;
        const float y = rows[row], x = 112 + window * 80 + (y-88) * .18f;
        const int index = row * 5 + col;
        source[index] = (ArRenderVertex2D){
          {x + (col*.5f-1)*widths[row] - effect->world_x, y - effect->world_y},
          {.28f*amount,.43f*amount,.70f*amount,1}, {0,0},
        };
        mapped[index] = -1;
      }
    }
    for (int row = 0; row < 4; row++) {
      for (int col = 0; col < 4; col++) {
        const int a = row*5+col, b = a+5;
        const int triangles[] = {a,a+1,b,a+1,b+1,b};
        for (int i = 0; i < 6; i += 3)
          if (!AppendSceneClippedTriangle(writer, &mesh, source, mapped, &triangles[i],
                  &clip, project_point, userdata)) return false;
      }
    }
  }
  return true;
}

bool AppendCaveEnvironment(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  switch (effect->kind) {
    case kActionEffect_TempleGroundMist:
      return TempleGroundMist(writer, effect, project_point, clip_bounds, userdata);
    case kActionEffect_CaveMist: return CaveMist(writer, effect, project_point, userdata);
    case kActionEffect_CaveSheen: return CaveSheen(writer, effect, project_point, userdata);
    case kActionEffect_TempleGrit: return TempleGrit(writer, effect, project_point, userdata);
    case kActionEffect_CaveAmbientLight:
      return CaveAmbientLight(writer, effect, project_point, clip_bounds, userdata);
    case kActionEffect_LandingDust: return LandingDust(writer, effect, project_point, userdata);
    case kActionEffect_CaveWater: return CaveWater(writer, effect, project_point, userdata);
    case kActionEffect_CaveDrips: return CaveDrips(writer, effect, project_point, userdata);
    case kActionEffect_TempleDust: return TempleDust(writer, effect, project_point, userdata);
    case kActionEffect_TowerWindowLight:
      return TowerLight(writer, effect, project_point, clip_bounds, userdata);
    default: return false;
  }
}
