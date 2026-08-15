#ifndef SIM_BACKGROUND_VOXEL_LOD_H
#define SIM_BACKGROUND_VOXEL_LOD_H

#include "sim_background_voxel_quality.h"

/* Resolves a per-object detail ceiling from its projected screen height. The
 * player's detail choice always remains the upper bound, so Adaptive can only
 * save work; it never silently opts into more geometry than requested. */
SimBackgroundVoxelDetail SimBackgroundVoxelLod_Resolve(
    SimBackgroundVoxelDetail requested,
    SimBackgroundVoxelLod mode,
    float projected_height_pixels);

#endif  /* SIM_BACKGROUND_VOXEL_LOD_H */
