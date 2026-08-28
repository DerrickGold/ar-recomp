#ifndef SIM_BACKGROUND_VOXEL_PROJECT_H
#define SIM_BACKGROUND_VOXEL_PROJECT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "scene3d_math.h"
#include "sim_background_voxel_palette.h"
#include "sim_background_voxel_quality.h"
#include "sim_background_voxel_renderer.h"
#include "sim_background_voxel_types.h"
#include "sim_town_terrain.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

/* The camera transform every SIM background render unit shares.
 *
 * Models, mountain relief, the terrain mesh and the ground contact decals all
 * place their vertices through this one path: a camera-facing lean, a
 * normalized presentation axis, and a town-texture-to-world mapping. Keeping
 * it in a single unit is what keeps an object's base and the terrain beneath
 * it from drifting apart, and it is the dependency the mountain and terrain
 * units are built on. */

enum {
  /* Staging for the shadow mask, the one path that still batches through
   * SDL_RenderGeometry rather than the depth pass. Sized to hold a real
   * town's casters in a single draw - Bloodpool's 168 objects come to about
   * 1200 quads - and to flush cleanly rather than truncate beyond that. */
  kSimBackgroundBatchMaxQuads = 2048,
  kSimBackgroundBatchMaxVertices = kSimBackgroundBatchMaxQuads * 4,
  kSimBackgroundBatchMaxIndices = kSimBackgroundBatchMaxQuads * 6,
};

typedef struct SimBackgroundGeometryBatch {
  ArRenderVertex2D vertices[kSimBackgroundBatchMaxVertices];
  int32_t indices[kSimBackgroundBatchMaxIndices];
  int vertex_count, index_count;
} SimBackgroundGeometryBatch;

typedef struct SimBackgroundProjectedFace {
  Scene3DPoint points[4];
  float gpu_depth[4];
  uint8_t material;
  uint8_t brightness[4];
} SimBackgroundProjectedFace;

typedef struct SimBackgroundModelLean {
  float x_per_height;
  float y_per_height;
} SimBackgroundModelLean;

typedef struct SimBackgroundProjectionAxis {
  float x_per_height;
  float y_per_height;
  float height_scale;
} SimBackgroundProjectionAxis;

/* The no-lean axis, for geometry that is measured rather than presented:
 * terrain corners, LOD probes and anything else that must stay where the map
 * says it is. */
extern const SimBackgroundProjectionAxis kSimBackgroundUprightProjectionAxis;

/* Precompose authored texture coordinates directly into clip space. Must be
 * called after source/viewport/matrix reach their final values for a pass. */
void SimBackgroundVoxelProject_Prepare(
    SimBackgroundVoxelRenderParams *params);

void SimBackgroundVoxelProject_TexturePointToWorld(
    const SimBackgroundVoxelRenderParams *params,
    float texture_x, float texture_y, float height_pixels,
    float *world_x, float *world_y, float *world_z);

/* Audited terrain altitude under a town-map point, already scaled by the
 * player's Landscape height. Zero in a build without terrain elevation, which
 * is what lets every caller add it unconditionally.
 *
 * Inline, and deliberately so. Terrain height feeds the depth of every model
 * vertex, every terrain corner and every contact decal, and the visible mesh
 * and the D32 occluder have to agree on it bit for bit or the shadow mask
 * flickers on and off across the ground. Splitting the renderer moved these
 * two converters across a translation-unit boundary; keeping them inline is
 * what keeps every caller's arithmetic identical to the single-file version. */
static inline float SimBackgroundVoxelProject_TerrainLiftPixels(
    const SimBackgroundVoxelRenderParams *params,
    float town_pixel_x, float town_pixel_y) {
#if AR_SIM3D_TERRAIN_ELEVATION
  if (!params || params->town < 1 ||
      params->town > kSimTownTerrainTownCount)
    return 0.0f;
  return SimTownTerrain_ScaledHeightPixels(
      SimTownTerrain_HeightUnitsAt(
          params->town, town_pixel_x, town_pixel_y),
      (float)params->landscape_height_pct);
#else
  (void)params; (void)town_pixel_x; (void)town_pixel_y;
  return 0.0f;
#endif
}

/* Audited elevation units to source pixels, for a caller that already holds a
 * height. Inline for the same reason. */
static inline float SimBackgroundVoxelProject_TerrainUnitsToPixels(
    const SimBackgroundVoxelRenderParams *params, float height_units) {
  return SimTownTerrain_ScaledHeightPixels(
      height_units, (float)params->landscape_height_pct);
}

SimBackgroundModelLean SimBackgroundVoxelProject_CameraFacingLean(
    const SimBackgroundVoxelRenderParams *params, float billboard_blend);

/* Normalizes a lean into a projection axis once per object kind rather than
 * once per vertex. */
SimBackgroundProjectionAxis SimBackgroundVoxelProject_Axis(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundModelLean lean);
void SimBackgroundVoxelProject_ResolveAxes(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount]);

void SimBackgroundVoxelProject_LeanedPointToWorld(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y, float height_pixels,
    float *world_x, float *world_y, float *world_z);

bool SimBackgroundVoxelProject_Point(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y, float height_pixels,
    Scene3DPoint *out, float *clip_depth);

/* Terrain altitude is a world translation; only the object's authored height
 * follows its camera-facing presentation axis. The DepthGround variant lets a
 * rigid span keep a horizontal visual datum while submitting a conservative
 * depth envelope. */
bool SimBackgroundVoxelProject_GroundedVertexWithDepthGround(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y,
    float height_pixels, float visual_ground_pixels,
    float depth_ground_pixels,
    Scene3DPoint *out, float *gpu_depth);
bool SimBackgroundVoxelProject_GroundedVertex(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y,
    float height_pixels, float ground_pixels,
    Scene3DPoint *out, float *gpu_depth);
bool SimBackgroundVoxelProject_GroundedPoint(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y,
    float height_pixels, float ground_pixels,
    Scene3DPoint *out, float *clip_depth);

void SimBackgroundVoxelProject_FlushBatch(
    ArRenderDevice *device, SimBackgroundGeometryBatch *batch);

/* Submits one material-shaded quad to the shared solid depth layer. */
void SimBackgroundVoxelProject_AppendFace(
    const SimBackgroundProjectedFace *face,
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelShading shading);

/* True when a projected quad encloses too little area to raster meaningfully.
 * Rejecting these keeps a grazing face from becoming a stray hairline. */
bool SimBackgroundVoxelProject_IsDegenerate(const Scene3DPoint points[4]);

#endif  /* SIM_BACKGROUND_VOXEL_PROJECT_H */
