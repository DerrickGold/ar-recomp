#include "sim/sim_background_voxel_lod.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  CHECK(SimBackgroundVoxelLod_Resolve(
      kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelLod_Fixed, 4.0f) ==
      kSimBackgroundVoxelDetail_Ultra);
  CHECK(SimBackgroundVoxelLod_Resolve(
      kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelLod_Adaptive, 8.0f) ==
      kSimBackgroundVoxelDetail_Low);
  CHECK(SimBackgroundVoxelLod_Resolve(
      kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelLod_Adaptive, 18.0f) ==
      kSimBackgroundVoxelDetail_Balanced);
  CHECK(SimBackgroundVoxelLod_Resolve(
      kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelLod_Adaptive, 32.0f) ==
      kSimBackgroundVoxelDetail_High);
  CHECK(SimBackgroundVoxelLod_Resolve(
      kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelLod_Adaptive, 54.0f) ==
      kSimBackgroundVoxelDetail_Ultra);
  CHECK(SimBackgroundVoxelLod_Resolve(
      kSimBackgroundVoxelDetail_Balanced,
      kSimBackgroundVoxelLod_Adaptive, 80.0f) ==
      kSimBackgroundVoxelDetail_Balanced);

  if (failures) return 1;
  puts("sim background voxel LOD checks passed");
  return 0;
}
