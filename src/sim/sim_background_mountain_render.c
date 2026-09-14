#include "sim_background_mountain_render.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "scene3d_math.h"
#include "sim3d_depth_pass.h"
#include "sim_background_mountain_objects.h"
#include "sim_background_mountain_relief.h"
#include "sim_background_mountain_mesh.h"
#include "sim_background_voxel_lod.h"
#include "sim_background_voxel_project.h"
#include "sim_background_voxels.h"

enum {
  /* Original cells plus the complete bounded set of reconstructed cap tiles. */
  kMaxMountainReliefCells = kSimBackgroundMountainCellCount +
      kSimBackgroundMountainMaxCapTiles,
  /* Every stack layer of every cell, plus the two silhouette skirt faces a
   * cell can contribute. The skirt is emitted once per cell rather than once
   * per layer, but the budget has to cover a mountain whose every cell is on
   * its own silhouette. */
  kMountainSkirtFacesPerCell = 2,
  kMaxMountainReliefFaces = kMaxMountainReliefCells *
      (kSimBackgroundMountainReliefMaxStackLayers +
       kMountainSkirtFacesPerCell),
};

static const float kMountainLodReferenceHeightPixels = 24.0f;
/* The volcano stands taller than the ordinary peaks it shares a stamp with. */
static const float kVolcanoHeightScale = 1.12f;

typedef struct ProjectedMountainReliefFace {
  Scene3DPoint points[4];
  float gpu_depth[4];
  ArRenderPointF uv[4];
  uint8_t brightness[4];
  uint8_t alpha[4];
} ProjectedMountainReliefFace;

/* Projection-only cache key. Pixel generations are deliberately absent:
 * atlas colour changes do not move geometry, while the scene generation,
 * terrain magnitude, camera, source/viewport and prepared projection do.
 * Animated volcano effects are emitted separately and therefore do not put
 * game_frame into this retained static-geometry key. */
typedef struct MountainProjectionCacheKey {
  uint32_t scene_serial;
  uint8_t detail;
  uint8_t lod;
  uint8_t facing;
  uint8_t render_scale;
  uint8_t town;
  uint16_t landscape_height_pct;
  uint16_t camera_x, camera_y;
  uint16_t town_screen_x0;
  ArRenderRectI source;
  ArRenderRectI viewport;
  float matrix[16];
  float texture_to_clip[16];
  bool texture_to_clip_valid;
} MountainProjectionCacheKey;

static struct {
  ProjectedMountainReliefFace projected[kMaxMountainReliefFaces];
  MountainProjectionCacheKey projection_key;
  SimBackgroundMountainObjectList mountain_objects;
  int projected_count;
  bool projection_valid;
  bool mountain_objects_valid;
  /* Exact per-column silhouette tops keep every repeated mountain copy
   * converged at its own local peak, even inside one connected range. */
  int16_t peak_y
      [kSimBackgroundMountainCellCount + 1]
      [kSimBackgroundMountainTownCells];
  /* A connected range may contain several overlapping peaks whose feet land
   * on different map rows. Per-column bases keep those local contacts on the
   * ground instead of lifting every peak to the component's lowest row. */
  int16_t base_y
      [kSimBackgroundMountainCellCount + 1]
      [kSimBackgroundMountainTownCells];
} g_mountain_state;

static bool SameRenderRect(ArRenderRectI a, ArRenderRectI b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static bool MountainProjectionCacheMatches(
    const SimBackgroundVoxelRenderParams *params, uint32_t scene_serial) {
  const MountainProjectionCacheKey *key = &g_mountain_state.projection_key;
  return g_mountain_state.projection_valid &&
      key->scene_serial == scene_serial &&
      key->detail == params->detail && key->lod == params->lod &&
      key->facing == params->facing &&
      key->render_scale == params->render_scale &&
      key->town == params->town &&
      key->landscape_height_pct == params->landscape_height_pct &&
      key->camera_x == params->camera_x && key->camera_y == params->camera_y &&
      key->town_screen_x0 == params->town_screen_x0 &&
      SameRenderRect(key->source, params->source) &&
      SameRenderRect(key->viewport, params->viewport) &&
      key->texture_to_clip_valid == params->texture_to_clip_valid &&
      memcmp(key->matrix, params->matrix, sizeof(key->matrix)) == 0 &&
      memcmp(key->texture_to_clip, params->texture_to_clip,
             sizeof(key->texture_to_clip)) == 0;
}

static void SaveMountainProjectionCacheKey(
    const SimBackgroundVoxelRenderParams *params, uint32_t scene_serial) {
  MountainProjectionCacheKey *key = &g_mountain_state.projection_key;
  *key = (MountainProjectionCacheKey){
    .scene_serial = scene_serial,
    .detail = params->detail,
    .lod = params->lod,
    .facing = params->facing,
    .render_scale = params->render_scale,
    .town = params->town,
    .landscape_height_pct = params->landscape_height_pct,
    .camera_x = params->camera_x,
    .camera_y = params->camera_y,
    .town_screen_x0 = params->town_screen_x0,
    .source = params->source,
    .viewport = params->viewport,
    .texture_to_clip_valid = params->texture_to_clip_valid,
  };
  memcpy(key->matrix, params->matrix, sizeof(key->matrix));
  memcpy(key->texture_to_clip, params->texture_to_clip,
         sizeof(key->texture_to_clip));
}

static SimBackgroundVoxelDetail EffectiveMountainDetail(
    const SimBackgroundVoxelRenderParams *params) {
  SimBackgroundVoxelDetail requested =
      (SimBackgroundVoxelDetail)params->detail;
  if (params->lod != kSimBackgroundVoxelLod_Adaptive) return requested;
  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  float center = kSimTownCanvasPixels * 0.5f;
  Scene3DPoint bottom, top;
  if (!SimBackgroundVoxelProject_Point(
          params, &kSimBackgroundUprightProjectionAxis,
          origin_x + center, origin_y + center,
          0.0f, &bottom, NULL) ||
      !SimBackgroundVoxelProject_Point(
          params, &kSimBackgroundUprightProjectionAxis,
          origin_x + center, origin_y + center,
          kMountainLodReferenceHeightPixels, &top, NULL))
    return requested;
  float dx = top.x - bottom.x, dy = top.y - bottom.y;
  float projected_height = sqrtf(dx * dx + dy * dy);
  if (params->render_scale == kSimBackgroundVoxelRenderScale_2x)
    projected_height *= 0.5f;
  return SimBackgroundVoxelLod_Resolve(
      requested, kSimBackgroundVoxelLod_Adaptive, projected_height);
}

static void AddProjectedMountainReliefFace(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float local_z[4], const ArRenderPointF uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4],
    int *count) {
  if (*count >= kMaxMountainReliefFaces) return;
  ProjectedMountainReliefFace face;
  for (int point = 0; point < 4; point++) {
    const float terrain_lift = SimBackgroundVoxelProject_TerrainLiftPixels(
        params, local_x[point], local_y[point]);
    if (!SimBackgroundVoxelProject_GroundedVertex(
            params, axis,
            origin_x + local_x[point], origin_y + local_y[point],
            local_z[point], terrain_lift,
            &face.points[point], &face.gpu_depth[point]))
      return;
    face.uv[point] = uv[point];
    face.brightness[point] = brightness[point];
    face.alpha[point] = alpha[point];
  }
  if (SimBackgroundVoxelProject_IsDegenerate(face.points)) return;
  g_mountain_state.projected[(*count)++] = face;
}

static bool AppendProjectedSolidEffectFace(void *user,
    const float local_x[4], const float local_y[4],
    const float local_z[4], const SimBackgroundProjectionAxis *axis,
    ArRenderColorF color) {
  const SimBackgroundVoxelRenderParams *params = user;
  const float origin_x = (float)params->town_screen_x0-params->camera_x;
  const float origin_y = -(float)params->camera_y;
  if (!Sim3DDepthPass_IsCollecting()) return true;
  Scene3DPoint points[4];
  Sim3DDepthVertex vertices[4];
  static const ArRenderPointF uv[4] = {
    {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
  };
  for (int point = 0; point < 4; point++) {
    float depth;
    float terrain_lift = SimBackgroundVoxelProject_TerrainLiftPixels(
        params, local_x[point], local_y[point]);
    if (!SimBackgroundVoxelProject_GroundedVertex(
            params, axis,
            origin_x + local_x[point], origin_y + local_y[point],
            local_z[point], terrain_lift, &points[point], &depth))
      return true;
    vertices[point] = (Sim3DDepthVertex){
      .x = points[point].x,
      .y = points[point].y,
      .depth = depth,
      .color = color,
      .uv = uv[point],
    };
  }
  return SimBackgroundVoxelProject_IsDegenerate(points) ||
      Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Effect, vertices);
}

static bool EmitSolidEffectBox(SimBackgroundMountainEffectEmit emit, void *user,
    const SimBackgroundProjectionAxis *axis,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    ArRenderColorF color) {
  static const uint8_t face[5][4] = {
    {4, 5, 6, 7},  /* top */
    {1, 0, 4, 5},  /* north */
    {2, 1, 5, 6},  /* east */
    {3, 2, 6, 7},  /* south */
    {0, 3, 7, 4},  /* west */
  };
  const float x[8] = {x0, x1, x1, x0, x0, x1, x1, x0};
  const float y[8] = {y0, y0, y1, y1, y0, y0, y1, y1};
  const float z[8] = {z0, z0, z0, z0, z1, z1, z1, z1};
  for (int side = 0; side < 5; side++) {
    float face_x[4], face_y[4], face_z[4];
    ArRenderColorF shaded = color;
    if (side) {
      float shade = side == 3 ? 0.92f : side == 2 ? 0.84f : 0.76f;
      shaded.r *= shade;
      shaded.g *= shade;
      shaded.b *= shade;
    }
    for (int point = 0; point < 4; point++) {
      int corner = face[side][point];
      face_x[point] = x[corner];
      face_y[point] = y[corner];
      face_z[point] = z[corner];
    }
    if (!emit(user,face_x,face_y,face_z,axis,shaded)) return false;
  }
  return true;
}

/* Unit direction, in town-texture pixels, that leads away from the camera
 * across the ground. */
typedef SimBackgroundMountainMeshDirection SimBackgroundStackDirection;

/* The relief stack fakes a mountain's thickness with parallel copies of its
 * art displaced behind the front one. "Behind" has to mean behind the CAMERA,
 * not map north: the sim camera has a yaw axis, a reactive lean and a manual
 * orbit, and a fixed northward displacement fans the copies out sideways as
 * soon as any of them is non-zero, which reads as the rear copies sliding off
 * the ground while the front one stays put. At yaw zero this resolves to
 * (0,-1) and reproduces the original northward offset exactly. */
static SimBackgroundStackDirection MountainStackDirection(
    const SimBackgroundVoxelRenderParams *params) {
  float aspect = (float)params->viewport.w / params->viewport.h;
  SimBackgroundStackDirection direction;
  if (!Scene3D_GroundDepthDirection(
          params->matrix, aspect, params->source.w, params->source.h,
          &direction.x, &direction.y, NULL))
    return (SimBackgroundStackDirection){0.0f, -1.0f};
  return direction;
}

/* What every tile of one mountain shares. Threading these seven values through
 * each emitter individually pushed both of them past fifteen parameters, which
 * is where the cell/source/baseline arguments that actually differ per tile
 * stopped being visible at the call site. */
typedef struct MountainTileContext {
  const SimBackgroundVoxelRenderParams *params;
  const SimBackgroundProjectionAxis *axis;
  const SimBackgroundMountainRelief *relief;
  SimBackgroundStackDirection stack_direction;
  float height_scale;
  float origin_x, origin_y;
  SimBackgroundMountainSourceEmit source_emit;
  void *source_user;
} MountainTileContext;

static bool MountainCapSource(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCapTile *tile,
    int *source_cell_x, int *source_cell_y) {
  *source_cell_x = tile->source_cell_x;
  *source_cell_y = tile->source_cell_y;
  return (*source_cell_x >= 0 && *source_cell_y >= 0) ||
      SimBackgroundMountains_TileSource(
          field, tile->source_tile, source_cell_x, source_cell_y);
}

enum {
  /* The lava blob authored into the $70/$71 crown occupies source pixels
   * x 138-149, y 136-141. The stamp's own origin is cell (6,8), so the blob's
   * centre is exactly the crown pair's shared edge, eleven pixels down. */
  kCraterCentreOffsetY = 11,
  kCraterSourceRadiusX = 6,
  kCraterSourceRadiusY = 3,
};

/* One flat elliptical fan on the crater plane. Each quad spans two octagon
 * segments so no submitted face collapses to a triangle, and the octagon
 * matches the stepped-but-round language the voxel models already use. */
static bool EmitCraterGlowRing(SimBackgroundMountainEffectEmit emit, void *user,
    const SimBackgroundProjectionAxis *axis,
    float centre_x, float centre_y, float z,
    float radius_x, float radius_y, ArRenderColorF colour) {
  static const float unit[8][2] = {
    {0.0f, -1.0f}, {0.7071f, -0.7071f}, {1.0f, 0.0f},
    {0.7071f, 0.7071f}, {0.0f, 1.0f}, {-0.7071f, 0.7071f},
    {-1.0f, 0.0f}, {-0.7071f, -0.7071f},
  };
  for (int segment = 0; segment < 8; segment += 2) {
    float local_x[4] = {centre_x};
    float local_y[4] = {centre_y};
    float local_z[4] = {z, z, z, z};
    for (int step = 0; step < 3; step++) {
      const float *point = unit[(segment + step) & 7];
      local_x[step + 1] = centre_x + point[0] * radius_x;
      local_y[step + 1] = centre_y + point[1] * radius_y;
    }
    if (!emit(user,local_x,local_y,local_z,axis,colour)) return false;
  }
  return true;
}

/* The crater mouth of the volcano drawn this frame. See the header: this is
 * published so the eruption's fireball arcs can launch from the point the
 * player sees smoking rather than from a constant kept beside the model. */
static SimBackgroundCraterAnchor g_crater_anchor;

bool SimBackgroundVoxelRenderer_CraterAnchor(SimBackgroundCraterAnchor *out) {
  if (!out) return false;
  *out = g_crater_anchor;
  return g_crater_anchor.valid;
}

static SimBackgroundCraterSource ResolveCraterSource(
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainObject *object,
    float baseline, float height_scale) {

  /* The glow belongs on the authored blob's centre, not on the crown row's
   * top edge: the old placement pushed it a full three pixels past the peak
   * and left it hanging over the grass behind the mountain. */
  float crater_x =
      (object->cell_x + object->width_cells * 0.5f) *
      kSimBackgroundCellPixels;
  float crater_source_y =
      object->cell_y * kSimBackgroundCellPixels + kCraterCentreOffsetY;
  float crater_y, crater_z, ignored_x, rim_y, rim_z;
  SimBackgroundMountainMesh_PlanePoint(
      crater_x, crater_source_y, baseline, relief, 0.0f, 0.0f,
      &ignored_x, &crater_y, &crater_z);
  /* Take the depth radius from the same plane mapping as the face it sits on,
   * so the ellipse keeps hugging the blob whenever relief is retuned. */
  SimBackgroundMountainMesh_PlanePoint(
      crater_x, crater_source_y - kCraterSourceRadiusY, baseline, relief,
      0.0f, 0.0f, &ignored_x, &rim_y, &rim_z);
  float radius_y = crater_y - rim_y;
  if (radius_y < 0.5f) radius_y = 0.5f;
  crater_z *= height_scale;
  return (SimBackgroundCraterSource){crater_x,crater_y,crater_z,radius_y,*axis};
}

static bool EmitCraterEffects(const SimBackgroundCraterSource *crater,
    uint16_t frame, uint8_t detail, uint8_t style,
    SimBackgroundMountainEffectEmit emit, void *user) {
  const float crater_x=crater->x, crater_y=crater->y, crater_z=crater->z;
  const float radius_y=crater->radius_y;
  const SimBackgroundProjectionAxis *axis=&crater->axis;
  if (detail < kSimBackgroundVoxelDetail_Balanced ||
      style < kSimBackgroundVoxelStyle_Trim) return true;

  /* The source crater flashes on an eight-frame cadence. Match that cadence
   * with two shallow depth-tested glow rings rather than a screen-space
   * bloom, so nearby peaks still occlude the light correctly. Rings, not
   * rectangles: a squared-off slab of lava is the one shape the 12x6 pixel
   * blob never has. */
  bool flash_on = ((frame >> 3) & 1u) == 0;
  if (flash_on) {
    if (!EmitCraterGlowRing(
        emit,user,axis,crater_x,crater_y,
        crater_z + 0.35f, (float)kCraterSourceRadiusX, radius_y,
        (ArRenderColorF){1.0f, 0.20f, 0.02f, 0.36f})) return false;
    if (!EmitCraterGlowRing(
        emit,user,axis,crater_x,crater_y,
        crater_z + 0.55f, kCraterSourceRadiusX * 0.55f, radius_y * 0.55f,
        (ArRenderColorF){1.0f, 0.56f, 0.08f, 0.84f})) return false;
  }

  if (detail < kSimBackgroundVoxelDetail_High ||
      style < kSimBackgroundVoxelStyle_Architectural) return true;
  int puff_count = detail == kSimBackgroundVoxelDetail_Ultra ? 4 : 2;
  for (int puff = 0; puff < puff_count; puff++) {
    unsigned age = (frame + (unsigned)puff * 17u) % 48u;
    if (age > 34u) continue;
    float age_fraction = age / 34.0f;
    float size = 2.4f + age_fraction * 2.8f;
    float drift_x = ((puff & 1) ? 1.0f : -1.0f) * age_fraction * 5.0f;
    float drift_y = -age_fraction * 4.0f;
    float z0 = crater_z + 2.0f + age_fraction * 17.0f;
    float opacity = 0.72f - age_fraction * 0.34f;
    ArRenderColorF smoke = {
      0.66f + age_fraction * 0.10f,
      0.63f + age_fraction * 0.10f,
      0.58f + age_fraction * 0.10f,
      opacity,
    };
    if (!EmitSolidEffectBox(
        emit,user,axis,
        crater_x + drift_x - size * 0.5f,
        crater_y + drift_y - size * 0.5f, z0,
        crater_x + drift_x + size * 0.5f,
        crater_y + drift_y + size * 0.5f, z0 + size,
        smoke)) return false;
  }
  return true;
}

static void RecordMountainPeakColumn(
    uint16_t component, int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y) {
  if (!component || component > kSimBackgroundMountainCellCount ||
      destination_cell_x < 0 ||
      destination_cell_x >= kSimBackgroundMountainTownCells ||
      source_cell_x < 0 ||
      source_cell_x >= kSimBackgroundMountainTownCells ||
      source_cell_y < 0 ||
      source_cell_y >= kSimBackgroundMountainTownCells)
    return;
  const uint32_t *atlas = SimBackgroundVoxels_AtlasPixels();
  int peak_y = INT_MAX;
  int source_x0 = source_cell_x * kSimBackgroundCellPixels;
  int source_y0 = source_cell_y * kSimBackgroundCellPixels;
  for (int y = 0; y < kSimBackgroundCellPixels; y++)
    for (int x = 0; x < kSimBackgroundCellPixels; x++) {
      size_t at = (size_t)(source_y0 + y) * kSimTownCanvasPixels +
          (size_t)(source_x0 + x);
      if ((atlas[at] >> 24) == 0) continue;
      int destination_y =
          destination_cell_y * kSimBackgroundCellPixels + y;
      if (destination_y < peak_y) peak_y = destination_y;
    }
  if (peak_y <
      g_mountain_state.peak_y[component][destination_cell_x])
    g_mountain_state.peak_y[component][destination_cell_x] =
        (int16_t)peak_y;
}

static void BuildMountainPeakColumns(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps) {
  for (int component = 0;
       component <= kSimBackgroundMountainCellCount; component++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++)
      g_mountain_state.peak_y[component][x] = INT16_MAX;
  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      if (!SimBackgroundMountains_CellOccupied(field, x, y)) continue;
      int cell = y * kSimBackgroundMountainTownCells + x;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      RecordMountainPeakColumn(component, x, y, x, y);
    }
  for (uint8_t at = 0; at < caps->tile_count; at++) {
    const SimBackgroundMountainCapTile *tile = &caps->tiles[at];
    int source_cell_x, source_cell_y;
    if (!MountainCapSource(
            field, tile, &source_cell_x, &source_cell_y))
      continue;
    uint16_t component = tile->component ? tile->component : 1;
    RecordMountainPeakColumn(
        component, tile->cell_x, tile->cell_y,
        source_cell_x, source_cell_y);
  }
}

static void BuildMountainBaseColumns(
    const SimBackgroundMountainField *field) {
  for (int component = 0;
       component <= kSimBackgroundMountainCellCount; component++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++)
      g_mountain_state.base_y[component][x] = INT16_MIN;
  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      if (!SimBackgroundMountains_CellOccupied(field, x, y)) continue;
      int cell = y * kSimBackgroundMountainTownCells + x;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      int bottom = (y + 1) * kSimBackgroundCellPixels;
      if (bottom > g_mountain_state.base_y[component][x])
        g_mountain_state.base_y[component][x] = (int16_t)bottom;
    }
}

static float MountainEdgeBaseline(
    uint16_t component, int edge_x, float fallback) {
  int baseline = INT_MIN;
  if (component <= kSimBackgroundMountainCellCount)
    for (int side = -1; side <= 0; side++) {
      int cell_x = edge_x + side;
      if (cell_x < 0 || cell_x >= kSimBackgroundMountainTownCells)
        continue;
      int candidate =
          g_mountain_state.base_y[component][cell_x];
      if (candidate > baseline) baseline = candidate;
    }
  return baseline == INT_MIN ? fallback : (float)baseline;
}

static float MountainEdgeMaximumRise(
    uint16_t component, int edge_x, float baseline, float fallback_top) {
  int peak_y = INT16_MAX;
  if (component <= kSimBackgroundMountainCellCount)
    for (int side = -1; side <= 0; side++) {
      int cell_x = edge_x + side;
      if (cell_x < 0 || cell_x >= kSimBackgroundMountainTownCells)
        continue;
      int candidate =
          g_mountain_state.peak_y[component][cell_x];
      if (candidate < peak_y) peak_y = candidate;
    }
  float top = peak_y == INT16_MAX ? fallback_top : (float)peak_y;
  float maximum_rise = baseline - top;
  return maximum_rise > 0.0f ? maximum_rise : 1.0f;
}

typedef struct MountainMeshProjection {
  const MountainTileContext *context;
  int *count;
} MountainMeshProjection;

static void ProjectMountainMesh(
    void *user, const float x[4], const float y[4], const float z[4],
    const SimBackgroundMountainMeshUV source_uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4]) {
  const MountainMeshProjection *projection = user;
  const MountainTileContext *context = projection->context;
  if (context->source_emit) {
    context->source_emit(context->source_user,x,y,z,context->axis,
        source_uv,brightness,alpha);
    ++*projection->count;
    return;
  }
  ArRenderPointF uv[4];
  for (int i = 0; i < 4; i++)
    uv[i] = (ArRenderPointF){source_uv[i].x, source_uv[i].y};
  AddProjectedMountainReliefFace(
      context->params, context->axis, context->origin_x, context->origin_y,
      x, y, z, uv, brightness, alpha, projection->count);
}

static SimBackgroundMountainMeshContext MountainMeshContext(
    const MountainTileContext *context, MountainMeshProjection *projection) {
  return (SimBackgroundMountainMeshContext){
    .relief = context->relief,
    .stack_direction = context->stack_direction,
    .height_scale = context->height_scale,
    .atlas_pixels = kSimTownCanvasPixels,
    .emit = ProjectMountainMesh,
    .user = projection,
  };
}

static void AddMountainStackTile(
    const MountainTileContext *context,
    float baseline_left, float baseline_right,
    float maximum_rise_left, float maximum_rise_right,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t flags, int *count) {
  MountainMeshProjection projection = {context, count};
  SimBackgroundMountainMeshContext mesh = MountainMeshContext(context, &projection);
  SimBackgroundMountainMesh_StackTile(
      &mesh, baseline_left, baseline_right, maximum_rise_left, maximum_rise_right,
      destination_cell_x, destination_cell_y, source_cell_x, source_cell_y, flags);
}

static void AddMountainSkirtTile(
    const MountainTileContext *context, float baseline, float maximum_rise,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t source_tile,
    bool right_edge, int *count) {
  MountainMeshProjection projection = {context, count};
  SimBackgroundMountainMeshContext mesh = MountainMeshContext(context, &projection);
  SimBackgroundMountainMesh_SkirtTile(
      &mesh, baseline, maximum_rise, destination_cell_x, destination_cell_y,
      source_cell_x, source_cell_y, source_tile, right_edge);
}

static bool MountainCellOccupied(const SimBackgroundMountainObject *object,
                                 int row, int column) {
  return column >= 0 && column < object->width_cells &&
      (object->row_occupied_mask[row] & (1u << column)) != 0;
}

static int BuildProjectedMountainObjectFaces(
    const MountainTileContext *shared,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainObjectList *objects) {
  int count = 0;
  for (uint8_t at = 0; at < objects->count; at++) {
    const SimBackgroundMountainObject *object = &objects->objects[at];
    /* Only the volcano's extra height varies between objects. */
    MountainTileContext context = *shared;
    context.height_scale *=
        object->flags & kSimBackgroundMountainObject_Volcano
            ? kVolcanoHeightScale : 1.0f;
    float baseline =
        (object->cell_y + object->height_cells) *
        (float)kSimBackgroundCellPixels;
    float maximum_rise =
        object->height_cells * (float)kSimBackgroundCellPixels;
    for (int row = 0; row < object->height_cells; row++)
      for (int column = 0; column < object->width_cells; column++) {
        if (!(object->row_occupied_mask[row] & (1u << column))) continue;
        int destination_x = object->cell_x + column;
        int destination_y = object->cell_y + row;
        /* Horizontal level edges retain their authentic half-peaks. At the
         * south edge, however, keep the rest of the independently reconstructed
         * object: clipping every row at y=32 reduced full mountains to shallow
         * disconnected caps whenever their bases extended beyond the map. */
        if (destination_x < 0 ||
            destination_x >= kSimBackgroundMountainTownCells ||
            destination_y < -kSimBackgroundMountainObjectMaxRows ||
            destination_y >= kSimBackgroundMountainTownCells +
                kSimBackgroundMountainObjectMaxRows)
          continue;
        int source_x, source_y;
        uint8_t source_tile = object->source_tile[row][column];
        if (!source_tile ||
            (!SimBackgroundVoxels_MountainTileSource(
                 source_tile, &source_x, &source_y) &&
             !SimBackgroundMountains_TileSource(
                 field, source_tile, &source_x, &source_y)))
          continue;
        AddMountainStackTile(
            &context, baseline, baseline, maximum_rise, maximum_rise,
            destination_x, destination_y, source_x, source_y, 0, &count);
        /* Close the silhouette wherever the mass ends, so the raised part of
         * the plane has a visible side reaching the ground instead of an open
         * edge with the ground showing under it. */
        for (int side = 0; side < 2; side++) {
          bool right_edge = side != 0;
          if (MountainCellOccupied(object, row,
                                   column + (right_edge ? 1 : -1)))
            continue;
          AddMountainSkirtTile(
              &context, baseline, maximum_rise,
              destination_x, destination_y,
              source_x, source_y, source_tile, right_edge, &count);
        }
      }
  }
  return count;
}

/* Volcano glow/smoke is intentionally outside the retained relief cache: its
 * geometry follows game_frame, while the mountain mass does not. The crater
 * anchor is also republished every presentation so leaving a volcano scene
 * cannot retain the prior town's emitter. */
static void AppendMountainObjectEffects(
    const MountainTileContext *shared,
    const SimBackgroundMountainObjectList *objects) {
  g_crater_anchor.valid = false;
  for (uint8_t at = 0; at < objects->count; at++) {
    const SimBackgroundMountainObject *object = &objects->objects[at];
    MountainTileContext context = *shared;
    context.height_scale *=
        object->flags & kSimBackgroundMountainObject_Volcano
            ? kVolcanoHeightScale : 1.0f;
    const float baseline =
        (object->cell_y + object->height_cells) *
        (float)kSimBackgroundCellPixels;
    if (!(object->flags & kSimBackgroundMountainObject_Volcano)) continue;
    const SimBackgroundCraterSource crater = ResolveCraterSource(
        context.axis,context.relief,object,baseline,context.height_scale);
    g_crater_anchor = (SimBackgroundCraterAnchor){true,
      crater.x+crater.z*crater.axis.x_per_height,
      crater.y+crater.z*crater.axis.y_per_height,crater.z*crater.axis.height_scale};
    (void)EmitCraterEffects(&crater,context.params->game_frame,
        context.params->detail,context.params->style,
        AppendProjectedSolidEffectFace,(void *)context.params);
  }
}

static void AddNorthMountainCaps(
    const MountainTileContext *context,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps,
    const float component_bottom[kSimBackgroundMountainCellCount + 1],
    const float component_top[kSimBackgroundMountainCellCount + 1],
    int *count) {
  for (uint8_t at = 0; at < caps->tile_count; at++) {
    const SimBackgroundMountainCapTile *tile = &caps->tiles[at];
    int source_cell_x, source_cell_y;
    if (!MountainCapSource(
            field, tile, &source_cell_x, &source_cell_y))
      continue;
    uint16_t component = tile->component ? tile->component : 1;
    float maximum_rise_left = MountainEdgeMaximumRise(
        component, tile->cell_x,
        MountainEdgeBaseline(
            component, tile->cell_x, component_bottom[component]),
        component_top[component]);
    float maximum_rise_right = MountainEdgeMaximumRise(
        component, tile->cell_x + 1,
        MountainEdgeBaseline(
            component, tile->cell_x + 1, component_bottom[component]),
        component_top[component]);
    float baseline_left = MountainEdgeBaseline(
        component, tile->cell_x, component_bottom[component]);
    float baseline_right = MountainEdgeBaseline(
        component, tile->cell_x + 1, component_bottom[component]);
    AddMountainStackTile(
        context, baseline_left, baseline_right,
        maximum_rise_left, maximum_rise_right,
        tile->cell_x, tile->cell_y,
        source_cell_x, source_cell_y, tile->flags,
        count);
  }
}

bool SimBackgroundMountainRender_SourceStyle(
    const SimBackgroundVoxelRenderParams *params, SimBackgroundMountainSourceStyle *out) {
  if (!out || !params || !params->matrix || params->source.w <= 0 ||
      params->source.h <= 0 || params->viewport.w <= 0 || params->viewport.h <= 0 ||
      params->town < 1 || params->town > kSimTownCount ||
      params->detail >= kSimBackgroundVoxelDetail_Count ||
      params->lod >= kSimBackgroundVoxelLod_Count ||
      params->facing >= kSimBackgroundVoxelFacing_Count ||
      params->render_scale >= kSimBackgroundVoxelRenderScale_Count) return false;
  for (int i = 0; i < 16; ++i) if (!isfinite(params->matrix[i])) return false;
  SimBackgroundMountainSourceStyle style;
  memset(&style,0,sizeof(style));
  style.detail = EffectiveMountainDetail(params);
  style.axis = SimBackgroundVoxelProject_Axis(params,
      SimBackgroundVoxelProject_CameraFacingLean(params,
          params->facing == kSimBackgroundVoxelFacing_PerModel ? .44f : .35f));
  style.stack_direction = MountainStackDirection(params);
  *out = style;
  return true;
}

bool SimBackgroundMountainRender_EffectSource(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundMountainEffectSource *out) {
  SimBackgroundMountainSourceStyle style;
  if (!out || !SimBackgroundMountainRender_SourceStyle(params,&style) ||
      !SimBackgroundVoxelRenderer_Ready(params->serial)) return false;
  const SimBackgroundVoxelScene *scene=SimBackgroundVoxels_Scene();
  if (scene->town != params->town) return false;
  SimBackgroundMountainObjectList objects;
  SimBackgroundMountainEffectSource source={0};
  _Static_assert(kSimBackgroundCraterSourceMaximum >= kSimBackgroundMountainMaxObjects,
      "Every authored volcano must fit the effect source");
  if (SimBackgroundMountainObjects_Build(&scene->mountains,&scene->mountain_caps,&objects)) {
    SimBackgroundMountainRelief relief;
    SimBackgroundMountainRelief_Resolve(style.detail,&relief);
    for (unsigned i=0; i<objects.count; ++i) {
      const SimBackgroundMountainObject *o=&objects.objects[i];
      if (!(o->flags & kSimBackgroundMountainObject_Volcano)) continue;
      source.craters[source.count++]=ResolveCraterSource(&style.axis,&relief,o,
          (o->cell_y+o->height_cells)*(float)kSimBackgroundCellPixels,kVolcanoHeightScale);
    }
  }
  *out=source;
  return true;
}

bool SimBackgroundMountainRender_EmitEffects(
    const SimBackgroundMountainEffectSource *source, uint16_t frame,
    uint8_t detail, uint8_t style, SimBackgroundMountainEffectEmit emit, void *user) {
  if (!source || !emit || source->count>kSimBackgroundCraterSourceMaximum ||
      detail>=kSimBackgroundVoxelDetail_Count || style>=kSimBackgroundVoxelStyle_Count) return false;
  /* Reject malformed recipes before invoking a caller's drawing callback. */
  for (unsigned i=0; i<source->count; ++i) {
    const SimBackgroundCraterSource *c=&source->craters[i];
    if (!isfinite(c->x) || !isfinite(c->y) || !isfinite(c->z) ||
        !isfinite(c->radius_y) || c->radius_y<=0 ||
        !isfinite(c->axis.x_per_height) || !isfinite(c->axis.y_per_height) ||
        !isfinite(c->axis.height_scale)) return false;
  }
  for (unsigned i=0; i<source->count; ++i)
    if (!EmitCraterEffects(&source->craters[i],frame,detail,style,emit,user)) return false;
  return true;
}

static int BuildMountainFaces(const SimBackgroundVoxelRenderParams *params,
    SimBackgroundMountainSourceEmit emit, void *user) {
  const bool source_only = emit != NULL;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  const SimBackgroundMountainField *field = &scene->mountains;
  if (!field->cell_count) return 0;
  SimBackgroundMountainSourceStyle style;
  if (!SimBackgroundMountainRender_SourceStyle(params,&style)) return -1;
  SimBackgroundMountainRelief relief;
  SimBackgroundMountainRelief_Resolve(
      style.detail, &relief);
  if (!relief.stack_layer_count) return 0;

  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  MountainTileContext context = {
    .params = params,
    .axis = &style.axis,
    .relief = &relief,
    .stack_direction = style.stack_direction,
    /* Landscape magnitude translates the mountain's BASE through
     * SimBackgroundVoxelProject_TerrainLiftPixels at every vertex. It must
     * not resize the mountain's separately authored relief; a 50% landscape
     * remains a full mountain on gentler ground rather than turning the
     * mountain itself into a hill. */
    .height_scale = 1.0f,
    .origin_x = origin_x,
    .origin_y = origin_y,
    .source_emit = emit,
    .source_user = user,
  };
  const uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  const bool cache_hit = !source_only && MountainProjectionCacheMatches(
      params, scene_serial);
  if (!cache_hit) {
    g_mountain_state.mountain_objects_valid =
        SimBackgroundMountainObjects_Build(
            field, &scene->mountain_caps,
            &g_mountain_state.mountain_objects);
  }
  if (g_mountain_state.mountain_objects_valid) {
    if (!source_only && !params->omit_mountain_effects) AppendMountainObjectEffects(
        &context, &g_mountain_state.mountain_objects);
    if (cache_hit) return g_mountain_state.projected_count;
    const int count = BuildProjectedMountainObjectFaces(
        &context, field, &g_mountain_state.mountain_objects);
    if (source_only) return count;
    g_mountain_state.projected_count = count;
    SaveMountainProjectionCacheKey(params, scene_serial);
    g_mountain_state.projection_valid = true;
    return g_mountain_state.projected_count;
  }
  if (!source_only) g_crater_anchor.valid = false;
  if (cache_hit) return g_mountain_state.projected_count;
  /* Each connected range shares one baseline. Mapping source Y partly into
   * height and partly into ground depth turns the original pseudo-perspective
   * art into one continuous shallow facade. */
  float component_bottom[kSimBackgroundMountainCellCount + 1] = {0};
  float component_top[kSimBackgroundMountainCellCount + 1];
  for (int component = 0;
       component <= kSimBackgroundMountainCellCount; component++) {
    component_top[component] = kSimTownCanvasPixels;
  }
  for (int cell_y = 0; cell_y < kSimBackgroundMountainTownCells;
       cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundMountainTownCells;
         cell_x++) {
      int cell = cell_y * kSimBackgroundMountainTownCells + cell_x;
      if (!(field->flags[cell] & kSimBackgroundMountainCell_Occupied))
        continue;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      float bottom = (cell_y + 1) * kSimBackgroundCellPixels;
      float top = cell_y * kSimBackgroundCellPixels;
      if (bottom > component_bottom[component])
        component_bottom[component] = bottom;
      if (top < component_top[component])
        component_top[component] = top;
    }
  for (uint8_t at = 0; at < scene->mountain_caps.tile_count; at++) {
    const SimBackgroundMountainCapTile *tile =
        &scene->mountain_caps.tiles[at];
    uint16_t component = tile->component ? tile->component : 1;
    float top = tile->cell_y * kSimBackgroundCellPixels;
    if (top < component_top[component])
      component_top[component] = top;
  }
  BuildMountainPeakColumns(field, &scene->mountain_caps);
  BuildMountainBaseColumns(field);
  int count = 0;
  for (int cell_y = 0; cell_y < kSimBackgroundMountainTownCells;
       cell_y++) {
    for (int cell_x = 0; cell_x < kSimBackgroundMountainTownCells;
         cell_x++) {
      if (!SimBackgroundMountains_CellOccupied(field, cell_x, cell_y))
        continue;
      int cell = cell_y * kSimBackgroundMountainTownCells + cell_x;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      float baseline_left = MountainEdgeBaseline(
          component, cell_x, component_bottom[component]);
      float baseline_right = MountainEdgeBaseline(
          component, cell_x + 1, component_bottom[component]);
      float maximum_rise_left = MountainEdgeMaximumRise(
          component, cell_x, baseline_left, component_top[component]);
      float maximum_rise_right = MountainEdgeMaximumRise(
          component, cell_x + 1, baseline_right, component_top[component]);
      AddMountainStackTile(
          &context, baseline_left, baseline_right,
          maximum_rise_left, maximum_rise_right,
          cell_x, cell_y, cell_x, cell_y, 0,
          &count);
    }
  }
  AddNorthMountainCaps(
      &context, field, &scene->mountain_caps,
      component_bottom, component_top, &count);
  if (source_only) return count;
  g_mountain_state.projected_count = count;
  SaveMountainProjectionCacheKey(params, scene_serial);
  g_mountain_state.projection_valid = true;
  return g_mountain_state.projected_count;
}

int SimBackgroundMountainRender_BuildFaces(
    const SimBackgroundVoxelRenderParams *params) {
  return BuildMountainFaces(params,NULL,NULL);
}

int SimBackgroundMountainRender_EmitSource(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundMountainSourceEmit emit, void *user) {
  if (!params || !emit || !params->matrix || params->source.w <= 0 ||
      params->source.h <= 0 || params->viewport.w <= 0 || params->viewport.h <= 0 ||
      params->town < 1 || params->town > kSimTownCount ||
      params->detail >= kSimBackgroundVoxelDetail_Count ||
      params->lod >= kSimBackgroundVoxelLod_Count ||
      params->facing >= kSimBackgroundVoxelFacing_Count ||
      params->render_scale >= kSimBackgroundVoxelRenderScale_Count)
    return -1;
  return BuildMountainFaces(params,emit,user);
}

static void AppendProjectedMountainReliefFace(
    const ProjectedMountainReliefFace *face) {
  if (!Sim3DDepthPass_IsCollecting()) return;
  Sim3DDepthVertex vertices[4];
  for (int point = 0; point < 4; point++) {
    float shade = face->brightness[point] / 255.0f;
    vertices[point] = (Sim3DDepthVertex){
      .x = face->points[point].x,
      .y = face->points[point].y,
      .depth = face->gpu_depth[point],
      .color = {shade, shade, shade, face->alpha[point] / 255.0f},
      .uv = face->uv[point],
    };
  }
  Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Mountain, vertices);
}

void SimBackgroundMountainRender_SubmitFaces(int count) {
  for (int at = 0; at < count; at++)
    AppendProjectedMountainReliefFace(&g_mountain_state.projected[at]);
}

void SimBackgroundMountainRender_Reset(void) {
  g_crater_anchor = (SimBackgroundCraterAnchor){0};
  g_mountain_state.projection_valid = false;
  g_mountain_state.mountain_objects_valid = false;
  g_mountain_state.projected_count = 0;
}
