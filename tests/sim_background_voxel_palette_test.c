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
  CHECK(SimBackgroundVoxelPalette_Base(
            &palette, kSimVoxelMaterial_Foundation) == 0xFF687462u);
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
    .record_slot = kSimBackgroundVoxelNoRecordSlot,
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
  CHECK(SimBackgroundVoxelPalette_Base(
            &desert, kSimVoxelMaterial_Foundation) == 0xFF7E6C4Eu);
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
  SimBackgroundVoxelObject yurt = kasandora_house;
  yurt.development_level = 0;
  SimBackgroundVoxelObject white_tent = kasandora_house;
  white_tent.development_level = 1;
  SimBackgroundVoxelObject marahna_logs = marahna_house;
  marahna_logs.development_level = 2;
  SimBackgroundVoxelPalette bloodpool, kasandora, aitos, marahna;
  SimBackgroundVoxelPalette yurt_palette, white_canvas, logs;
  SimBackgroundVoxelPalette_Build(
      &bloodpool_house, kSimBackgroundVoxelBiome_Wetland, &bloodpool);
  SimBackgroundVoxelPalette_Build(
      &kasandora_house, kSimBackgroundVoxelBiome_Desert, &kasandora);
  SimBackgroundVoxelPalette_Build(
      &aitos_house, kSimBackgroundVoxelBiome_Volcanic, &aitos);
  SimBackgroundVoxelPalette_Build(
      &marahna_house, kSimBackgroundVoxelBiome_Tropical, &marahna);
  SimBackgroundVoxelPalette_Build(
      &yurt, kSimBackgroundVoxelBiome_Desert, &yurt_palette);
  SimBackgroundVoxelPalette_Build(
      &white_tent, kSimBackgroundVoxelBiome_Desert, &white_canvas);
  SimBackgroundVoxelPalette_Build(
      &marahna_logs, kSimBackgroundVoxelBiome_Tropical, &logs);
  CHECK(SimBackgroundVoxelPalette_Base(
            &bloodpool, kSimVoxelMaterial_Roof) == 0xFF4A205Au);
  CHECK(SimBackgroundVoxelPalette_Base(
            &kasandora, kSimVoxelMaterial_Wall) == 0xFFCDAC73u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &aitos, kSimVoxelMaterial_Roof) == 0xFFB45A10u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &marahna, kSimVoxelMaterial_Roof) == 0xFF836A31u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &yurt_palette, kSimVoxelMaterial_Roof) == 0xFF8B4A10u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &white_canvas, kSimVoxelMaterial_Roof) == 0xFFCDCDBDu);
  CHECK(SimBackgroundVoxelPalette_Base(
            &logs, kSimVoxelMaterial_Wall) == 0xFF834A10u);
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

  SimBackgroundVoxelObject story_tree = {
    .kind = kSimBackgroundVoxel_StoryTree,
    .town = 6,
  };
  SimBackgroundVoxelObject castle = {
    .kind = kSimBackgroundVoxel_BloodpoolCastle,
    .town = 2,
  };
  SimBackgroundVoxelObject temple = {
    .kind = kSimBackgroundVoxel_MarahnaTemple,
    .town = 5,
  };
  SimBackgroundVoxelPalette story_palette, castle_palette, temple_palette;
  SimBackgroundVoxelPalette_Build(
      &story_tree, kSimBackgroundVoxelBiome_Snow, &story_palette);
  SimBackgroundVoxelPalette_Build(
      &castle, kSimBackgroundVoxelBiome_Wetland, &castle_palette);
  SimBackgroundVoxelPalette_Build(
      &temple, kSimBackgroundVoxelBiome_Tropical, &temple_palette);
  /* Landmark ramps are sampled from the landmark's own art. The story tree's
   * snow must survive the snow-biome foliage override that follows it, and the
   * castle is pale stone rather than the town's purple house roofs. */
  CHECK(SimBackgroundVoxelPalette_Base(
            &story_palette, kSimVoxelMaterial_Snow) == 0xFFD5DEE6u);
  /* The canopy's three bands must stay apart after the face-brightness ramp,
   * or the whole crown flattens into one pale blob. */
  CHECK(SimBackgroundVoxelPalette_Base(
            &story_palette, kSimVoxelMaterial_LeavesLight) == 0xFF8BACC5u);
  CHECK(SimBackgroundVoxelPalette_Ramp(
            &story_palette, kSimVoxelMaterial_Snow, 255) !=
        SimBackgroundVoxelPalette_Ramp(
            &story_palette, kSimVoxelMaterial_LeavesLight, 255));
  CHECK(SimBackgroundVoxelPalette_Ramp(
            &story_palette, kSimVoxelMaterial_LeavesLight, 232) !=
        SimBackgroundVoxelPalette_Ramp(
            &story_palette, kSimVoxelMaterial_LeavesDark, 232));
  CHECK(SimBackgroundVoxelPalette_Base(
            &castle_palette, kSimVoxelMaterial_Wall) == 0xFF6A6A62u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &castle_palette, kSimVoxelMaterial_Roof) == 0xFFA49452u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &temple_palette, kSimVoxelMaterial_Paving) == 0xFF83945Au);

  SimBackgroundVoxelObject pyramid = {
    .kind = kSimBackgroundVoxel_Pyramid,
    .town = 3,
  };
  SimBackgroundVoxelObject shrub = {
    .kind = kSimBackgroundVoxel_Shrub,
    .town = 1,
    .cell_x = 1,
    .cell_y = 1,
    .record_slot = kSimBackgroundVoxelNoRecordSlot,
  };
  SimBackgroundVoxelPalette pyramid_palette, shrub_palette;
  SimBackgroundVoxelPalette_Build(
      &pyramid, kSimBackgroundVoxelBiome_Desert, &pyramid_palette);
  SimBackgroundVoxelPalette_Build(
      &shrub, kSimBackgroundVoxelBiome_Temperate, &shrub_palette);
  CHECK(SimBackgroundVoxelPalette_Base(
            &pyramid_palette, kSimVoxelMaterial_WallLight) == 0xFFBDA462u);
  /* Three permanent/clearable foliage families share one CGRAM ramp and are
   * told apart by which part of it they occupy: the bush is brightest, the
   * broad canopy sits in the middle and the evergreen is darkest. Any two of
   * them landing on the same base colour would undo the classification. */
  SimBackgroundVoxelObject broad = shrub;
  broad.kind = kSimBackgroundVoxel_BroadTree;
  broad.town = 5;
  SimBackgroundVoxelPalette broad_palette;
  SimBackgroundVoxelPalette_Build(
      &broad, kSimBackgroundVoxelBiome_Tropical, &broad_palette);
  uint32_t shrub_leaves = SimBackgroundVoxelPalette_Base(
      &shrub_palette, kSimVoxelMaterial_Leaves);
  uint32_t broad_leaves = SimBackgroundVoxelPalette_Base(
      &broad_palette, kSimVoxelMaterial_Leaves);
  uint32_t fir_leaves = SimBackgroundVoxelPalette_Base(
      &palette_a, kSimVoxelMaterial_Leaves);
  CHECK(shrub_leaves != broad_leaves);
  CHECK(broad_leaves != fir_leaves);
  CHECK(shrub_leaves != fir_leaves);

  SimBackgroundVoxelObject bridge = {
    .kind = kSimBackgroundVoxel_Bridge,
    .town = 1,
  };
  SimBackgroundVoxelPalette bridge_palette;
  SimBackgroundVoxelPalette_Build(
      &bridge, kSimBackgroundVoxelBiome_Temperate, &bridge_palette);
  CHECK(SimBackgroundVoxelPalette_Base(
            &bridge_palette, kSimVoxelMaterial_Wall) == 0xFF687462u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &bridge_palette, kSimVoxelMaterial_Paving) == 0xFF707B68u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &bridge_palette, kSimVoxelMaterial_Trim) == 0xFFB8B499u);
  CHECK(SimBackgroundVoxelPalette_Base(
            &bridge_palette, kSimVoxelMaterial_Dark) == 0xFF262D27u);

  if (failures) return 1;
  puts("sim background voxel palette checks passed");
  return 0;
}
