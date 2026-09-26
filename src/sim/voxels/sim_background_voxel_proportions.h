#ifndef AR_SIM_BACKGROUND_VOXEL_PROPORTIONS_H
#define AR_SIM_BACKGROUND_VOXEL_PROPORTIONS_H
/* SimBackgroundVoxelProportions: per-kind footprint and height scale, the one
 * table that balances model families against each other in the 3D town.
 * Phase: pure.
 * Tests: tests/sim_background_voxel_proportions_test.c */

#include "sim/voxels/sim_background_voxel_types.h"

typedef struct SimBackgroundVoxelProportions {
  float footprint_scale;
  float height_scale;
} SimBackgroundVoxelProportions;

/* Final presentation proportions, independent of model geometry and detail
 * level. Models remain authored in exact town-pixel units; this table is the
 * single place where families are balanced against one another in SIM3D. */
const SimBackgroundVoxelProportions *SimBackgroundVoxelProportions_Get(
    SimBackgroundVoxelKind kind);

#endif  /* AR_SIM_BACKGROUND_VOXEL_PROPORTIONS_H */
