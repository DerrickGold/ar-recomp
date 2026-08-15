#include "sim_background_voxel_depth.h"

#include <float.h>

uint8_t SimBackgroundVoxelDepth_SliceCount(
    SimBackgroundVoxelDetail detail) {
  switch (detail) {
    case kSimBackgroundVoxelDetail_Low: return 1;
    case kSimBackgroundVoxelDetail_Balanced: return 4;
    case kSimBackgroundVoxelDetail_High: return 8;
    case kSimBackgroundVoxelDetail_Ultra: return 12;
    case kSimBackgroundVoxelDetail_Count: break;
  }
  return 1;
}

void SimBackgroundVoxelDepth_SliceRange(
    float visible_minimum, float visible_maximum,
    uint8_t slice_count, uint8_t slice,
    float *minimum, float *maximum) {
  if (!minimum || !maximum) return;
  if (!slice_count) slice_count = 1;
  if (slice >= slice_count) slice = slice_count - 1;
  if (visible_maximum < visible_minimum) {
    float swap = visible_minimum;
    visible_minimum = visible_maximum;
    visible_maximum = swap;
  }
  float span = visible_maximum - visible_minimum;
  *maximum = slice == 0
      ? FLT_MAX
      : visible_maximum - span * slice / slice_count;
  *minimum = slice + 1 == slice_count
      ? -FLT_MAX
      : visible_maximum - span * (slice + 1) / slice_count;
}

bool SimBackgroundVoxelDepth_Contains(
    float depth, float minimum, float maximum) {
  return depth >= minimum && depth < maximum;
}
