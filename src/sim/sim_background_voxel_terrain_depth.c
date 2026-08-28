#include "sim_background_voxel_terrain_depth.h"

#if AR_SIM3D_TERRAIN_ELEVATION

#include <float.h>
#include <string.h>

#include "constants.h"
#include "scene3d_math.h"
#include "sim3d_depth_pass.h"
#include "sim_background_voxel_project.h"
#include "sim_town_terrain.h"

typedef struct SimTerrainDepthCell {
  float height[kSimTownTerrainCornerCount];
  uint8_t hard_edges;
} SimTerrainDepthCell;

enum {
  /* Every terrain cell contributes one top and at most one skirt per edge.
   * The projection cache is deliberately bounded by that geometric maximum,
   * so caching can never turn malformed terrain metadata into an allocation
   * or an incomplete draw. */
  kMaxTerrainDepthQuads = kSimTownTerrainCells *
      kSimTownTerrainCells * 5,
};

typedef struct SimTerrainProjectedQuad {
  Scene3DPoint points[4];
  float depth[4];
  float clip_depth[4];
  bool receives_shadow;
} SimTerrainProjectedQuad;

typedef struct SimTerrainProjectionKey {
  uint8_t town;
  uint8_t render_scale;
  uint16_t landscape_height_pct;
  uint16_t camera_x, camera_y;
  uint16_t town_screen_x0;
  ArRenderRectI source;
  ArRenderRectI viewport;
  float matrix[16];
} SimTerrainProjectionKey;

static struct {
  bool valid;
  uint8_t town;
  SimTerrainDepthCell cell[
      kSimTownTerrainCells * kSimTownTerrainCells];
} s_terrain_depth_cache;

/* The shadow receiver and the main depth composite use the same camera and
 * terrain mesh, and a still camera reuses it again on following frames. Keep
 * the projected result so those passes only restage vertices; models remain
 * dynamic and continue to project normally. */
static struct {
  bool valid;
  bool overflow;
  int count;
  float minimum_clip_depth;
  float maximum_clip_depth;
  SimTerrainProjectionKey key;
  SimTerrainProjectedQuad quads[kMaxTerrainDepthQuads];
} s_terrain_projection_cache;

static SimTerrainDepthCell *TerrainDepthCell(int x, int y) {
  return &s_terrain_depth_cache.cell[
      y * kSimTownTerrainCells + x];
}

static void PrepareTerrainDepthCache(uint8_t town) {
  if (s_terrain_depth_cache.valid && s_terrain_depth_cache.town == town)
    return;
  for (int y = 0; y < kSimTownTerrainCells; y++)
    for (int x = 0; x < kSimTownTerrainCells; x++) {
      SimTerrainDepthCell *cell = TerrainDepthCell(x, y);
      cell->hard_edges = SimTownTerrain_HardEdges(town, x, y);
      for (int corner = 0; corner < kSimTownTerrainCornerCount; corner++)
        cell->height[corner] = SimTownTerrain_CornerUnits(
            town, x, y, corner);
    }
  s_terrain_depth_cache.town = town;
  s_terrain_depth_cache.valid = true;
}

static bool TerrainProjectionKeyEquals(
    const SimTerrainProjectionKey *key,
    const SimBackgroundVoxelRenderParams *params) {
  return key && params &&
      key->town == params->town &&
      key->render_scale == params->render_scale &&
      key->landscape_height_pct == params->landscape_height_pct &&
      key->camera_x == params->camera_x &&
      key->camera_y == params->camera_y &&
      key->town_screen_x0 == params->town_screen_x0 &&
      key->source.x == params->source.x &&
      key->source.y == params->source.y &&
      key->source.w == params->source.w &&
      key->source.h == params->source.h &&
      key->viewport.x == params->viewport.x &&
      key->viewport.y == params->viewport.y &&
      key->viewport.w == params->viewport.w &&
      key->viewport.h == params->viewport.h &&
      memcmp(key->matrix, params->matrix, sizeof(key->matrix)) == 0;
}

static bool TerrainProjectionKeyMatches(
    const SimBackgroundVoxelRenderParams *params) {
  return s_terrain_projection_cache.valid &&
      TerrainProjectionKeyEquals(&s_terrain_projection_cache.key, params);
}

static void SetTerrainProjectionKey(
    SimTerrainProjectionKey *key,
    const SimBackgroundVoxelRenderParams *params) {
  *key = (SimTerrainProjectionKey){
    .town = params->town,
    .render_scale = params->render_scale,
    .landscape_height_pct = params->landscape_height_pct,
    .camera_x = params->camera_x,
    .camera_y = params->camera_y,
    .town_screen_x0 = params->town_screen_x0,
    .source = params->source,
    .viewport = params->viewport,
  };
  memcpy(key->matrix, params->matrix, sizeof(key->matrix));
}

static void BeginTerrainProjectionCache(
    const SimBackgroundVoxelRenderParams *params) {
  s_terrain_projection_cache.valid = false;
  s_terrain_projection_cache.overflow = false;
  s_terrain_projection_cache.count = 0;
  s_terrain_projection_cache.minimum_clip_depth = FLT_MAX;
  s_terrain_projection_cache.maximum_clip_depth = -FLT_MAX;
  SetTerrainProjectionKey(&s_terrain_projection_cache.key, params);
}

static void EmitTerrainDepthQuad(
    const SimBackgroundVoxelRenderParams *params,
    const SimTerrainProjectedQuad *quad) {
  Sim3DDepthVertex vertices[4];
  for (int point = 0; point < 4; point++) {
    vertices[point] = (Sim3DDepthVertex){
      .x = quad->points[point].x,
      .y = quad->points[point].y,
      .depth = quad->depth[point],
      /* The shared fragment shader discards zero alpha before depth can be
       * written. Color writes are disabled by the occluder pipeline, so an
       * opaque white fragment is invisible while still reaching D32. */
      .color = {1.0f, 1.0f, 1.0f, 1.0f},
      .uv = {0.0f, 0.0f},
    };
  }
  Sim3DDepthPass_AppendQuad(kSim3DDepthPass_DepthOccluder, vertices);
  if (!quad->receives_shadow ||
      !ArRenderTexture_IsValid(params->shadow_mask) ||
      !params->shadow_opacity_pct)
    return;
  const float alpha =
      params->shadow_opacity_pct / (float)kPercentScale;
  for (int point = 0; point < 4; point++) {
    vertices[point].color = (ArRenderColorF){1.0f, 1.0f, 1.0f, alpha};
    vertices[point].uv = (ArRenderPointF){
      quad->points[point].x / params->viewport.w,
      quad->points[point].y / params->viewport.h,
    };
  }
  Sim3DDepthPass_AppendQuad(kSim3DDepthPass_ShadowReceiver, vertices);
}

static void CacheTerrainDepthQuad(
    const SimBackgroundVoxelRenderParams *params,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float height_units[4], bool receives_shadow) {
  SimTerrainProjectedQuad quad = {.receives_shadow = receives_shadow};
  for (int point = 0; point < 4; point++) {
    const float texture_x = origin_x + local_x[point];
    const float texture_y = origin_y + local_y[point];
    const float terrain_pixels =
        SimBackgroundVoxelProject_TerrainUnitsToPixels(
            params, height_units[point]);
    if (!SimBackgroundVoxelProject_GroundedVertex(
            params, &kSimBackgroundUprightProjectionAxis,
            texture_x, texture_y, 0.0f, terrain_pixels,
            &quad.points[point], &quad.depth[point]))
      return;
    float world_x, world_y, world_z;
    SimBackgroundVoxelProject_TexturePointToWorld(
        params, texture_x, texture_y, terrain_pixels,
        &world_x, &world_y, &world_z);
    quad.clip_depth[point] = Scene3D_ClipDepth(
        params->matrix, world_x, world_y, world_z);
  }
  if (SimBackgroundVoxelProject_IsDegenerate(quad.points)) return;
  if (s_terrain_projection_cache.count < kMaxTerrainDepthQuads) {
    s_terrain_projection_cache.quads[
        s_terrain_projection_cache.count++] = quad;
    for (int point = 0; point < 4; point++) {
      if (quad.clip_depth[point] <
          s_terrain_projection_cache.minimum_clip_depth)
        s_terrain_projection_cache.minimum_clip_depth =
            quad.clip_depth[point];
      if (quad.clip_depth[point] >
          s_terrain_projection_cache.maximum_clip_depth)
        s_terrain_projection_cache.maximum_clip_depth =
            quad.clip_depth[point];
    }
  } else {
    s_terrain_projection_cache.overflow = true;
  }
}

static void AppendTerrainDepthSkirt(
    const SimBackgroundVoxelRenderParams *params,
    float origin_x, float origin_y,
    float x0, float y0, float x1, float y1,
    float current_0, float current_1,
    float neighbour_0, float neighbour_1) {
  float t0, t1;
  if (!SimTownTerrain_ClipVisibleHigherEdge(
          current_0, current_1, neighbour_0, neighbour_1,
          &t0, &t1))
    return;
  const float t[2] = {t0, t1};
  float xx[4], yy[4], hh[4];
  for (int endpoint = 0; endpoint < 2; endpoint++) {
    const float at = t[endpoint];
    const float x = x0 + (x1 - x0) * at;
    const float y = y0 + (y1 - y0) * at;
    xx[endpoint] = xx[3 - endpoint] = x;
    yy[endpoint] = yy[3 - endpoint] = y;
    hh[endpoint] = current_0 + (current_1 - current_0) * at;
    hh[3 - endpoint] =
        neighbour_0 + (neighbour_1 - neighbour_0) * at;
  }
  CacheTerrainDepthQuad(
      params, origin_x, origin_y, xx, yy, hh, false);
}

static void BuildTerrainProjectionCache(
    const SimBackgroundVoxelRenderParams *params) {
  if (!params || params->town < 1 ||
      params->town > kSimTownTerrainTownCount)
    return;
  BeginTerrainProjectionCache(params);
  PrepareTerrainDepthCache(params->town);
  const float origin_x = (float)params->town_screen_x0 - params->camera_x;
  const float origin_y = -(float)params->camera_y;
  for (int y = 0; y < kSimTownTerrainCells; y++) {
    for (int x = 0; x < kSimTownTerrainCells; x++) {
      const float x0 = x * (float)kSimTownTerrainCellPixels;
      const float y0 = y * (float)kSimTownTerrainCellPixels;
      const float x1 = x0 + kSimTownTerrainCellPixels;
      const float y1 = y0 + kSimTownTerrainCellPixels;
      const SimTerrainDepthCell *cell = TerrainDepthCell(x, y);
      const float *h = cell->height;
      const uint8_t hard_edges = cell->hard_edges;

      if (y > 0 && (hard_edges & kSimTownTerrainEdgeNorth)) {
        const float *n = TerrainDepthCell(x, y - 1)->height;
        float n0 = n[kSimTownTerrainCornerSW];
        float n1 = n[kSimTownTerrainCornerSE];
        AppendTerrainDepthSkirt(
            params, origin_x, origin_y, x1, y0, x0, y0,
            h[1], h[0], n1, n0);
      }
      if (x > 0 && (hard_edges & kSimTownTerrainEdgeWest)) {
        const float *n = TerrainDepthCell(x - 1, y)->height;
        float n0 = n[kSimTownTerrainCornerNE];
        float n1 = n[kSimTownTerrainCornerSE];
        AppendTerrainDepthSkirt(
            params, origin_x, origin_y, x0, y0, x0, y1,
            h[0], h[3], n0, n1);
      }
      if (y + 1 < kSimTownTerrainCells &&
          (hard_edges & kSimTownTerrainEdgeSouth)) {
        const float *n = TerrainDepthCell(x, y + 1)->height;
        float n0 = n[kSimTownTerrainCornerNW];
        float n1 = n[kSimTownTerrainCornerNE];
        AppendTerrainDepthSkirt(
            params, origin_x, origin_y, x0, y1, x1, y1,
            h[3], h[2], n0, n1);
      }
      if (x + 1 < kSimTownTerrainCells &&
          (hard_edges & kSimTownTerrainEdgeEast)) {
        const float *n = TerrainDepthCell(x + 1, y)->height;
        float n0 = n[kSimTownTerrainCornerNW];
        float n1 = n[kSimTownTerrainCornerSW];
        AppendTerrainDepthSkirt(
            params, origin_x, origin_y, x1, y0, x1, y1,
            h[1], h[2], n0, n1);
      }
      const float xx[4] = {x0, x1, x1, x0};
      const float yy[4] = {y0, y0, y1, y1};
      CacheTerrainDepthQuad(
          params, origin_x, origin_y, xx, yy, h, true);
    }
  }
  s_terrain_projection_cache.valid =
      !s_terrain_projection_cache.overflow;
}

/* The visible terrain mesh is textured in present_sim3d.c. Re-submit its
 * exact top surface and exposed cliff skirts here as depth-only geometry, so
 * a nearer ridge can reject a house or landmark on the far side without
 * covering the already-rendered town texture or the billboard actor bands. */
void SimBackgroundVoxelTerrainDepth_Append(
    const SimBackgroundVoxelRenderParams *params) {
  if (!params || params->town < 1 ||
      params->town > kSimTownTerrainTownCount)
    return;
  if (!TerrainProjectionKeyMatches(params))
    BuildTerrainProjectionCache(params);
  if (!TerrainProjectionKeyMatches(params)) return;
  for (int i = 0; i < s_terrain_projection_cache.count; i++)
    EmitTerrainDepthQuad(params, &s_terrain_projection_cache.quads[i]);
}

/* Actor-band clipping uses the clip-W extrema of the exact projected quads.
 * When shadows are enabled that projection was already built by the receiver;
 * otherwise this call builds it once and the following depth composite reuses
 * it. The old separate 33x33 sampler repeated terrain lookup and matrix work
 * on every camera move, then the main pass projected the same surface again. */
void SimBackgroundVoxelTerrainDepth_GroundDepthRange(
    const SimBackgroundVoxelRenderParams *params,
    float *minimum, float *maximum) {
  *minimum = FLT_MAX;
  *maximum = -FLT_MAX;
  if (!TerrainProjectionKeyMatches(params))
    BuildTerrainProjectionCache(params);
  if (!TerrainProjectionKeyMatches(params)) return;
  *minimum = s_terrain_projection_cache.minimum_clip_depth;
  *maximum = s_terrain_projection_cache.maximum_clip_depth;
}

void SimBackgroundVoxelTerrainDepth_Reset(void) {
  s_terrain_projection_cache.valid = false;
  s_terrain_projection_cache.count = 0;
  s_terrain_depth_cache.valid = false;
}

#endif  /* AR_SIM3D_TERRAIN_ELEVATION */
