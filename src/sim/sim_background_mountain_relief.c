#include "sim_background_mountain_relief.h"

void SimBackgroundMountainRelief_Resolve(
    SimBackgroundVoxelDetail detail, SimBackgroundMountainRelief *out) {
  if (!out) return;
  *out = (SimBackgroundMountainRelief){0};
  out->face_height_scale = 0.30f;
  out->face_depth_scale = 0.62f;
  switch (detail) {
    case kSimBackgroundVoxelDetail_Low:
      out->stack_layer_count = 1;
      break;
    case kSimBackgroundVoxelDetail_Balanced:
      out->stack_layer_count = 2;
      out->stack_depth_pixels = 1.5f;
      break;
    case kSimBackgroundVoxelDetail_High:
      out->stack_layer_count = 3;
      out->stack_depth_pixels = 2.5f;
      break;
    case kSimBackgroundVoxelDetail_Ultra:
      out->stack_layer_count = 5;
      out->stack_depth_pixels = 4.0f;
      break;
    case kSimBackgroundVoxelDetail_Count:
      break;
  }
}

float SimBackgroundMountainRelief_StackOffsetY(
    const SimBackgroundMountainRelief *relief, uint8_t layer,
    float rise, float maximum_rise) {
  if (!relief || relief->stack_layer_count <= 1 ||
      layer >= relief->stack_layer_count || maximum_rise <= 0.0f)
    return 0.0f;
  if (rise < 0.0f) rise = 0.0f;
  if (rise > maximum_rise) rise = maximum_rise;
  float layer_fraction = (float)layer /
      (float)(relief->stack_layer_count - 1);
  float height_fraction = rise / maximum_rise;
  float contact_and_ridge_taper =
      4.0f * height_fraction * (1.0f - height_fraction);
  /* North is the fixed rear direction in the canonical town map. Every copy
   * meets at both the ground contact and ridge. The displacement swells only
   * through the body, so yaw can reveal thickness without detaching the base
   * or pulling the peak apart. */
  return -relief->stack_depth_pixels * layer_fraction *
      contact_and_ridge_taper;
}
