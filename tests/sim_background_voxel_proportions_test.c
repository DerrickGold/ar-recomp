#include "sim/sim_background_voxel_proportions.h"
#include "sim/sim_background_voxel_models.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static float EffectiveHeight(SimBackgroundVoxelKind kind) {
  SimBackgroundVoxelObject object = {
    .kind = kind,
    .record_slot = 2,
  };
  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModel_Build(
      &object, kSimBackgroundVoxelDetail_High, &model);
  CHECK(!model.overflow);
  CHECK(model.face_count > 0);
  return model.max_z * SimBackgroundVoxelProportions_Get(kind)->height_scale;
}

int main(void) {
  const SimBackgroundVoxelProportions *house =
      SimBackgroundVoxelProportions_Get(kSimBackgroundVoxel_House);
  const SimBackgroundVoxelProportions *cathedral =
      SimBackgroundVoxelProportions_Get(kSimBackgroundVoxel_Cathedral);
  const SimBackgroundVoxelProportions *windmill =
      SimBackgroundVoxelProportions_Get(kSimBackgroundVoxel_Windmill);
  const SimBackgroundVoxelProportions *factory =
      SimBackgroundVoxelProportions_Get(kSimBackgroundVoxel_Factory);
  const SimBackgroundVoxelProportions *tree =
      SimBackgroundVoxelProportions_Get(kSimBackgroundVoxel_Tree);

  float house_height = EffectiveHeight(kSimBackgroundVoxel_House);
  float factory_height = EffectiveHeight(kSimBackgroundVoxel_Factory);
  float tree_height = EffectiveHeight(kSimBackgroundVoxel_Tree);
  float cathedral_height = EffectiveHeight(kSimBackgroundVoxel_Cathedral);
  float windmill_height = EffectiveHeight(kSimBackgroundVoxel_Windmill);
  CHECK(house->height_scale == 0.68f);
  CHECK(factory_height > house_height * 1.20f);
  CHECK(tree_height > house_height * 1.20f);
  CHECK(cathedral_height > factory_height);
  CHECK(windmill_height > cathedral_height);
  CHECK(house->footprint_scale == cathedral->footprint_scale);
  CHECK(house->footprint_scale == windmill->footprint_scale);
  CHECK(house->footprint_scale == factory->footprint_scale);
  CHECK(house->footprint_scale == tree->footprint_scale);

  if (failures) {
    fprintf(stderr, "%d sim background voxel proportion checks failed\n",
            failures);
    return 1;
  }
  puts("sim background voxel proportion checks passed");
  return 0;
}
