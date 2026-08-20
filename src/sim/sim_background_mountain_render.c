#include "sim_background_mountain_render.h"

#include <limits.h>
#include <math.h>

#include "constants.h"
#include "scene3d_math.h"
#include "sim3d_depth_pass.h"
#include "sim_background_mountain_objects.h"
#include "sim_background_mountain_relief.h"
#include "sim_background_mountain_silhouette.h"
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
/* Fully lit / fully opaque, for the relief faces that want neither shaped. */
static const uint8_t kMountainFullIntensity[4] = {255, 255, 255, 255};
/* The volcano stands taller than the ordinary peaks it shares a stamp with. */
static const float kVolcanoHeightScale = 1.12f;

typedef struct ProjectedMountainReliefFace {
  Scene3DPoint points[4];
  float gpu_depth[4];
  SDL_FPoint uv[4];
  uint8_t brightness[4];
  uint8_t alpha[4];
} ProjectedMountainReliefFace;

static struct {
  ProjectedMountainReliefFace projected[kMaxMountainReliefFaces];
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
    const float local_z[4], const SDL_FPoint uv[4],
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

static void AppendProjectedSolidEffectFace(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float local_z[4], SDL_FColor color) {
  if (!Sim3DDepthPass_IsCollecting()) return;
  Scene3DPoint points[4];
  Sim3DDepthVertex vertices[4];
  static const SDL_FPoint uv[4] = {
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
      return;
    vertices[point] = (Sim3DDepthVertex){
      .x = points[point].x,
      .y = points[point].y,
      .depth = depth,
      .color = color,
      .uv = uv[point],
    };
  }
  if (!SimBackgroundVoxelProject_IsDegenerate(points))
    Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Effect, vertices);
}

static void AppendProjectedSolidEffectBox(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    SDL_FColor color) {
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
    SDL_FColor shaded = color;
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
    AppendProjectedSolidEffectFace(
        params, axis, origin_x, origin_y,
        face_x, face_y, face_z, shaded);
  }
}

/* Unit direction, in town-texture pixels, that leads away from the camera
 * across the ground. */
typedef struct SimBackgroundStackDirection {
  float x, y;
} SimBackgroundStackDirection;

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
} MountainTileContext;

static void MountainPlanePoint(
    float source_x, float source_y, float baseline,
    const SimBackgroundMountainRelief *relief,
    float offset_x, float offset_y,
    float *local_x, float *local_y, float *local_z) {
  float rise = baseline - source_y;
  *local_x = source_x + offset_x;
  *local_y = baseline - rise * relief->face_depth_scale + offset_y;
  *local_z = rise * relief->face_height_scale;
}

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
static void AppendCraterGlowRing(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    float centre_x, float centre_y, float z,
    float radius_x, float radius_y, SDL_FColor colour) {
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
    AppendProjectedSolidEffectFace(
        params, axis, origin_x, origin_y,
        local_x, local_y, local_z, colour);
  }
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

static void AppendVolcanoEffects(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainObject *object,
    float origin_x, float origin_y, float baseline, float height_scale) {
  if (!(object->flags & kSimBackgroundMountainObject_Volcano)) return;

  /* The glow belongs on the authored blob's centre, not on the crown row's
   * top edge: the old placement pushed it a full three pixels past the peak
   * and left it hanging over the grass behind the mountain. */
  float crater_x =
      (object->cell_x + object->width_cells * 0.5f) *
      kSimBackgroundCellPixels;
  float crater_source_y =
      object->cell_y * kSimBackgroundCellPixels + kCraterCentreOffsetY;
  float crater_y, crater_z, ignored_x, rim_y, rim_z;
  MountainPlanePoint(
      crater_x, crater_source_y, baseline, relief, 0.0f, 0.0f,
      &ignored_x, &crater_y, &crater_z);
  /* Take the depth radius from the same plane mapping as the face it sits on,
   * so the ellipse keeps hugging the blob whenever relief is retuned. */
  MountainPlanePoint(
      crater_x, crater_source_y - kCraterSourceRadiusY, baseline, relief,
      0.0f, 0.0f, &ignored_x, &rim_y, &rim_z);
  float radius_y = crater_y - rim_y;
  if (radius_y < 0.5f) radius_y = 0.5f;
  crater_z *= height_scale;

  /* Publish the mouth before anything is drawn from it, and publish it LEANED
   * -- the same transform SimBackgroundVoxelProject_LeanedPointToWorld
   * applies to every model vertex, including the glow ring below. An
   * unleaned anchor sits a few pixels off
   * the glow at any pitch that leans the models at all, which is every pitch
   * the player uses. */
  g_crater_anchor = (SimBackgroundCraterAnchor){
    .valid = true,
    .local_x = crater_x + crater_z * axis->x_per_height,
    .local_y = crater_y + crater_z * axis->y_per_height,
    .height_pixels = crater_z * axis->height_scale,
  };

  if (params->detail < kSimBackgroundVoxelDetail_Balanced ||
      params->style < kSimBackgroundVoxelStyle_Trim)
    return;

  /* The source crater flashes on an eight-frame cadence. Match that cadence
   * with two shallow depth-tested glow rings rather than a screen-space
   * bloom, so nearby peaks still occlude the light correctly. Rings, not
   * rectangles: a squared-off slab of lava is the one shape the 12x6 pixel
   * blob never has. */
  bool flash_on = ((params->game_frame >> 3) & 1u) == 0;
  if (flash_on) {
    AppendCraterGlowRing(
        params, axis, origin_x, origin_y, crater_x, crater_y,
        crater_z + 0.35f, (float)kCraterSourceRadiusX, radius_y,
        (SDL_FColor){1.0f, 0.20f, 0.02f, 0.36f});
    AppendCraterGlowRing(
        params, axis, origin_x, origin_y, crater_x, crater_y,
        crater_z + 0.55f, kCraterSourceRadiusX * 0.55f, radius_y * 0.55f,
        (SDL_FColor){1.0f, 0.56f, 0.08f, 0.84f});
  }

  if (params->detail < kSimBackgroundVoxelDetail_High ||
      params->style < kSimBackgroundVoxelStyle_Architectural)
    return;
  int puff_count = params->detail == kSimBackgroundVoxelDetail_Ultra ? 4 : 2;
  for (int puff = 0; puff < puff_count; puff++) {
    unsigned age = (params->game_frame + (unsigned)puff * 17u) % 48u;
    if (age > 34u) continue;
    float age_fraction = age / 34.0f;
    float size = 2.4f + age_fraction * 2.8f;
    float drift_x = ((puff & 1) ? 1.0f : -1.0f) * age_fraction * 5.0f;
    float drift_y = -age_fraction * 4.0f;
    float z0 = crater_z + 2.0f + age_fraction * 17.0f;
    float opacity = 0.72f - age_fraction * 0.34f;
    SDL_FColor smoke = {
      0.66f + age_fraction * 0.10f,
      0.63f + age_fraction * 0.10f,
      0.58f + age_fraction * 0.10f,
      opacity,
    };
    AppendProjectedSolidEffectBox(
        params, axis, origin_x, origin_y,
        crater_x + drift_x - size * 0.5f,
        crater_y + drift_y - size * 0.5f, z0,
        crater_x + drift_x + size * 0.5f,
        crater_y + drift_y + size * 0.5f, z0 + size,
        smoke);
  }
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

static void AddMountainStackTile(
    const MountainTileContext *context,
    float baseline_left, float baseline_right,
    float maximum_rise_left, float maximum_rise_right,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t flags,
    int *count) {
  bool mirror_x =
      (flags & kSimBackgroundMountainCapTile_MirrorX) != 0;
  float x0 = destination_cell_x * kSimBackgroundCellPixels;
  float y0 = destination_cell_y * kSimBackgroundCellPixels;
  float x1 = x0 + kSimBackgroundCellPixels;
  float y1 = y0 + kSimBackgroundCellPixels;
  float u0 = source_cell_x * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float v0 = source_cell_y * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float u1 = (source_cell_x + 1) * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float v1 = (source_cell_y + 1) * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float source_x[4] = {x0, x1, x1, x0};
  float source_y[4] = {y0, y0, y1, y1};
  /* Every layer retains the exact front silhouette orientation. Flipping only
   * the rear copy moves off-centre tip pixels across their tile and turns one
   * peak into two visible horns, so the mapping does not vary by layer. */
  const SDL_FPoint uv[4] = {
    {mirror_x ? u1 : u0, v0},
    {mirror_x ? u0 : u1, v0},
    {mirror_x ? u0 : u1, v1},
    {mirror_x ? u1 : u0, v1},
  };
  /* The displacement of the rearmost copy. Every nearer layer is a fixed
   * fraction of it, so the taper is resolved once per corner instead of once
   * per corner per layer. */
  const SimBackgroundMountainRelief *relief = context->relief;
  float corner_baseline[4], rearmost_depth[4];
  for (int point = 0; point < 4; point++) {
    bool left = point == 0 || point == 3;
    corner_baseline[point] = left ? baseline_left : baseline_right;
    float rise = corner_baseline[point] - source_y[point];
    /* The relief module states the displacement as a signed northward offset,
     * which is its magnitude along whichever way the camera says is back.
     * Negating recovers that magnitude; the direction supplies the sign, so a
     * zero-yaw camera lands on exactly the old northward value. */
    rearmost_depth[point] = -SimBackgroundMountainRelief_StackOffsetY(
        relief, (uint8_t)(relief->stack_layer_count - 1), rise,
        left ? maximum_rise_left : maximum_rise_right);
  }
  float layers = (float)(relief->stack_layer_count - 1);
  /* Emit rear copies first for coherent material batching. Visibility is
   * resolved per fragment by the shared D32 target, so this loop order is not
   * a correctness dependency. */
  for (int layer = relief->stack_layer_count - 1; layer >= 0; layer--) {
    float fraction = layers > 0.0f ? (float)layer / layers : 0.0f;
    float local_x[4], local_y[4], local_z[4];
    for (int point = 0; point < 4; point++) {
      float depth = rearmost_depth[point] * fraction;
      MountainPlanePoint(
          source_x[point], source_y[point], corner_baseline[point], relief,
          depth * context->stack_direction.x,
          depth * context->stack_direction.y,
          &local_x[point], &local_y[point], &local_z[point]);
      local_z[point] *= context->height_scale;
    }
    AddProjectedMountainReliefFace(
        context->params, context->axis, context->origin_x, context->origin_y,
        local_x, local_y, local_z, uv, kMountainFullIntensity,
        kMountainFullIntensity, count);
  }
}

/* Each relief layer is a flat inclined plane with the authentic art painted on
 * it: its cross-section is a straight line from the ground at the front up and
 * back to the ridge. That reads as a mountain only from the canonical camera.
 * The wedge between the plane and the ground is empty and open at the sides,
 * so any other angle shows a tilted board whose upper half hangs in the air -
 * and no amount of extra stack layers changes that, because the layers are
 * parallel copies of the same plane and thicken it along the ground rather
 * than filling underneath it.
 *
 * This closes the silhouette: wherever a mountain cell has no neighbour, a
 * quad drops from that cell's edge of the plane straight down to z=0, giving
 * the mass a visible side and a footprint. It follows the outline in both
 * axes, stretches the tile's own art down the wall, and ramps its shading
 * from the face's own value to a shaded base. */
enum {
  /* How far into the tile the wall's ground edge samples. The mountain tiles
   * carry a one-to-two pixel dither margin along their outline that is both
   * grass-coloured and partly transparent, so a wall sampled from the outline
   * alone shows green see-through streaks. Stretching the tile's own interior
   * across the quad is what makes it read as rock. */
  kMountainSkirtStretchPixels = 8,
  /* A wall needs more than a couple of rows of outline to be worth standing. */
  kMountainSkirtMinimumRows = 2,
};
/* Shading down the wall. The mountain face is drawn fully lit, so a wall at a
 * single darker value meets it in a hard tonal step, and any pixel of
 * misalignment at that junction then reads as a shelf rather than as an edge.
 * Ramping from almost the face's own value at the top to a shaded base makes
 * the join continuous and lets the wall still read as a side. */
static const uint8_t kMountainSkirtTopBrightness = 240;
static const uint8_t kMountainSkirtBaseBrightness = 150;
/* The wall stands a fraction of a pixel proud of the art and starts a hair
 * above the face it meets. Both are below one source pixel, and they close
 * the sub-pixel seam the fitted line cannot follow exactly - the outline is
 * jagged row to row and a single quad can only be straight. */
static const float kMountainSkirtOutwardMargin = 0.5f;
static const float kMountainSkirtOverlapPixels = 0.35f;
/* How far a row's outline may sit inside the fitted line before that row is
 * treated as dither fringe rather than part of the wall. */
static const float kMountainSkirtStraightTolerance = 1.5f;
/* Where a tile's art meets the open side of its cell. Derived from the
 * immutable silhouette table, so it is resolved once per tile/side and reused
 * for every frame, object and town. */
typedef struct MountainSkirtProfile {
  bool resolved;
  bool present;
  uint8_t first_row, last_row;
  /* Where the wall stands, fitted to the tile's whole outline and then pushed
   * outward until no row of art lies outside it. Taking the two end rows
   * alone slants the line inward at a base tile, whose last row is the
   * dithered fringe several pixels in, and the mountain then overhangs its
   * own wall by three or four pixels. */
  float wall_first, wall_last;
  /* Columns the quad samples: the outline for the top edge so it joins the
   * mountain without a seam, the interior for the ground edge. */
  uint8_t outer_first, outer_last;
  uint8_t inner_first, inner_last;
} MountainSkirtProfile;

/* Resolved from the compile-time silhouette table, so an entry is the same
 * value whenever it is computed and never needs invalidating - not on a town
 * change, not on Reset. It is filled lazily from the present thread, which is
 * the only thread that renders; a second renderer thread would need this
 * built up front instead. */
static MountainSkirtProfile g_skirt_profiles[256][2];

static bool MountainSilhouetteOpaque(uint8_t tile, int column, int row) {
  bool opaque = false;
  if (!SimBackgroundMountainSilhouette_Lookup(tile, column, row, &opaque))
    return true;  /* Unknown tiles are treated as solid elsewhere too. */
  return opaque;
}

/* Outermost and innermost opaque columns of one row, scanning from `edge`
 * toward the far side of the tile. */
static bool MountainRowRun(uint8_t tile, int row, int edge, int step,
                           int *outer, int *inner) {
  int first = -1, last = -1;
  for (int at = 0; at < kSimBackgroundCellPixels; at++) {
    int column = edge + at * step;
    if (!MountainSilhouetteOpaque(tile, column, row)) continue;
    if (first < 0) first = column;
    last = column;
  }
  if (first < 0) return false;
  int reach = first + step * kMountainSkirtStretchPixels;
  /* Never sample past the run: beyond it is the tile's other outline. */
  if ((step > 0 && reach > last) || (step < 0 && reach < last)) reach = last;
  /* The fringe rows where a mountain's foot meets grass are dithered, not
   * solid runs, so the column that far in can still be a hole. Back off to
   * the nearest opaque one rather than punching the wall through. */
  while (reach != first && !MountainSilhouetteOpaque(tile, reach, row))
    reach -= step;
  *outer = first;
  *inner = reach;
  return true;
}

/* Least-squares fit of the outline over a row span, as column = intercept +
 * slope * (row - first_row). */
static void MountainFitOutline(uint8_t tile, int edge, int step,
                               int first_row, int last_row,
                               float *slope, float *intercept) {
  float rows = 0.0f, sum_row = 0.0f, sum_column = 0.0f;
  float sum_row_column = 0.0f, sum_row_row = 0.0f;
  for (int row = first_row; row <= last_row; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    float offset = (float)(row - first_row);
    rows += 1.0f;
    sum_row += offset;
    sum_column += (float)outer;
    sum_row_column += offset * (float)outer;
    sum_row_row += offset * offset;
  }
  if (rows <= 0.0f) {
    *slope = 0.0f;
    *intercept = 0.0f;
    return;
  }
  float denominator = rows * sum_row_row - sum_row * sum_row;
  *slope = denominator > 0.0001f
      ? (rows * sum_row_column - sum_row * sum_column) / denominator : 0.0f;
  *intercept = (sum_column - *slope * sum_row) / rows;
}

static const MountainSkirtProfile *MountainSkirtProfileFor(uint8_t tile,
                                                           bool right_edge) {
  MountainSkirtProfile *profile = &g_skirt_profiles[tile][right_edge ? 1 : 0];
  if (profile->resolved) return profile;
  profile->resolved = true;
  int step = right_edge ? -1 : 1;
  int edge = right_edge ? kSimBackgroundCellPixels - 1 : 0;

  int first_row = -1, last_row = -1;
  for (int row = 0; row < kSimBackgroundCellPixels; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    if (first_row < 0) first_row = row;
    last_row = row;
  }
  if (first_row < 0 || last_row - first_row < kMountainSkirtMinimumRows)
    return profile;

  /* A mountain's foot dissolves into the lawn over two or three dithered
   * rows whose outline sits several pixels inside the rest of the wall.
   * Following them would either drag the wall into the mountain, leaving the
   * art overhanging it, or leave the wall protruding past the art once it is
   * biased back out. Ending the wall where the outline stops being straight
   * avoids both, and costs nothing: those rows are within a pixel of the
   * ground already. A genuine diagonal deviates from its own fit by nothing,
   * so it is never trimmed. */
  float slope, intercept;
  while (last_row - first_row > kMountainSkirtMinimumRows) {
    MountainFitOutline(tile, edge, step, first_row, last_row,
                       &slope, &intercept);
    int outer, inner;
    if (!MountainRowRun(tile, last_row, edge, step, &outer, &inner)) break;
    float line = intercept + slope * (float)(last_row - first_row);
    float deviation = (float)step * ((float)outer - line);
    if (deviation <= kMountainSkirtStraightTolerance) break;
    last_row--;
  }
  MountainFitOutline(tile, edge, step, first_row, last_row,
                     &slope, &intercept);

  /* Push the fitted line outward until no row of art lies outside it. */
  float bias = 0.0f;
  for (int row = first_row; row <= last_row; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    float line = intercept + slope * (float)(row - first_row);
    float outside = (float)step * (line - (float)outer);
    if (outside > bias) bias = outside;
  }

  int outer_first, inner_first, outer_last, inner_last;
  if (!MountainRowRun(tile, first_row, edge, step,
                      &outer_first, &inner_first) ||
      !MountainRowRun(tile, last_row, edge, step, &outer_last, &inner_last))
    return profile;

  float span = (float)(last_row - first_row);
  profile->present = true;
  profile->first_row = (uint8_t)first_row;
  profile->last_row = (uint8_t)last_row;
  profile->wall_first = intercept - (float)step * bias;
  profile->wall_last = intercept + slope * span - (float)step * bias;
  profile->outer_first = (uint8_t)outer_first;
  profile->outer_last = (uint8_t)outer_last;
  profile->inner_first = (uint8_t)inner_first;
  profile->inner_last = (uint8_t)inner_last;
  return profile;
}

/* Displacement of the outermost stack copy on one side of the mountain. The
 * relief stack recedes along the camera's away axis, which under yaw has a
 * sideways component - up to 2.8px at Ultra - so a wall standing at the front
 * copy is overhung by the rear ones. Zero when the stack fans the other way,
 * because then the front copy already is the outermost. */
static void MountainStackOutwardOffset(
    const SimBackgroundMountainRelief *relief,
    SimBackgroundStackDirection direction, float rise, float maximum_rise,
    bool right_edge, float *offset_x, float *offset_y) {
  *offset_x = 0.0f;
  *offset_y = 0.0f;
  if (relief->stack_layer_count < 2) return;
  float magnitude = -SimBackgroundMountainRelief_StackOffsetY(
      relief, (uint8_t)(relief->stack_layer_count - 1), rise, maximum_rise);
  float sideways = magnitude * direction.x;
  if ((right_edge ? sideways : -sideways) <= 0.0f) return;
  *offset_x = sideways;
  *offset_y = magnitude * direction.y;
}

static void AddMountainSkirtTile(
    const MountainTileContext *context,
    float baseline, float maximum_rise,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t source_tile,
    bool right_edge, int *count) {
  const MountainSkirtProfile *profile =
      MountainSkirtProfileFor(source_tile, right_edge);
  if (!profile->present) return;

  float outward = right_edge ? kMountainSkirtOutwardMargin
                             : -kMountainSkirtOutwardMargin;
  float cell_x = destination_cell_x * (float)kSimBackgroundCellPixels +
      outward;
  float cell_top = destination_cell_y * (float)kSimBackgroundCellPixels;
  /* The wall's top edge is the outline itself, so it slants with a shoulder
   * tile's diagonal instead of standing at the cell boundary beside it. */
  float north_x = cell_x + profile->wall_first;
  float south_x = cell_x + profile->wall_last;
  float y0 = cell_top + (float)profile->first_row;
  float y1 = cell_top + (float)profile->last_row + 1.0f;
  float north_offset_x, north_offset_y, south_offset_x, south_offset_y;
  MountainStackOutwardOffset(context->relief, context->stack_direction,
                             baseline - y0, maximum_rise, right_edge,
                             &north_offset_x, &north_offset_y);
  MountainStackOutwardOffset(context->relief, context->stack_direction,
                             baseline - y1, maximum_rise, right_edge,
                             &south_offset_x, &south_offset_y);
  float north_wall_x, north_y, north_z, south_wall_x, south_y, south_z;
  MountainPlanePoint(north_x, y0, baseline, context->relief,
                     north_offset_x, north_offset_y,
                     &north_wall_x, &north_y, &north_z);
  MountainPlanePoint(south_x, y1, baseline, context->relief,
                     south_offset_x, south_offset_y,
                     &south_wall_x, &south_y, &south_z);
  const float overlap = kMountainSkirtOverlapPixels * context->height_scale;
  north_z = north_z * context->height_scale + overlap;
  south_z = south_z * context->height_scale + overlap;
  /* Art already sitting on the ground has no wedge to close. */
  if (north_z <= overlap) return;

  float local_x[4] = {
    north_wall_x, south_wall_x, south_wall_x, north_wall_x,
  };
  float local_y[4] = {north_y, south_y, south_y, north_y};
  float local_z[4] = {north_z, south_z, 0.0f, 0.0f};
  float cell_u = source_cell_x * (float)kSimBackgroundCellPixels;
  float cell_v = source_cell_y * (float)kSimBackgroundCellPixels;
  /* Stretched down the wall rather than smeared along it: the top edge takes
   * the outline so it joins the mountain without a seam, and the ground edge
   * takes the interior, so the quad is the tile's own rock in the town's own
   * palette. */
  float v0 = (cell_v + profile->first_row + 0.5f) /
      (float)kSimTownCanvasPixels;
  float v1 = (cell_v + profile->last_row + 0.5f) /
      (float)kSimTownCanvasPixels;
  SDL_FPoint uv[4] = {
    {(cell_u + profile->outer_first + 0.5f) / kSimTownCanvasPixels, v0},
    {(cell_u + profile->outer_last + 0.5f) / kSimTownCanvasPixels, v1},
    {(cell_u + profile->inner_last + 0.5f) / kSimTownCanvasPixels, v1},
    {(cell_u + profile->inner_first + 0.5f) / kSimTownCanvasPixels, v0},
  };
  /* Points 0 and 1 are the top edge against the mountain; 2 and 3 stand on
   * the ground. */
  const uint8_t brightness[4] = {
    kMountainSkirtTopBrightness, kMountainSkirtTopBrightness,
    kMountainSkirtBaseBrightness, kMountainSkirtBaseBrightness,
  };
  AddProjectedMountainReliefFace(
      context->params, context->axis, context->origin_x, context->origin_y,
      local_x, local_y, local_z, uv, brightness, kMountainFullIntensity,
      count);
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
  /* Cleared every frame so a town with no volcano cannot hand the eruption
   * arcs the last town's crater. */
  g_crater_anchor.valid = false;
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
    AppendVolcanoEffects(
        context.params, context.axis, context.relief, object,
        context.origin_x, context.origin_y, baseline, context.height_scale);
  }
  return count;
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

int SimBackgroundMountainRender_BuildFaces(
    const SimBackgroundVoxelRenderParams *params) {
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  const SimBackgroundMountainField *field = &scene->mountains;
  if (!field->cell_count) return 0;
  SimBackgroundMountainRelief relief;
  SimBackgroundMountainRelief_Resolve(
      EffectiveMountainDetail(params), &relief);
  if (!relief.stack_layer_count) return 0;

  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  SimBackgroundModelLean mountain_lean =
      SimBackgroundVoxelProject_CameraFacingLean(
          params, params->facing == kSimBackgroundVoxelFacing_PerModel
              ? 0.44f : 0.35f);
  SimBackgroundProjectionAxis mountain_axis =
      SimBackgroundVoxelProject_Axis(params, mountain_lean);
  MountainTileContext context = {
    .params = params,
    .axis = &mountain_axis,
    .relief = &relief,
    .stack_direction = MountainStackDirection(params),
    /* Landscape magnitude translates the mountain's BASE through
     * SimBackgroundVoxelProject_TerrainLiftPixels at every vertex. It must
     * not resize the mountain's separately authored relief; a 50% landscape
     * remains a full mountain on gentler ground rather than turning the
     * mountain itself into a hill. */
    .height_scale = 1.0f,
    .origin_x = origin_x,
    .origin_y = origin_y,
  };
  SimBackgroundMountainObjectList mountain_objects;
  if (SimBackgroundMountainObjects_Build(
          field, &scene->mountain_caps, &mountain_objects)) {
    return BuildProjectedMountainObjectFaces(
        &context, field, &mountain_objects);
  }
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
  return count;
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
}
