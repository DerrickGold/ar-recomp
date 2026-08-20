#ifndef SIM_BACKGROUND_VOXEL_TERRAIN_DEPTH_H
#define SIM_BACKGROUND_VOXEL_TERRAIN_DEPTH_H

#include "sim_background_voxel_renderer.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

#if AR_SIM3D_TERRAIN_ELEVATION

/* The audited terrain surface as depth geometry: one top per cell plus the
 * exposed skirt of every baked hard edge, clipped to the interval on which
 * each cell actually owns the drop.
 *
 * The maps and their cliff topology are immutable, so this unit decodes a
 * town once and then caches the projected result by camera, viewport,
 * landscape scale and town. The depth composite and the earlier shadow
 * receiver consume that same projection instead of transforming every corner
 * twice, and a still camera retains it across frames. */

/* Appends the cached (or freshly projected) terrain surface to the active
 * depth pass. Used by both the shadow receiver and the object composite. */
void SimBackgroundVoxelTerrainDepth_Append(
    const SimBackgroundVoxelRenderParams *params);

/* Clip-depth extrema of the terrain surface, for clipping the interleaved
 * actor bands against it. */
void SimBackgroundVoxelTerrainDepth_GroundDepthRange(
    const SimBackgroundVoxelRenderParams *params,
    float *minimum, float *maximum);

void SimBackgroundVoxelTerrainDepth_Reset(void);

#endif  /* AR_SIM3D_TERRAIN_ELEVATION */

#endif  /* SIM_BACKGROUND_VOXEL_TERRAIN_DEPTH_H */
