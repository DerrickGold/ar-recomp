#include "sim/sim_background_voxel_model_cache.h"

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
    .cell_x = 4,
    .cell_y = 7,
    .record_slot = 3,
  };
  SimBackgroundVoxelModelCache_Reset();
  const SimBackgroundVoxelModel *first = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 1, NULL, NULL);
  const SimBackgroundVoxelModel *second = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 2, NULL, NULL);
  CHECK(first && first == second && first->face_count > 0);
  SimBackgroundVoxelModelCacheStats stats =
      SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 1 && stats.hits == 1 && stats.evictions == 0);

  house.record_slot = 4;
  const SimBackgroundVoxelModel *variant = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 3, NULL, NULL);
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
          kSimBackgroundVoxelStyle_Varied, 4, NULL, NULL);
  house.development_level = 2;
  const SimBackgroundVoxelModel *developed_kasandora =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, 5, NULL, NULL);
  house.town = 2;
  const SimBackgroundVoxelModel *developed_bloodpool =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, 6, NULL, NULL);
  CHECK(early_kasandora && developed_kasandora && developed_bloodpool);
  CHECK(early_kasandora != developed_kasandora);
  CHECK(developed_kasandora != developed_bloodpool);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 5 && stats.hits == 1);

  /* A bridge's banks define its compiled span. The live marker may remain in
   * the same cell while a wider water run is exposed, so endpoint identity
   * belongs in the key rather than only in the renderer transform. */
  SimBackgroundVoxelModelCache_Reset();
  SimBackgroundVoxelObject bridge = {
    .kind = kSimBackgroundVoxel_Bridge,
    .cell_x = 10,
    .cell_y = 10,
    .source_cells_w = 1,
    .source_cells_h = 1,
    .bridge_axis = kSimBackgroundBridgeAxis_EastWest,
    .bridge_bank_a_x = 8,
    .bridge_bank_b_x = 12,
  };
  const SimBackgroundVoxelModel *short_bridge =
      SimBackgroundVoxelModelCache_Get(
          &bridge, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Basic, 7, NULL, NULL);
  CHECK(short_bridge && short_bridge->max_x == 50.0f);
  bridge.bridge_bank_b_x = 14;
  const SimBackgroundVoxelModel *long_bridge =
      SimBackgroundVoxelModelCache_Get(
          &bridge, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Basic, 8, NULL, NULL);
  CHECK(long_bridge && long_bridge->max_x == 82.0f);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 2 && stats.hits == 0);

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
        kSimBackgroundVoxelStyle_Basic, 10, NULL, NULL) != NULL);
  }
  stats = SimBackgroundVoxelModelCache_Stats();
  uint32_t first_pass_misses = stats.misses;
  for (int i = 0; i < kDevelopedTownObjects; i++)
    CHECK(SimBackgroundVoxelModelCache_Get(
        &objects[i], kSimBackgroundVoxelDetail_Low,
        kSimBackgroundVoxelStyle_Basic, 11, NULL, NULL) != NULL);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(first_pass_misses == kDevelopedTownObjects);
  if (stats.hits < kDevelopedTownObjects * 9 / 10)
    fprintf(stderr, "cache retention: hits=%u misses=%u evictions=%u\n",
            stats.hits, stats.misses, stats.evictions);
  CHECK(stats.hits >= kDevelopedTownObjects * 9 / 10);

  /* Shading is memoised with the geometry. None of its inputs move with the
   * camera, so a second frame at the same lighting must reuse the stored
   * result rather than relight 45,000 faces again. */
  SimBackgroundVoxelModelCache_Reset();
  const SimBackgroundVoxelModelShadingKey noon = {
    .light_azimuth_deg = 315,
    .light_elevation_deg = 40,
    .shading = kSimBackgroundVoxelShading_MaterialAware,
    .biome = kSimBackgroundVoxelBiome_Temperate,
  };
  const SimBackgroundVoxelModelShading *lit_first = NULL;
  const SimBackgroundVoxelModel *lit_model = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 20, &noon, &lit_first);
  CHECK(lit_model && lit_first && lit_model->face_count > 0);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.relights == 0);

  const SimBackgroundVoxelModelShading *lit_again = NULL;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 21, &noon, &lit_again) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(lit_again == lit_first && stats.relights == 0);

  /* Moving the light must relight the same geometry, and actually change it.
   * The comparison has to be over the whole model: a top face's diffuse term
   * depends only on the light's elevation, so swinging the azimuth leaves it
   * untouched and any single face is a coin toss. */
  static uint8_t before[kSimBackgroundVoxelModelMaxFaces][4];
  memcpy(before, lit_first->brightness, sizeof(before));
  SimBackgroundVoxelModelShadingKey dusk = noon;
  dusk.light_azimuth_deg = 135;
  const SimBackgroundVoxelModelShading *relit = NULL;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 22, &dusk, &relit) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(relit && stats.relights == 1);
  CHECK(memcmp(before, relit->brightness,
               (size_t)lit_model->face_count * 4) != 0);

  /* The biome selects snow surfaces, so it belongs in the key too. */
  SimBackgroundVoxelModelShadingKey snow = dusk;
  snow.biome = kSimBackgroundVoxelBiome_Snow;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 23, &snow, &relit) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.relights == 2);

  /* Asking for geometry alone must neither relight nor hand back shading. */
  const SimBackgroundVoxelModelShading *none = lit_first;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, 24, NULL, &none) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(none == NULL && stats.relights == 2);

  SimBackgroundVoxelModelCache_Reset();
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 0 && stats.hits == 0 && stats.evictions == 0);
  CHECK(stats.relights == 0);

  if (failures) {
    fprintf(stderr, "%d sim background voxel cache checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel cache checks passed");
  return 0;
}
