#include "sim/sim_background_voxel_model_cache.h"

#include <stdio.h>

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
    .cell_x = 4,
    .cell_y = 7,
    .record_slot = 3,
  };
  SimBackgroundVoxelModelCache_Reset();
  const SimBackgroundVoxelModel *first = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 1);
  const SimBackgroundVoxelModel *second = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 2);
  CHECK(first && first == second && first->face_count > 0);
  SimBackgroundVoxelModelCacheStats stats =
      SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 1 && stats.hits == 1 && stats.evictions == 0);

  house.record_slot = 4;
  const SimBackgroundVoxelModel *variant = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 3);
  CHECK(variant && variant != first);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 2 && stats.hits == 1);

  SimBackgroundVoxelModelCache_Reset();
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 0 && stats.hits == 0 && stats.evictions == 0);

  if (failures) {
    fprintf(stderr, "%d sim background voxel cache checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel cache checks passed");
  return 0;
}
