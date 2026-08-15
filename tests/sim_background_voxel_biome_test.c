#include "sim/sim_background_voxel_biome.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static SimBackgroundVoxelModelFace HorizontalFace(void) {
  return (SimBackgroundVoxelModelFace){
    .points = {
      {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    },
  };
}

static SimBackgroundVoxelModelFace VerticalFace(void) {
  return (SimBackgroundVoxelModelFace){
    .points = {
      {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
    },
  };
}

int main(void) {
  CHECK(SimBackgroundVoxelBiome_ForTown(1) ==
        kSimBackgroundVoxelBiome_Temperate);
  CHECK(SimBackgroundVoxelBiome_ForTown(3) ==
        kSimBackgroundVoxelBiome_Desert);
  CHECK(SimBackgroundVoxelBiome_ForTown(6) ==
        kSimBackgroundVoxelBiome_Snow);
  SimBackgroundVoxelModelFace horizontal = HorizontalFace();
  SimBackgroundVoxelModelFace vertical = VerticalFace();
  CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(
            kSimBackgroundVoxelBiome_Snow,
            kSimBackgroundVoxelDetail_High,
            kSimVoxelMaterial_Roof, &horizontal) ==
        kSimVoxelMaterial_Snow);
  CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(
            kSimBackgroundVoxelBiome_Snow,
            kSimBackgroundVoxelDetail_Low,
            kSimVoxelMaterial_Roof, &horizontal) ==
        kSimVoxelMaterial_Roof);
  CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(
            kSimBackgroundVoxelBiome_Snow,
            kSimBackgroundVoxelDetail_High,
            kSimVoxelMaterial_Roof, &vertical) ==
        kSimVoxelMaterial_Roof);
  CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(
            kSimBackgroundVoxelBiome_Desert,
            kSimBackgroundVoxelDetail_Ultra,
            kSimVoxelMaterial_Leaves, &horizontal) ==
        kSimVoxelMaterial_Leaves);

  if (failures) return 1;
  puts("sim background voxel biome checks passed");
  return 0;
}
