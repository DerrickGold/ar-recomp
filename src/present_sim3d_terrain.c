/* The visible textured terrain mesh: per-cell corner lighting, the cached
 * painter order, and the top/skirt quads. Split out of present_sim3d.c; the
 * definitions are unchanged.
 *
 * Entirely behind AR_SIM3D_TERRAIN_ELEVATION. The D32 occluder for the same
 * surface lives in sim_background_voxel_terrain_depth; both consume the one
 * corner field and the one baked hard-edge mask, which is what stops a cliff
 * from being a wall in one pass and a slope in the other. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "present_sim3d_terrain.h"
#include "sim/sim_town_canvas.h"
#include "sim/sim_town_terrain.h"
#include "sim/sim3d.h"
#include "sim/sim3d_performance.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

#if AR_SIM3D_TERRAIN_ELEVATION

enum {
  kSimTerrainCellCount =
      kSimTownTerrainCells * kSimTownTerrainCells,
  /* One top plus, at worst, four exposed neighbour-facing skirts. */
  kSimTerrainMaxQuads = kSimTerrainCellCount * 5,
  kSimTerrainMaxVertices = kSimTerrainMaxQuads * 4,
  kSimTerrainMaxIndices = kSimTerrainMaxQuads * 6,
  kSimTerrainContactRadiusCells = 2,
  kSimTerrainCaveJambPixelX = 2,
  kSimTerrainCaveJambPixelY = 8,
};

/* Deliberate terrain-lighting tunings, kept separate from geometry tolerances
 * and percentage/unit conversions. */
static const float kSimTerrainAmbientLight = 0.76f;
static const float kSimTerrainContactMaximumHeightUnits = 1.0f;
static const float kSimTerrainContactStrength = 0.16f;
static const float kSimTerrainNorthSouthSkirtShade = 0.82f;
static const float kSimTerrainEastWestSkirtShade = 0.86f;

typedef struct SimTerrainCellOrder {
  uint8_t x, y;
  float depth;
} SimTerrainCellOrder;

typedef struct SimTerrainRenderCell {
  float height[4];
  float light[4];
  float centre_height;
  uint8_t hard_edges;
  SimTownTerrainFaceKind face_kind;
} SimTerrainRenderCell;

typedef struct SimTerrainRenderCache {
  bool valid;
  uint8_t town;
  uint16_t landscape_height_pct;
  SimTerrainRenderCell cell[kSimTerrainCellCount];
  bool order_valid;
  float order_depth_x;
  float order_depth_y;
  float order_depth_height;
  SimTerrainCellOrder order[kSimTerrainCellCount];
} SimTerrainRenderCache;

static SimTerrainRenderCache s_sim_terrain_render_cache;

static SimTerrainRenderCell *SimTerrainCachedCell(int x, int y) {
  return &s_sim_terrain_render_cache.cell[
      y * kSimTownTerrainCells + x];
}

static int CompareSimTerrainCells(const void *left, const void *right) {
  const SimTerrainCellOrder *a = (const SimTerrainCellOrder *)left;
  const SimTerrainCellOrder *b = (const SimTerrainCellOrder *)right;
  /* The portable 2D geometry path has no depth buffer. Submit far geometry
   * first so the later, near cell wins at every projected overlap. `depth` is
   * clip W, the camera-space distance used by the perspective divide. */
  if (a->depth < b->depth) return 1;
  if (a->depth > b->depth) return -1;
  return (int)a->y * kSimTownTerrainCells + a->x -
      ((int)b->y * kSimTownTerrainCells + b->x);
}

static float SimTerrainHeightWorld(const FrameSlot *slot,
                                   ArRenderRectI source,
                                   float height_units) {
  if (source.h <= 0) return 0.0f;
  return SimTownTerrain_ScaledHeightPixels(
      height_units, (float)slot->sim.landscape_height_pct) /
      (float)source.h;
}

static float SimTerrainCellLight(int x, int y, int corner,
                                 float landscape_scale) {
  const float *c = SimTerrainCachedCell(x, y)->height;
  float dzdx, dzdy;
  switch (corner) {
    case kSimTownTerrainCornerNW: dzdx = c[1] - c[0]; dzdy = c[3] - c[0]; break;
    case kSimTownTerrainCornerNE: dzdx = c[1] - c[0]; dzdy = c[2] - c[1]; break;
    case kSimTownTerrainCornerSE: dzdx = c[2] - c[3]; dzdy = c[2] - c[1]; break;
    default: dzdx = c[2] - c[3]; dzdy = c[3] - c[0]; break;
  }
  dzdx *= landscape_scale;
  dzdy *= landscape_scale;
  float normal_z = 1.0f / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.0f);
  float light = kSimTerrainAmbientLight +
      (1.0f - kSimTerrainAmbientLight) * normal_z;

  /* Short-range, corner-sampled contact shade seats cliff feet.  Its 16%
   * ceiling is intentionally much gentler than the old cell-wide shadow. */
  int vertex_x = x + (corner == kSimTownTerrainCornerNE ||
                      corner == kSimTownTerrainCornerSE);
  int vertex_y = y + (corner == kSimTownTerrainCornerSE ||
                      corner == kSimTownTerrainCornerSW);
  float h0 = c[corner], occlusion = 0.0f, weight = 0.0f;
  for (int cell_y = vertex_y - kSimTerrainContactRadiusCells;
       cell_y < vertex_y + kSimTerrainContactRadiusCells; cell_y++) {
    if (cell_y < 0 || cell_y >= kSimTownTerrainCells) continue;
    for (int cell_x = vertex_x - kSimTerrainContactRadiusCells;
         cell_x < vertex_x + kSimTerrainContactRadiusCells; cell_x++) {
      if (cell_x < 0 || cell_x >= kSimTownTerrainCells) continue;
      float dx = (cell_x + 0.5f) - vertex_x;
      float dy = (cell_y + 0.5f) - vertex_y;
      float sample_weight = 1.0f / (1.0f + dx * dx + dy * dy);
      float above = (SimTerrainCachedCell(
          cell_x, cell_y)->centre_height - h0) *
          landscape_scale;
      if (above < 0.0f) above = 0.0f;
      if (above > kSimTerrainContactMaximumHeightUnits)
        above = kSimTerrainContactMaximumHeightUnits;
      occlusion += above * sample_weight;
      weight += sample_weight;
    }
  }
  float contact = weight > 0.0f
      ? occlusion / weight * kSimTerrainContactStrength : 0.0f;
  if (contact > kSimTerrainContactStrength)
    contact = kSimTerrainContactStrength;
  return light * (1.0f - contact);
}

static void PrepareSimTerrainRenderCache(
    uint8_t town, uint16_t landscape_height_pct) {
  if (s_sim_terrain_render_cache.valid &&
      s_sim_terrain_render_cache.town == town &&
      s_sim_terrain_render_cache.landscape_height_pct ==
          landscape_height_pct)
    return;

  /* Topology and heights are immutable. Lighting depends only on those
   * values and the user-controlled landscape magnitude, so recomputing it
   * every rendered frame spent 4,096 square roots and roughly 65k validated
   * terrain lookups on data that had not changed. Two passes keep all centre
   * heights ready before the short-range contact term examines neighbours. */
  for (int y = 0; y < kSimTownTerrainCells; y++)
    for (int x = 0; x < kSimTownTerrainCells; x++) {
      SimTerrainRenderCell *cell = SimTerrainCachedCell(x, y);
      cell->centre_height = SimTownTerrain_CellUnits(town, x, y);
      cell->hard_edges = SimTownTerrain_HardEdges(town, x, y);
      cell->face_kind = SimTownTerrain_FaceKind(town, x, y);
      for (int corner = 0; corner < kSimTownTerrainCornerCount; corner++)
        cell->height[corner] = SimTownTerrain_CornerUnits(
            town, x, y, corner);
    }
  const float landscape_scale =
      (float)landscape_height_pct / (float)kPercentScale;
  for (int y = 0; y < kSimTownTerrainCells; y++)
    for (int x = 0; x < kSimTownTerrainCells; x++) {
      SimTerrainRenderCell *cell = SimTerrainCachedCell(x, y);
      for (int corner = 0; corner < kSimTownTerrainCornerCount; corner++)
        cell->light[corner] = SimTerrainCellLight(
            x, y, corner, landscape_scale);
    }
  s_sim_terrain_render_cache.town = town;
  s_sim_terrain_render_cache.landscape_height_pct = landscape_height_pct;
  s_sim_terrain_render_cache.valid = true;
  s_sim_terrain_render_cache.order_valid = false;
}

static const SimTerrainCellOrder *PrepareSimTerrainCellOrder(
    const FrameSlot *slot, ArRenderRectI source, ArRenderRectI viewport,
    const float matrix[16]) {
  SimTerrainRenderCache *cache = &s_sim_terrain_render_cache;
  const float aspect = (float)viewport.w / (float)viewport.h;
  const float depth_x = matrix[3] * aspect *
      (float)kSimTownTerrainCellPixels / (float)source.w;
  const float depth_y = -matrix[7] *
      (float)kSimTownTerrainCellPixels / (float)source.h;
  const float depth_height = matrix[11] *
      SimTownTerrain_ScaledHeightPixels(
          1.0f, (float)slot->sim.landscape_height_pct) /
      (float)source.h;
  if (cache->order_valid && cache->order_depth_x == depth_x &&
      cache->order_depth_y == depth_y &&
      cache->order_depth_height == depth_height)
    return cache->order;

  /* Painter order depends on camera-space distance, not on the town's common
   * screen/camera translation or camera distance. Clip W is linear, so only
   * its per-cell X/Y/height coefficients can affect order; FOV, output scale,
   * matrix translation, and viewport placement cannot. This narrower key
   * preserves the order during zoom and resize-only camera work.
   *
   * Orbit motion normally changes the order only near a few crossing pairs.
   * Refresh depths in the already-sorted list and repair it with insertion
   * sort, whose cost is linear plus those crossings. The first town frame
   * still uses qsort so an arbitrary initial camera never hits its quadratic
   * worst case. */
  if (!cache->order_valid) {
    int count = 0;
    for (int y = 0; y < kSimTownTerrainCells; y++)
      for (int x = 0; x < kSimTownTerrainCells; x++)
        cache->order[count++] = (SimTerrainCellOrder){
          .x = (uint8_t)x,
          .y = (uint8_t)y,
        };
  }
  for (int at = 0; at < kSimTerrainCellCount; at++) {
    SimTerrainCellOrder *cell = &cache->order[at];
    cell->depth = depth_x * (cell->x + 0.5f) +
        depth_y * (cell->y + 0.5f) +
        depth_height * SimTerrainCachedCell(
            cell->x, cell->y)->centre_height;
  }
  if (!cache->order_valid) {
    qsort(cache->order, kSimTerrainCellCount, sizeof(cache->order[0]),
          CompareSimTerrainCells);
  } else {
    for (int at = 1; at < kSimTerrainCellCount; at++) {
      SimTerrainCellOrder cell = cache->order[at];
      int before = at - 1;
      while (before >= 0 &&
             CompareSimTerrainCells(&cache->order[before], &cell) > 0) {
        cache->order[before + 1] = cache->order[before];
        before--;
      }
      cache->order[before + 1] = cell;
    }
  }
  cache->order_depth_x = depth_x;
  cache->order_depth_y = depth_y;
  cache->order_depth_height = depth_height;
  cache->order_valid = true;
  return cache->order;
}

static bool AddSimTerrainQuad(
    ArRenderVertex2D *vertices, int *vertex_count,
    int32_t *indices, int *index_count,
    const float texture_xy[4][2], const float height_units[4],
    const float uv[4][2], const float shade[4], const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const float matrix[16],
    const SimCullFade *fade) {
  if (*vertex_count + 4 > kSimTerrainMaxVertices ||
      *index_count + 6 > kSimTerrainMaxIndices)
    return false;
  int first = *vertex_count;
  for (int i = 0; i < 4; i++) {
    Scene3DPoint point;
    if (!ProjectSimTexturePoint(
            matrix, source, viewport, texture_xy[i][0], texture_xy[i][1],
            SimTerrainHeightWorld(slot, source, height_units[i]), &point))
      return false;
    float away = SimCullProximityAt(
        fade, texture_xy[i][0], texture_xy[i][1], source);
    float brightness = (1.0f - away * fade->dim) * shade[i];
    float alpha = (1.0f - away * fade->fade) *
        SimGroundExtentAlphaAt(fade, texture_xy[i][0], texture_xy[i][1]);
    vertices[(*vertex_count)++] = (ArRenderVertex2D){
      {point.x, point.y},
      {brightness, brightness, brightness, alpha},
      {uv[i][0], uv[i][1]},
    };
  }
  indices[(*index_count)++] = first;
  indices[(*index_count)++] = first + 1;
  indices[(*index_count)++] = first + 2;
  indices[(*index_count)++] = first;
  indices[(*index_count)++] = first + 2;
  indices[(*index_count)++] = first + 3;
  return true;
}

static bool AddSimTerrainSkirt(
    ArRenderVertex2D *vertices, int *vertex_count,
    int32_t *indices, int *index_count,
    const float endpoint_xy[2][2], const float current_height[2],
    const float neighbour_height[2], const float endpoint_shade[2],
    const float side_uv[4][2], const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const float matrix[16],
    const SimCullFade *fade) {
  float t0, t1;
  if (!SimTownTerrain_ClipVisibleHigherEdge(
          current_height[0], current_height[1],
          neighbour_height[0], neighbour_height[1],
          &t0, &t1))
    return true;

  float xy[4][2], height[4], shade[4];
  const float t[2] = {t0, t1};
  for (int endpoint = 0; endpoint < 2; endpoint++) {
    const float at = t[endpoint];
    const float x = endpoint_xy[0][0] +
        (endpoint_xy[1][0] - endpoint_xy[0][0]) * at;
    const float y = endpoint_xy[0][1] +
        (endpoint_xy[1][1] - endpoint_xy[0][1]) * at;
    const float top = current_height[0] +
        (current_height[1] - current_height[0]) * at;
    const float bottom = neighbour_height[0] +
        (neighbour_height[1] - neighbour_height[0]) * at;
    const float light = endpoint_shade[0] +
        (endpoint_shade[1] - endpoint_shade[0]) * at;
    xy[endpoint][0] = xy[3 - endpoint][0] = x;
    xy[endpoint][1] = xy[3 - endpoint][1] = y;
    height[endpoint] = top;
    height[3 - endpoint] = bottom;
    shade[endpoint] = shade[3 - endpoint] = light;
  }
  return AddSimTerrainQuad(
      vertices, vertex_count, indices, index_count,
      xy, height, side_uv, shade, slot, source, viewport, matrix, fade);
}

static void SimTerrainCliffUv(int x, int y, int nx, int ny,
                              float uv_cell, float out[4][2]) {
  int source_x = x, source_y = y;
  if (SimTerrainCachedCell(x, y)->face_kind == kSimTownTerrainFace_None &&
      SimTerrainCachedCell(nx, ny)->face_kind != kSimTownTerrainFace_None) {
    source_x = nx;
    source_y = ny;
  }
  float source_u = 0.5f, source_v = 0.5f;
  if (SimTerrainCachedCell(source_x, source_y)->face_kind ==
      kSimTownTerrainFace_Cave) {
    /* $72's centre is the black opening. Its left jamb at native pixel
     * (2,8) is neutral cliff stone, so it can safely fill the tiny N/S skirt
     * without smearing the aperture into the horizontal seams seen around
     * Bloodpool's entrance. Half-pixel centres avoid atlas filtering bleed. */
    source_u = (kSimTerrainCaveJambPixelX + 0.5f) /
        (float)kSimTownTerrainCellPixels;
    source_v = (kSimTerrainCaveJambPixelY + 0.5f) /
        (float)kSimTownTerrainCellPixels;
  }
  const float u = (source_x + source_u) * uv_cell;
  const float v = (source_y + source_v) * uv_cell;
  for (int point = 0; point < 4; point++) {
    out[point][0] = u;
    out[point][1] = v;
  }
}

bool DrawSimTownTerrain(
    ArRenderDevice *device, ArRenderTexture texture, const FrameSlot *slot,
    float extent_x0, float extent_y0, ArRenderRectI source,
    ArRenderRectI viewport,
    const float matrix[16], const SimCullFade *fade) {
  if (!ArRenderDevice_IsReady(device) || !ArRenderTexture_IsValid(texture) ||
      !slot || slot->sim.town < 1 ||
      slot->sim.town > kSimTownTerrainTownCount || source.w <= 0 ||
      source.h <= 0 || viewport.w <= 0 || viewport.h <= 0)
    return false;

  PrepareSimTerrainRenderCache(
      slot->sim.town, slot->sim.landscape_height_pct);

  static ArRenderVertex2D vertices[kSimTerrainMaxVertices];
  static int32_t indices[kSimTerrainMaxIndices];
  const SimTerrainCellOrder *order = PrepareSimTerrainCellOrder(
      slot, source, viewport, matrix);

  int vertex_count = 0, index_count = 0;
  const float uv_inset = 0.5f / (float)kSimTownCanvasPixels;
  const float uv_cell =
      (float)kSimTownTerrainCellPixels / (float)kSimTownCanvasPixels;
  for (int at = 0; at < kSimTerrainCellCount; at++) {
    int x = order[at].x, y = order[at].y;
    float x0 = extent_x0 + x * kSimTownTerrainCellPixels;
    float y0 = extent_y0 + y * kSimTownTerrainCellPixels;
    float x1 = x0 + kSimTownTerrainCellPixels;
    float y1 = y0 + kSimTownTerrainCellPixels;
    const SimTerrainRenderCell *cell = SimTerrainCachedCell(x, y);
    const float *h = cell->height;
    const float *light = cell->light;
    const uint8_t hard_edges = cell->hard_edges;
    const float top_xy[4][2] = {{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    const float u0 = x * uv_cell + uv_inset;
    const float v0 = y * uv_cell + uv_inset;
    const float u1 = (x + 1) * uv_cell - uv_inset;
    const float v1 = (y + 1) * uv_cell - uv_inset;
    const float top_uv[4][2] = {{u0,v0},{u1,v0},{u1,v1},{u0,v1}};
    /* Skirts precede the top so the cell cap closes their shared edge. Every
     * map direction is emitted because the SIM camera may yaw/orbit. The
     * authored face side owns the material at a hard boundary; borrowing the
     * higher cell's grass is what produced Marahna's diagonal green wedge. */
    if (y > 0 && (hard_edges & kSimTownTerrainEdgeNorth)) {
      const float *n = SimTerrainCachedCell(x, y - 1)->height;
      float n0 = n[kSimTownTerrainCornerSW];
      float n1 = n[kSimTownTerrainCornerSE];
      const float xy[2][2] = {{x1,y0},{x0,y0}};
      const float current[2] = {h[1],h[0]};
      const float neighbour[2] = {n1,n0};
      const float shade[2] = {
        light[1] * kSimTerrainNorthSouthSkirtShade,
        light[0] * kSimTerrainNorthSouthSkirtShade,
      };
      float side_uv[4][2];
      SimTerrainCliffUv(x, y, x, y - 1, uv_cell, side_uv);
      if (!AddSimTerrainSkirt(
              vertices,&vertex_count,indices,&index_count,
              xy,current,neighbour,shade,side_uv,slot,source,viewport,
              matrix,fade))
        return false;
    }
    if (x > 0 && (hard_edges & kSimTownTerrainEdgeWest)) {
      const float *n = SimTerrainCachedCell(x - 1, y)->height;
      float n0 = n[kSimTownTerrainCornerNE];
      float n1 = n[kSimTownTerrainCornerSE];
      const float xy[2][2] = {{x0,y0},{x0,y1}};
      const float current[2] = {h[0],h[3]};
      const float neighbour[2] = {n0,n1};
      const float shade[2] = {
        light[0] * kSimTerrainEastWestSkirtShade,
        light[3] * kSimTerrainEastWestSkirtShade,
      };
      float side_uv[4][2];
      SimTerrainCliffUv(x, y, x - 1, y, uv_cell, side_uv);
      if (!AddSimTerrainSkirt(
              vertices,&vertex_count,indices,&index_count,
              xy,current,neighbour,shade,side_uv,slot,source,viewport,
              matrix,fade))
        return false;
    }
    if (y + 1 < kSimTownTerrainCells &&
        (hard_edges & kSimTownTerrainEdgeSouth)) {
      const float *n = SimTerrainCachedCell(x, y + 1)->height;
      float n0 = n[kSimTownTerrainCornerNW];
      float n1 = n[kSimTownTerrainCornerNE];
      const float xy[2][2] = {{x0,y1},{x1,y1}};
      const float current[2] = {h[3],h[2]};
      const float neighbour[2] = {n0,n1};
      const float shade[2] = {
        light[3] * kSimTerrainNorthSouthSkirtShade,
        light[2] * kSimTerrainNorthSouthSkirtShade,
      };
      float side_uv[4][2];
      SimTerrainCliffUv(x, y, x, y + 1, uv_cell, side_uv);
      if (!AddSimTerrainSkirt(
              vertices,&vertex_count,indices,&index_count,
              xy,current,neighbour,shade,side_uv,slot,source,viewport,
              matrix,fade))
        return false;
    }
    if (x + 1 < kSimTownTerrainCells &&
        (hard_edges & kSimTownTerrainEdgeEast)) {
      const float *n = SimTerrainCachedCell(x + 1, y)->height;
      float n0 = n[kSimTownTerrainCornerNW];
      float n1 = n[kSimTownTerrainCornerSW];
      const float xy[2][2] = {{x1,y0},{x1,y1}};
      const float current[2] = {h[1],h[2]};
      const float neighbour[2] = {n0,n1};
      const float shade[2] = {
        light[1] * kSimTerrainEastWestSkirtShade,
        light[2] * kSimTerrainEastWestSkirtShade,
      };
      float side_uv[4][2];
      SimTerrainCliffUv(x, y, x + 1, y, uv_cell, side_uv);
      if (!AddSimTerrainSkirt(
              vertices,&vertex_count,indices,&index_count,
              xy,current,neighbour,shade,side_uv,slot,source,viewport,
              matrix,fade))
        return false;
    }
    if (!AddSimTerrainQuad(vertices,&vertex_count,indices,&index_count,
                           top_xy,h,top_uv,light,slot,source,viewport,matrix,fade))
      return false;
  }
  if (!ArRenderDevice_DrawGeometry(
          device, texture, vertices, vertex_count, indices, index_count))
    return false;
  Sim3DPerformance_AddDraw((uint64_t)vertex_count, (uint64_t)index_count);
  return true;
}

#endif  /* AR_SIM3D_TERRAIN_ELEVATION */
