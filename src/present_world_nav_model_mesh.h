/* Presentation-private retained sources for globe and active-town models. */
#ifndef PRESENT_WORLD_NAV_MODEL_MESH_H
#define PRESENT_WORLD_NAV_MODEL_MESH_H
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_background_voxel_models.h"
#include "present_sim_globe_mapping.h"
#include "sim/sim_background_voxel_project.h"

typedef struct WorldNavigationModelSource {
  SimBackgroundVoxelObject object;
  SimBackgroundVoxelDetail detail;
  uint16_t object_index; /* Identity within the immutable capture revision. */
  float source_x, source_y, centre_x, centre_y, anchor_height;
} WorldNavigationModelSource;

typedef struct WorldNavigationModelSourceStyle {
  SimGlobeMapping embedding; /* town=0 preserves the ordinary globe source */
  uint32_t model_revision, surface_revision;
  float chart_radius_tiles, tile_world;
  int height_percent, light_azimuth, light_elevation;
  SimBackgroundVoxelStyle style;
  bool lighting;
  /* Active SIM publishes each structure's actual authored phase, including
   * individually stopped windmills; navigation instead selects clock poses. */
  bool captured_poses;
  Sim3DDepthSurfaceFocus focus;
} WorldNavigationModelSourceStyle;

/* Default on; AR_SIM3D_WORLD_GPU_MODELS=0 opts out until resource reset. */
bool WorldNavigationModelMesh_Enabled(void);
/* Sources are already whole-object culled and LOD selected, in draw order.
 * No FrameSlot or compiler view is retained. False queues nothing: the caller
 * can use its existing multicore renderer. Camera/viewport/pose changes alone
 * never republish the source vertices unless the 16 MiB residency cache needs
 * eviction. Eviction rebuilds the current selection without changing detail.
 * Content rejection is selection-local, not a permanent device failure.
 * One bounded opaque handle is owned. */
bool WorldNavigationModelMesh_Draw(const WorldNavigationModelSource *sources,
    size_t count, const WorldNavigationModelSourceStyle *style,
    const Sim3DDepthRadialTransform *transform);
/* Caller certifies the complete projection/source key is unchanged. Only the
 * pose uniform may change. Rejects invalidated GPU storage without queuing. */
bool WorldNavigationModelMesh_Repeat(const Sim3DDepthRadialTransform *transform);
void WorldNavigationModelMesh_Reset(void);

/* Active continuous-town presentation. Sources must all belong to embedding's
 * town. Ground positions remain curved; only authored model height uses the
 * supplied SIM axes. Bridges retain the geometric radial path at the caller.
 * Static models and the three windmill poses are retained independently;
 * captured_poses instead updates only a separate small windmill stream using
 * the supplied individual phases, leaving static buildings resident. No new backend
 * contract, per-object draw, native state or frame pointer is retained. */
bool WorldNavigationModelMesh_DrawFacingTown(
    const WorldNavigationModelSource *sources, size_t count,
    const WorldNavigationModelSourceStyle *style,
    SimBackgroundVoxelShading shading,
    const SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount],
    const float matrix[16], unsigned wind_pose);
#endif
