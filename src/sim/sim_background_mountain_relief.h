#ifndef SIM_BACKGROUND_MOUNTAIN_RELIEF_H
#define SIM_BACKGROUND_MOUNTAIN_RELIEF_H

#include <stdint.h>

#include "sim_background_voxel_quality.h"

enum { kSimBackgroundMountainReliefMaxSideBands = 4 };

typedef struct SimBackgroundMountainRelief {
  uint8_t side_band_count;
  uint8_t side_alpha;
  float depth_pixels;
  float face_height_scale;
  float face_depth_scale;
  uint8_t side_brightness[kSimBackgroundMountainReliefMaxSideBands];
} SimBackgroundMountainRelief;

/* The authentic mountain art remains the single visible front surface.
 * Quality controls only the number of shaded bands on exposed range sides,
 * so LOD changes can never replace or reclassify a peak. */
void SimBackgroundMountainRelief_Resolve(
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelShading shading,
    SimBackgroundMountainRelief *out);

#endif  /* SIM_BACKGROUND_MOUNTAIN_RELIEF_H */
