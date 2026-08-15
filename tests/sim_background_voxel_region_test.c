#include "sim/sim_background_voxel_region.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  static const SimBackgroundVoxelHouseStyle expected[6][3] = {
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Fillmore},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Bloodpool},
    {kSimBackgroundHouseStyle_Tent,
     kSimBackgroundHouseStyle_KasandoraEarlyStone,
     kSimBackgroundHouseStyle_KasandoraStone},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_Aitos},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Marahna,
     kSimBackgroundHouseStyle_Timber},
    {kSimBackgroundHouseStyle_Tent, kSimBackgroundHouseStyle_Timber,
     kSimBackgroundHouseStyle_KasandoraStone},
  };
  for (uint8_t town = 1; town <= 6; town++)
    for (uint8_t level = 0; level < 3; level++)
      CHECK(SimBackgroundVoxelRegion_HouseStyle(town, level) ==
            expected[town - 1][level]);

  CHECK(SimBackgroundVoxelRegion_HouseStyle(3, 1) ==
        kSimBackgroundHouseStyle_KasandoraEarlyStone);
  CHECK(SimBackgroundVoxelRegion_HouseStyle(3, 2) ==
        kSimBackgroundHouseStyle_KasandoraStone);
  CHECK(SimBackgroundVoxelRegion_HouseStyle(1, 2) !=
        SimBackgroundVoxelRegion_HouseStyle(3, 2));
  CHECK(SimBackgroundVoxelRegion_HouseStyle(5, 2) ==
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

  if (failures) return 1;
  puts("sim background voxel regional-style checks passed");
  return 0;
}
