#ifndef SIM_BACKGROUND_MOUNTAIN_RELIEF_H
#define SIM_BACKGROUND_MOUNTAIN_RELIEF_H

#include <stdint.h>

#include "sim_background_voxel_quality.h"

enum { kSimBackgroundMountainReliefMaxStackLayers = 5 };

typedef struct SimBackgroundMountainRelief {
  uint8_t stack_layer_count;
  float stack_depth_pixels;
  float face_height_scale;
  float face_depth_scale;
} SimBackgroundMountainRelief;

/* Quality controls a bounded set of complete authentic-art copies. They move
 * in fixed map space and taper to a shared ridge; no camera-relative or
 * texture-stretched connector geometry is generated. */
void SimBackgroundMountainRelief_Resolve(
    SimBackgroundVoxelDetail detail, SimBackgroundMountainRelief *out);
float SimBackgroundMountainRelief_StackOffsetY(
    const SimBackgroundMountainRelief *relief, uint8_t layer,
    float rise, float maximum_rise);

#endif  /* SIM_BACKGROUND_MOUNTAIN_RELIEF_H */
