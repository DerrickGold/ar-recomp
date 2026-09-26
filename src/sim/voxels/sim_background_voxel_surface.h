#ifndef AR_SIM_BACKGROUND_VOXEL_SURFACE_H
#define AR_SIM_BACKGROUND_VOXEL_SURFACE_H
/* SimBackgroundVoxelSurface: the outward normal of a model face, correcting
 * the model builders' mixed winding in one place for lighting and any later
 * surface analysis.
 * Phase: pure.
 * Tests: tests/sim_background_voxel_lighting_test.c */

#include <stdbool.h>

#include "sim/voxels/sim_background_voxel_models.h"

typedef struct SimBackgroundVoxelSurfaceNormal {
  float x, y, z;
} SimBackgroundVoxelSurfaceNormal;

/* Returns the authored outward unit normal for an opaque model face.
 *
 * The model builders deliberately use the original AddBox convention: side
 * faces are wound inward while top and sloped roof faces are wound outward.
 * Keeping that correction here gives lighting and future surface analysis
 * one authoritative interpretation of the geometry. */
bool SimBackgroundVoxelSurface_OutwardNormal(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelSurfaceNormal *out);

#endif  /* AR_SIM_BACKGROUND_VOXEL_SURFACE_H */
