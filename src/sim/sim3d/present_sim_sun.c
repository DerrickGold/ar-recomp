/* Sun miracle presentation. The native PPU ramp restores the missing scene
 * wash; optional shafts, ground light and descending motes share the captured
 * target square and logic clock. No renderer-owned lifetime or random state. */
#include "sim/sim3d/present_sim3d_effects.h"

#include <math.h>

#include "deterministic_hash.h"
#include "host/host_video.h"
#include "render/effect_batch.h"
#include "render/render_device.h"

static float SunStrength(const FrameSlot *slot) {
  const SimSunMiracle *sun = &slot->sim.sun_miracle;
  if (!sun->active || sun->phase >= kSimSunDuration) return 0;
  /* The native ramp rises for 60 ticks, holds for 20, and falls for 60.
   * Interpolate the enhancement instead of copying its four-tick steps. */
  return fminf(1, fminf(sun->phase, kSimSunDuration - sun->phase) / 60.0f);
}

bool DrawSimSunTint(const FrameSlot *slot, ArRenderRectI viewport) {
  const SimFrameData *sim = &slot->sim;
  /* Read the actual register ramp, including its zero endpoints. A stale
   * fixed-colour register with math disabled must never light a later frame. */
  if (!sim->sun_miracle.active || !sim->separated_valid || sim->separated_cgwsel != 0 ||
      sim->separated_cgadsub != 0x01 || sim->separated_brightness != 15 ||
      !sim->separated_fixed_color)
    return true;
  const unsigned color = sim->separated_fixed_color;
  const ArRenderRectF rect = {(float)viewport.x, (float)viewport.y, (float)viewport.w,
                              (float)viewport.h};
  return ArRenderDevice_DrawSolidRect(&g_render_device, &rect,
                                      (ArRenderColorF){(color & 31) / 31.0f,
                                                       ((color >> 5) & 31) / 31.0f,
                                                       ((color >> 10) & 31) / 31.0f, 1},
                                      kArRenderBlendMode_Add);
}

static bool SunProjectGround(const FrameSlot *slot, const SimSceneProjection *scene, float x,
                             float y, Scene3DPoint *point) {
  if (!scene || scene->source.h <= 0) return false;
  const float support = SimTerrainGroundHeightUnits(slot, x, y);
  if (scene->globe) {
    PresentSimGlobeProjectedPoint projected;
    if (!ProjectSimCurvedAnchor(slot, scene->globe, x, y, support, 0, false, &projected))
      return false;
    *point = projected.screen;
    return true;
  }
  return scene->matrix &&
         ProjectSimTexturePoint(scene->matrix, scene->source, scene->viewport,
                                x - slot->sim.camera_x + slot->ws_extra, y - slot->sim.camera_y,
                                SimTerrainHeightWorld(slot, scene->source, support), point);
}

static void AppendGridIndices(EffectBatch *batch, int base, int columns, int rows) {
  for (int y = 0; y < rows - 1; ++y) {
    for (int x = 0; x < columns - 1; ++x) {
      const int a = base + y * columns + x, b = a + columns;
      const int32_t indices[] = {a, a + 1, b + 1, a, b + 1, b};
      for (int i = 0; i < 6; ++i)
        batch->indices[batch->index_count++] = indices[i];
    }
  }
}

void DrawSimSunLight(const FrameSlot *slot, bool lighting, const SimSceneProjection *scene) {
  const float strength = SunStrength(slot);
  if (!lighting || strength <= 0 || !EffectRenderer_Available()) return;
  enum { kSide = 9, kVertices = kSide * kSide, kIndices = (kSide - 1) * (kSide - 1) * 6 };
  ArRenderVertex2D vertices[kVertices];
  int32_t indices[kIndices];
  EffectBatch batch = {.vertices = vertices,
                       .indices = indices,
                       .vertex_capacity = kVertices,
                       .index_capacity = kIndices};
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      const float u = (float)x / (kSide - 1), v = (float)y / (kSide - 1);
      Scene3DPoint p;
      if (!SunProjectGround(slot, scene, slot->sim.sun_miracle.target_x + u * kSimSunTargetPixels,
                            slot->sim.sun_miracle.target_y + v * kSimSunTargetPixels, &p))
        return;
      /* Feather inside the selected square, without lighting neighbouring cells. */
      const float edge = fminf(1, fminf(fminf(u, 1 - u), fminf(v, 1 - v)) * 8);
      vertices[batch.vertex_count++] = (ArRenderVertex2D){
          .position = {p.x, p.y}, .color = {1, .78f, .30f, .24f * edge * strength}};
    }
  }
  AppendGridIndices(&batch, 0, kSide, kSide);
  EffectRenderer_Submit(&g_render_device, &batch, kArRenderBlendMode_Add);
}

/* Use the grounded sprites' facing axes for the whole column. Projecting each
 * height as world Z makes the shaft lean away from its neighbouring billboards;
 * adding a fixed map-X slant also changes that lean whenever the camera turns. */
static bool SunAnchor(const FrameSlot *slot, const SimSceneProjection *scene, float x, float y,
                      Scene3DPoint *p, PresentSimGlobeBillboardAxes *axes) {
  if (!scene || scene->source.h <= 0) return false;
  const float support = SimTerrainGroundHeightUnits(slot, x, y);
  const float height_world = SimTerrainHeightWorld(slot, scene->source, support);
  if (scene->globe) {
    PresentSimGlobeProjectedPoint point;
    if (!ProjectSimCurvedAnchor(slot, scene->globe, x, y, support, 0, false, &point) ||
        !PresentSimGlobeProject_GroundBillboardAxes(scene->globe, x, y, &point, axes))
      return false;
    *p = point.screen;
  } else {
    float sx, sy;
    if (!scene->camera || !scene->matrix ||
        !ProjectSimAnchorAndScale(scene->matrix, scene->source, scene->viewport,
                                  x - slot->sim.camera_x + slot->ws_extra, y - slot->sim.camera_y,
                                  height_world, Scene3D_AutoFitDistance(scene->camera->fov_y), p,
                                  &sx, &sy))
      return false;
    *axes = (PresentSimGlobeBillboardAxes){.right = {sx, 0}, .down = {0, sy}};
  }
  const float pop = SimBillboardHeightPop(scene->source, height_world, slot->sim.height_pop_pct);
  axes->right.x *= pop;
  axes->right.y *= pop;
  axes->down.x *= pop;
  axes->down.y *= pop;
  return true;
}

static ArRenderPointF
SunBillboardPoint(Scene3DPoint anchor, const PresentSimGlobeBillboardAxes *axes, float x, float y) {
  return (ArRenderPointF){anchor.x + x * axes->right.x + y * axes->down.x,
                          anchor.y + x * axes->right.y + y * axes->down.y};
}

void DrawSimSunRays(const FrameSlot *slot, bool lighting, bool particles,
                    const SimSceneProjection *scene) {
  const float strength = SunStrength(slot);
  if ((!lighting && !particles) || strength <= 0 || !EffectRenderer_Available()) return;
  enum {
    kRays = 16,
    kRows = 6,
    kColumns = 3,
    kMotes = 24,
    kVertices = kRays * kRows * kColumns + kMotes * 4,
    kIndices = kRays * (kRows - 1) * (kColumns - 1) * 6 + kMotes * 6,
  };
  ArRenderVertex2D vertices[kVertices];
  int32_t indices[kIndices];
  EffectBatch batch = {.vertices = vertices,
                       .indices = indices,
                       .vertex_capacity = kVertices,
                       .index_capacity = kIndices};
  const SimSunMiracle *sun = &slot->sim.sun_miracle;
  if (lighting) {
    for (int ray = 0; ray < kRays; ++ray) {
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)ray + 97);
      const float jitter_x = (seed & 255) / 255.0f;
      const float jitter_y = ((seed >> 8) & 255) / 255.0f;
      const float variation = ((seed >> 16) & 255) / 255.0f;
      /* One jittered sample per X/Y stratum covers the square without lining
       * rear shafts up behind front ones. The coprime permutation spreads Y
       * across the full depth; fixed seeds keep paused redraws motionless. */
      const float spacing = (kSimSunTargetPixels - 8.0f) / kRays;
      const float x = sun->target_x + 4 + (ray + .2f + .6f * jitter_x) * spacing;
      const float y = sun->target_y + 4 + ((ray * 5) % kRays + .2f + .6f * jitter_y) * spacing;
      const float length = 76 + 40 * variation;
      const float width = .45f + .45f * jitter_y;
      const float flare = .5f + .5f * jitter_x;
      const float shimmer = .75f + .25f * sinf(sun->phase * .13f + ray * 2.4f);
      const float opacity = .18f + .08f * variation;
      Scene3DPoint anchor;
      PresentSimGlobeBillboardAxes axes;
      if (!SunAnchor(slot, scene, x, y, &anchor, &axes)) continue;
      const int base = batch.vertex_count;
      for (int row = 0; row < kRows; ++row) {
        const float t = (float)row / (kRows - 1);
        const float height = t * length;
        const float envelope = fmaxf(0, sinf(t * 3.14159265f));
        for (int col = 0; col < kColumns; ++col) {
          vertices[batch.vertex_count++] = (ArRenderVertex2D){
              .position =
                  SunBillboardPoint(anchor, &axes, (col - 1) * (width + t * flare), -height),
              .color = {1, .87f, .52f, col == 1 ? opacity * envelope * shimmer * strength : 0}};
        }
      }
      AppendGridIndices(&batch, base, kColumns, kRows);
    }
  }
  if (particles) {
    for (int mote = 0; mote < kMotes; ++mote) {
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)mote + 1);
      const float x = sun->target_x + 3 + (seed & 255) * (58.0f / 255);
      const float y = sun->target_y + 3 + ((seed >> 8) & 255) * (58.0f / 255);
      const float t = ((sun->phase + mote * 7) % 48) / 48.0f;
      const float height = (1 - t) * 80;
      Scene3DPoint anchor;
      PresentSimGlobeBillboardAxes axes;
      if (!SunAnchor(slot, scene, x, y, &anchor, &axes)) continue;
      const float alpha = sinf(t * 3.14159265f) * strength * .7f;
      const int base = batch.vertex_count;
      const float xy[4][2] = {{0, -2}, {1, 0}, {-1, 0}, {0, 2}};
      for (int i = 0; i < 4; ++i)
        vertices[batch.vertex_count++] = (ArRenderVertex2D){
            .position = SunBillboardPoint(anchor, &axes, xy[i][0] * .55f, xy[i][1] * .55f - height),
            .color = {1, .92f, .65f, alpha}};
      AppendGridIndices(&batch, base, 2, 2);
    }
  }
  if (batch.index_count) EffectRenderer_Submit(&g_render_device, &batch, kArRenderBlendMode_Add);
}
