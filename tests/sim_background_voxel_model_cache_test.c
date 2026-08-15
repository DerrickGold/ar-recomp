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

  /* Town and civilization tier select different authored architecture and
   * must never alias an otherwise identical house model. */
  house.town = 3;
  house.development_level = 1;
  const SimBackgroundVoxelModel *early_kasandora =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, 4);
  house.development_level = 2;
  const SimBackgroundVoxelModel *developed_kasandora =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, 5);
  house.town = 2;
  const SimBackgroundVoxelModel *developed_bloodpool =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, 6);
  CHECK(early_kasandora && developed_kasandora && developed_bloodpool);
  CHECK(early_kasandora != developed_kasandora);
  CHECK(developed_kasandora != developed_bloodpool);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 5 && stats.hits == 1);

  /* A developed-town pass should retain hundreds of independently seeded
   * objects and hit them on the following frame instead of linearly scanning
   * and evicting the next entry before it can be reused. */
  SimBackgroundVoxelModelCache_Reset();
  enum { kDevelopedTownObjects = 320 };
  SimBackgroundVoxelObject objects[kDevelopedTownObjects];
  for (int i = 0; i < kDevelopedTownObjects; i++) {
    objects[i] = (SimBackgroundVoxelObject){
      .kind = i & 1 ? kSimBackgroundVoxel_House
                    : kSimBackgroundVoxel_Tree,
      .cell_x = (uint8_t)(i & 31),
      .cell_y = (uint8_t)((i >> 5) & 31),
      .group = (uint16_t)(i + 1),
      .record_slot = (uint8_t)i,
    };
    CHECK(SimBackgroundVoxelModelCache_Get(
        &objects[i], kSimBackgroundVoxelDetail_Low,
        kSimBackgroundVoxelStyle_Basic, 10) != NULL);
  }
  stats = SimBackgroundVoxelModelCache_Stats();
  uint32_t first_pass_misses = stats.misses;
  for (int i = 0; i < kDevelopedTownObjects; i++)
    CHECK(SimBackgroundVoxelModelCache_Get(
        &objects[i], kSimBackgroundVoxelDetail_Low,
        kSimBackgroundVoxelStyle_Basic, 11) != NULL);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(first_pass_misses == kDevelopedTownObjects);
  if (stats.hits < kDevelopedTownObjects * 9 / 10)
    fprintf(stderr, "cache retention: hits=%u misses=%u evictions=%u\n",
            stats.hits, stats.misses, stats.evictions);
  CHECK(stats.hits >= kDevelopedTownObjects * 9 / 10);

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
