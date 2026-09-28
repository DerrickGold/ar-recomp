#include "sim/voxels/sim_background_voxel_biome.h"

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
        kSimVoxelMaterial_Snow);
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

  /* An anisotropic mill roof can be much steeper than the foliage cutoff.
   * Its snow coverage must survive every LOD without whitening its gable. */
  SimBackgroundVoxelModelFace steep = {.points = {{0, 0, 0}, {1, 0, 0},
      {1, 1, 3}, {0, 1, 3}}};
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
        detail, kSimVoxelMaterial_Roof, &steep) == kSimVoxelMaterial_Snow);
    CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
        detail, kSimVoxelMaterial_RoofLight, &steep) == kSimVoxelMaterial_Snow);
    CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
        detail, kSimVoxelMaterial_WallLight, &steep) == kSimVoxelMaterial_WallLight);
    CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
        detail, kSimVoxelMaterial_Roof, &vertical) == kSimVoxelMaterial_Roof);
    CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
        detail, kSimVoxelMaterial_Leaves, &steep) == kSimVoxelMaterial_Leaves);
  }

  horizontal.outward_winding = true;
  /* Authored dark pockets stay visible; a downward foliage face receives no snow. */
  CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
      kSimBackgroundVoxelDetail_Ultra, kSimVoxelMaterial_LeavesDark, &horizontal) ==
      kSimVoxelMaterial_LeavesDark);
  SimBackgroundVoxelModelPoint swap = horizontal.points[1];
  horizontal.points[1] = horizontal.points[3]; horizontal.points[3] = swap;
  CHECK(SimBackgroundVoxelBiome_SurfaceMaterial(kSimBackgroundVoxelBiome_Snow,
      kSimBackgroundVoxelDetail_Ultra, kSimVoxelMaterial_Leaves, &horizontal) ==
      kSimVoxelMaterial_Leaves);

  if (failures) return 1;
  puts("sim background voxel biome checks passed");
  return 0;
}
