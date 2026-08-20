#ifndef SIM_BACKGROUND_MOUNTAIN_RENDER_H
#define SIM_BACKGROUND_MOUNTAIN_RENDER_H

#include "sim_background_voxel_renderer.h"

/* Projected mountain relief: the camera-receding stack copies, the silhouette
 * skirts that close a range down to the ground, the reconstructed north caps,
 * and the volcano's crater glow and eruption anchor.
 *
 * This is a separate unit because it is the one background surface built from
 * the town's mountain metatile art rather than from authored voxel models. It
 * owns its own projected-face budget and per-column peak/base tables, and the
 * general renderer only asks it for a face count and then for its
 * submission. */

/* Projects this frame's relief into the unit's own face buffer and returns how
 * many faces it produced. Call before SubmitFaces. */
int SimBackgroundMountainRender_BuildFaces(
    const SimBackgroundVoxelRenderParams *params);

/* Submits the first `count` projected faces to the shared mountain depth
 * layer. Submission order against models is immaterial: they share one D32
 * attachment and the GPU resolves visibility per pixel. */
void SimBackgroundMountainRender_SubmitFaces(int count);

void SimBackgroundMountainRender_Reset(void);

#endif  /* SIM_BACKGROUND_MOUNTAIN_RENDER_H */
