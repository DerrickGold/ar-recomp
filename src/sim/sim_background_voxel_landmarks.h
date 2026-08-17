#ifndef SIM_BACKGROUND_VOXEL_LANDMARKS_H
#define SIM_BACKGROUND_VOXEL_LANDMARKS_H

#include <stddef.h>
#include <stdint.h>

#include "sim_background_voxel_types.h"

/* Unique story landmarks are not structure records. Each one owns a 2x2 plot
 * in its town's cell map stamped with a reserved $E0-$EF expansion metatile,
 * which is both where the art actually is and how large it actually is. */
size_t SimBackgroundVoxelLandmarks_Classify(
    uint8_t town, const uint8_t *wram,
    SimBackgroundVoxelObject *objects, size_t capacity);

#endif  /* SIM_BACKGROUND_VOXEL_LANDMARKS_H */
