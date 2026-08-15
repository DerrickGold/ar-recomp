#ifndef SIM_BACKGROUND_VOXEL_DEPTH_H
#define SIM_BACKGROUND_VOXEL_DEPTH_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_voxel_quality.h"

/* Depth interleaving is also a performance boundary. Low retains one batched
 * building layer; richer meshes earn progressively finer sprite/building
 * painter slices without changing the authored geometry. */
uint8_t SimBackgroundVoxelDepth_SliceCount(
    SimBackgroundVoxelDetail detail);

/* Slices are returned far-to-near. The first and last ranges extend to the
 * floating-point limits so projected objects just outside the ground quad are
 * not accidentally phased out. Ranges are [minimum, maximum). */
void SimBackgroundVoxelDepth_SliceRange(
    float visible_minimum, float visible_maximum,
    uint8_t slice_count, uint8_t slice,
    float *minimum, float *maximum);

bool SimBackgroundVoxelDepth_Contains(
    float depth, float minimum, float maximum);

#endif  /* SIM_BACKGROUND_VOXEL_DEPTH_H */
