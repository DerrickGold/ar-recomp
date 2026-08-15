#include "sim/sim_background_voxel_palette.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  SimBackgroundVoxelObject house = {
    .kind = kSimBackgroundVoxel_House,
    .cell_x = 3,
    .cell_y = 4,
    .record_slot = 2,
  };
  SimBackgroundVoxelPalette palette, repeated;
  SimBackgroundVoxelPalette_Build(&house, &palette);
  SimBackgroundVoxelPalette_Build(&house, &repeated);
  CHECK(memcmp(&palette, &repeated, sizeof(palette)) == 0);
  uint32_t shadow = SimBackgroundVoxelPalette_Ramp(
      &palette, kSimVoxelMaterial_Roof, 80);
  uint32_t mid = SimBackgroundVoxelPalette_Ramp(
      &palette, kSimVoxelMaterial_Roof, 150);
  uint32_t base = SimBackgroundVoxelPalette_Base(
      &palette, kSimVoxelMaterial_Roof);
  uint32_t highlight = SimBackgroundVoxelPalette_Ramp(
      &palette, kSimVoxelMaterial_Roof, 250);
  CHECK(shadow != mid && mid != base && base != highlight);
  CHECK((SimBackgroundVoxelPalette_Base(
      &palette, kSimVoxelMaterial_Contact) >> 24) < 0x80u);
  CHECK(SimBackgroundVoxelPalette_Ramp(
            &palette, kSimVoxelMaterial_Gold, 80) !=
        SimBackgroundVoxelPalette_Ramp(
            &palette, kSimVoxelMaterial_Gold, 250));

  SimBackgroundVoxelObject tree_a = {
    .kind = kSimBackgroundVoxel_Tree,
    .cell_x = 1,
    .cell_y = 1,
    .record_slot = 0xFF,
  };
  SimBackgroundVoxelObject tree_b = tree_a;
  tree_b.cell_y = 2;
  SimBackgroundVoxelPalette palette_a, palette_b;
  SimBackgroundVoxelPalette_Build(&tree_a, &palette_a);
  SimBackgroundVoxelPalette_Build(&tree_b, &palette_b);
  CHECK(SimBackgroundVoxelPalette_Base(
            &palette_a, kSimVoxelMaterial_Leaves) !=
        SimBackgroundVoxelPalette_Base(
            &palette_b, kSimVoxelMaterial_Leaves));

  if (failures) return 1;
  puts("sim background voxel palette checks passed");
  return 0;
}
