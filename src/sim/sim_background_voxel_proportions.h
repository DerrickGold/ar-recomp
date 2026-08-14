#ifndef SIM_BACKGROUND_VOXEL_PROPORTIONS_H
#define SIM_BACKGROUND_VOXEL_PROPORTIONS_H

#include "sim_background_voxels.h"

typedef struct SimBackgroundVoxelProportions {
  float footprint_scale;
  float height_scale;
} SimBackgroundVoxelProportions;

/* Final presentation proportions, independent of model geometry and detail
 * level. Models remain authored in exact town-pixel units; this table is the
 * single place where families are balanced against one another in SIM3D. */
const SimBackgroundVoxelProportions *SimBackgroundVoxelProportions_Get(
    SimBackgroundVoxelKind kind);

#endif  /* SIM_BACKGROUND_VOXEL_PROPORTIONS_H */
