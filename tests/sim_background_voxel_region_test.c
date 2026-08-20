#include "sim/sim_background_voxel_region.h"
#include "sim/sim_background_bridge.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  static const SimBackgroundVoxelHouseStyle
      expected[kSimBackgroundTownCount]
              [kSimBackgroundDevelopmentLevelCount] = {
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Fillmore},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Bloodpool},
    {kSimBackgroundHouseStyle_Yurt,
     kSimBackgroundHouseStyle_WhiteTent,
     kSimBackgroundHouseStyle_Adobe},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Aitos},
    {kSimBackgroundHouseStyle_Yurt,
     kSimBackgroundHouseStyle_MarahnaStilt,
     kSimBackgroundHouseStyle_MarahnaLogCabin},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Stone},
  };
  for (uint8_t town = 1; town <= kSimBackgroundTownCount; town++)
    for (uint8_t level = 0;
         level < kSimBackgroundDevelopmentLevelCount; level++)
      CHECK(SimBackgroundVoxelRegion_HouseStyle(town, level) ==
            expected[town - 1][level]);

  CHECK(SimBackgroundVoxelRegion_HouseStyle(3, 1) ==
        kSimBackgroundHouseStyle_WhiteTent);
  CHECK(SimBackgroundVoxelRegion_HouseStyle(3, 2) ==
        kSimBackgroundHouseStyle_Adobe);
  CHECK(SimBackgroundVoxelRegion_HouseStyle(1, 2) !=
        SimBackgroundVoxelRegion_HouseStyle(3, 2));
  CHECK(SimBackgroundVoxelRegion_HouseStyle(3, 0) ==
        SimBackgroundVoxelRegion_HouseStyle(5, 0));
  CHECK(SimBackgroundVoxelRegion_HouseStyle(5, 2) !=
        SimBackgroundVoxelRegion_HouseStyle(1, 1));

  SimBackgroundVoxelObject tent = {
    .kind = kSimBackgroundVoxel_House,
    .town = 3,
    .development_level = 0,
  };
  SimBackgroundVoxelObject stone = tent;
  stone.development_level = 2;
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&tent) <
        SimBackgroundVoxelRegion_AuthoredHeight(&stone));

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
  /* Each landmark occupies a 2x2 plot, so none of them may stand taller than
   * its own 32-pixel base. */
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&story_tree) == 30.0f);
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&castle) == 32.0f);
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&temple) == 24.0f);
  SimBackgroundVoxelObject bridge = {
    .kind = kSimBackgroundVoxel_Bridge,
  };
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&bridge) ==
        SimBackgroundBridge_AuthoredHeight());

  SimBackgroundVoxelObject pyramid = {
    .kind = kSimBackgroundVoxel_Pyramid,
    .town = 3,
  };
  SimBackgroundVoxelObject shrub = {
    .kind = kSimBackgroundVoxel_Shrub,
    .town = 1,
  };
  SimBackgroundVoxelObject forest = {
    .kind = kSimBackgroundVoxel_Tree,
    .town = 1,
  };
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&pyramid) == 28.0f);
  /* The clearable bush must stay visibly lower than the permanent forest. */
  CHECK(SimBackgroundVoxelRegion_AuthoredHeight(&shrub) <
        SimBackgroundVoxelRegion_AuthoredHeight(&forest));

  if (failures) return 1;
  puts("sim background voxel regional-style checks passed");
  return 0;
}
