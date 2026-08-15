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
    .town = 1,
    .development_level = 2,
    .cell_x = 3,
    .cell_y = 4,
    .record_slot = 1,
  };
  SimBackgroundVoxelPalette palette, repeated;
  SimBackgroundVoxelPalette_Build(
      &house, kSimBackgroundVoxelBiome_Temperate, &palette);
  SimBackgroundVoxelPalette_Build(
      &house, kSimBackgroundVoxelBiome_Temperate, &repeated);
  CHECK(memcmp(&palette, &repeated, sizeof(palette)) == 0);
  uint32_t shadow = SimBackgroundVoxelPalette_Ramp(
      &palette, kSimVoxelMaterial_Roof, 80);
  uint32_t mid = SimBackgroundVoxelPalette_Ramp(
      &palette, kSimVoxelMaterial_Roof, 150);
  uint32_t base = SimBackgroundVoxelPalette_Base(
      &palette, kSimVoxelMaterial_Roof);
  uint32_t highlight = SimBackgroundVoxelPalette_Ramp(
      &palette, kSimVoxelMaterial_Roof, 250);
  CHECK(shadow != base && mid != base && base != highlight);
  /* Fillmore houses use the authentic group-6 SIM background colours. */
  CHECK(base == 0xFF834100u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &palette, kSimVoxelMaterial_Wall) == 0xFFB48B4Au);
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
  SimBackgroundVoxelPalette_Build(
      &tree_a, kSimBackgroundVoxelBiome_Temperate, &palette_a);
  SimBackgroundVoxelPalette_Build(
      &tree_b, kSimBackgroundVoxelBiome_Temperate, &palette_b);
  CHECK(SimBackgroundVoxelPalette_Base(
            &palette_a, kSimVoxelMaterial_Leaves) !=
        SimBackgroundVoxelPalette_Base(
            &palette_b, kSimVoxelMaterial_Leaves));

  SimBackgroundVoxelPalette desert, northwall;
  SimBackgroundVoxelPalette_Build(
      &house, kSimBackgroundVoxelBiome_Desert, &desert);
  SimBackgroundVoxelPalette_Build(
      &house, kSimBackgroundVoxelBiome_Snow, &northwall);
  /* A biome change does not overwrite the house's authentic regional ramp. */
  CHECK(SimBackgroundVoxelPalette_Base(
            &desert, kSimVoxelMaterial_Wall) ==
        SimBackgroundVoxelPalette_Base(
            &palette, kSimVoxelMaterial_Wall));

  SimBackgroundVoxelObject bloodpool_house = house;
  bloodpool_house.town = 2;
  SimBackgroundVoxelObject kasandora_house = house;
  kasandora_house.town = 3;
  SimBackgroundVoxelObject aitos_house = house;
  aitos_house.town = 4;
  SimBackgroundVoxelObject marahna_house = house;
  marahna_house.town = 5;
  marahna_house.development_level = 1;
  SimBackgroundVoxelObject tent = kasandora_house;
  tent.development_level = 0;
  SimBackgroundVoxelPalette bloodpool, kasandora, aitos, marahna, canvas;
  SimBackgroundVoxelPalette_Build(
      &bloodpool_house, kSimBackgroundVoxelBiome_Wetland, &bloodpool);
  SimBackgroundVoxelPalette_Build(
      &kasandora_house, kSimBackgroundVoxelBiome_Desert, &kasandora);
  SimBackgroundVoxelPalette_Build(
      &aitos_house, kSimBackgroundVoxelBiome_Volcanic, &aitos);
  SimBackgroundVoxelPalette_Build(
      &marahna_house, kSimBackgroundVoxelBiome_Tropical, &marahna);
  SimBackgroundVoxelPalette_Build(
      &tent, kSimBackgroundVoxelBiome_Desert, &canvas);
  CHECK(SimBackgroundVoxelPalette_Base(
            &bloodpool, kSimVoxelMaterial_Roof) == 0xFF4A205Au);
  CHECK(SimBackgroundVoxelPalette_Base(
            &kasandora, kSimVoxelMaterial_Wall) == 0xFFA4A494u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &aitos, kSimVoxelMaterial_Roof) == 0xFFB45A10u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &marahna, kSimVoxelMaterial_Roof) == 0xFF836A31u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &canvas, kSimVoxelMaterial_Roof) == 0xFFA47B39u);
  SimBackgroundVoxelObject northwall_tree = tree_a;
  SimBackgroundVoxelPalette northwall_foliage;
  SimBackgroundVoxelPalette_Build(
      &northwall_tree, kSimBackgroundVoxelBiome_Snow,
      &northwall_foliage);
  CHECK(SimBackgroundVoxelPalette_Base(
            &northwall_foliage, kSimVoxelMaterial_Leaves) ==
        0xFF414A41u);
  CHECK((SimBackgroundVoxelPalette_Base(
      &northwall, kSimVoxelMaterial_Snow) >> 24) == 0xFFu);

  if (failures) return 1;
  puts("sim background voxel palette checks passed");
  return 0;
}
