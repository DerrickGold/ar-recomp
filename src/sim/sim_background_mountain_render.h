#ifndef SIM_BACKGROUND_MOUNTAIN_RENDER_H
#define SIM_BACKGROUND_MOUNTAIN_RENDER_H

#include "sim_background_voxel_renderer.h"
#include "sim_background_mountain_mesh.h"
#include "sim_background_voxel_project.h"

/* Projected mountain relief: the camera-receding stack copies, the silhouette
 * skirts that close a range down to the ground, the reconstructed north caps,
 * and the volcano's crater glow and eruption anchor.
 *
 * This is a separate unit because it is the one background surface built from
 * the town's mountain metatile art rather than from authored voxel models. It
 * owns its own projected-face budget and per-column peak/base tables, and the
 * general renderer only asks it for a face count and then for its
 * submission. */

/* Publishes the current projected relief into the unit's own face buffer,
 * retaining static geometry while its scene/camera inputs remain unchanged.
 * Dynamic volcano effects are still emitted for every presentation. Call
 * before SubmitFaces. */
int SimBackgroundMountainRender_BuildFaces(
    const SimBackgroundVoxelRenderParams *params);

/* Submits the first `count` projected faces to the shared mountain depth
 * layer. Submission order against models is immaterial: they share one D32
 * attachment and the GPU resolves visibility per pixel. */
void SimBackgroundMountainRender_SubmitFaces(int count);

/* Synchronous source-space seam for an alternate surface embedding. Emits
 * the SAME authored relief/caps/skirts as BuildFaces, before projection.
 * Local coordinates are town pixels; only z uses the supplied facing axis.
 * UVs address the published SIM mountain atlas. The callback must consume
 * borrowed arrays before returning. No effect geometry, draw, projected-face
 * or image publication, or retained caller pointer is produced by this function.
 * Returns emitted face count, or -1 for invalid presentation inputs. */
typedef void (*SimBackgroundMountainSourceEmit)(void *user,
    const float x[4], const float y[4], const float z[4],
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainMeshUV uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4]);
/* The complete camera-dependent source recipe, excluding screen placement.
 * Fixed-LOD pan/zoom leaves this unchanged; adaptive zoom changes detail only
 * when crossing an actual LOD boundary. Owned values, no retained inputs. */
typedef struct SimBackgroundMountainSourceStyle {
  SimBackgroundProjectionAxis axis;
  SimBackgroundMountainMeshDirection stack_direction;
  uint8_t detail;
} SimBackgroundMountainSourceStyle;
bool SimBackgroundMountainRender_SourceStyle(
    const SimBackgroundVoxelRenderParams *params, SimBackgroundMountainSourceStyle *out);
int SimBackgroundMountainRender_EmitSource(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundMountainSourceEmit emit, void *user);

/* Owned effect recipe, resolved alongside static source geometry. Native XY
 * and authored rise precede terrain lift, facing and world projection. This
 * separates the shared art/timing from either consumer's surface mapping. */
typedef struct SimBackgroundCraterSource {
  float x, y, z, radius_y;
  SimBackgroundProjectionAxis axis;
} SimBackgroundCraterSource;
enum { kSimBackgroundCraterSourceMaximum = 64 };
typedef struct SimBackgroundMountainEffectSource {
  unsigned count;
  SimBackgroundCraterSource craters[kSimBackgroundCraterSourceMaximum];
} SimBackgroundMountainEffectSource;
typedef bool (*SimBackgroundMountainEffectEmit)(void *user,
    const float x[4], const float y[4], const float z[4],
    const SimBackgroundProjectionAxis *axis, ArRenderColorF color);
bool SimBackgroundMountainRender_EffectSource(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundMountainEffectSource *out);
bool SimBackgroundMountainRender_EmitEffects(
    const SimBackgroundMountainEffectSource *source, uint16_t frame,
    uint8_t detail, uint8_t style, SimBackgroundMountainEffectEmit emit, void *user);

void SimBackgroundMountainRender_Reset(void);

#endif  /* SIM_BACKGROUND_MOUNTAIN_RENDER_H */
