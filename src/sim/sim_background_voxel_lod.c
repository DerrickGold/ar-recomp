#include "sim_background_voxel_lod.h"

static SimBackgroundVoxelDetail ClampDetail(
    SimBackgroundVoxelDetail detail) {
  if (detail < kSimBackgroundVoxelDetail_Low)
    return kSimBackgroundVoxelDetail_Low;
  if (detail > kSimBackgroundVoxelDetail_Ultra)
    return kSimBackgroundVoxelDetail_Ultra;
  return detail;
}

SimBackgroundVoxelDetail SimBackgroundVoxelLod_Resolve(
    SimBackgroundVoxelDetail requested,
    SimBackgroundVoxelLod mode,
    float projected_height_pixels) {
  requested = ClampDetail(requested);
  if (mode != kSimBackgroundVoxelLod_Adaptive ||
      projected_height_pixels <= 0.0f)
    return requested;

  SimBackgroundVoxelDetail ceiling;
  if (projected_height_pixels < 12.0f)
    ceiling = kSimBackgroundVoxelDetail_Low;
  else if (projected_height_pixels < 24.0f)
    ceiling = kSimBackgroundVoxelDetail_Balanced;
  else if (projected_height_pixels < 42.0f)
    ceiling = kSimBackgroundVoxelDetail_High;
  else
    ceiling = kSimBackgroundVoxelDetail_Ultra;
  return requested < ceiling ? requested : ceiling;
}
