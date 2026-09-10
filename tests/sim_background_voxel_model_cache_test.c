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
  const SimBackgroundVoxelModelView *first = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  const SimBackgroundVoxelModelView *second = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  CHECK(first && first == second && first->face_count > 0);
  SimBackgroundVoxelModelCacheStats stats =
      SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 1 && stats.hits == 1 && stats.evictions == 0);

  house.record_slot = 4;
  const SimBackgroundVoxelModelView *variant = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  CHECK(variant && variant != first);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 2 && stats.hits == 1);

  /* Town and civilization tier select different authored architecture and
   * must never alias an otherwise identical house model. */
  house.town = 3;
  house.development_level = 1;
  const SimBackgroundVoxelModelView *early_kasandora =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  house.development_level = 2;
  const SimBackgroundVoxelModelView *developed_kasandora =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  house.town = 2;
  const SimBackgroundVoxelModelView *developed_bloodpool =
      SimBackgroundVoxelModelCache_Get(
          &house, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Varied, NULL, NULL);
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
  const SimBackgroundVoxelModelView *short_bridge =
      SimBackgroundVoxelModelCache_Get(
          &bridge, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Basic, NULL, NULL);
  CHECK(short_bridge && short_bridge->max_x == 50.0f);
  bridge.bridge_bank_b_x = 14;
  const SimBackgroundVoxelModelView *long_bridge =
      SimBackgroundVoxelModelCache_Get(
          &bridge, kSimBackgroundVoxelDetail_High,
          kSimBackgroundVoxelStyle_Basic, NULL, NULL);
  CHECK(long_bridge && long_bridge->max_x == 82.0f);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 2 && stats.hits == 0);

  /* The shared visual state is explicit cache identity. Today construction
   * also changes flags, but keeping the state itself in the key prevents a
   * future phase-specific model from aliasing before its geometry is rebuilt. */
  SimBackgroundVoxelModelCache_Reset();
  SimBackgroundVoxelObject state_house = {
    .kind = kSimBackgroundVoxel_House,
    .town = 4,
    .development_level = 2,
    .cell_x = 12,
    .cell_y = 8,
    .record_slot = 9,
    .visual_state = kSimStructureVisualState_Finished,
  };
  const SimBackgroundVoxelModelView *finished_state =
      SimBackgroundVoxelModelCache_Get(
          &state_house, kSimBackgroundVoxelDetail_Balanced,
          kSimBackgroundVoxelStyle_Basic, NULL, NULL);
  state_house.visual_state = kSimStructureVisualState_Construction0;
  const SimBackgroundVoxelModelView *construction_state =
      SimBackgroundVoxelModelCache_Get(
          &state_house, kSimBackgroundVoxelDetail_Balanced,
          kSimBackgroundVoxelStyle_Basic, NULL, NULL);
  CHECK(finished_state && construction_state &&
        finished_state != construction_state);
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
        kSimBackgroundVoxelStyle_Basic, NULL, NULL) != NULL);
  }
  stats = SimBackgroundVoxelModelCache_Stats();
  uint32_t first_pass_misses = stats.misses;
  for (int i = 0; i < kDevelopedTownObjects; i++)
    CHECK(SimBackgroundVoxelModelCache_Get(
        &objects[i], kSimBackgroundVoxelDetail_Low,
        kSimBackgroundVoxelStyle_Basic, NULL, NULL) != NULL);
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
  const SimBackgroundVoxelModelView *lit_model = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &noon, &lit_first);
  CHECK(lit_model && lit_first && lit_model->face_count > 0);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.relights == 0);

  const SimBackgroundVoxelModelShading *lit_again = NULL;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &noon, &lit_again) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(lit_again == lit_first && stats.relights == 0);

  /* Moving the light must relight the same geometry, and actually change it.
   * The comparison has to be over the whole model: a top face's diffuse term
   * depends only on the light's elevation, so swinging the azimuth leaves it
   * untouched and any single face is a coin toss. */
  static uint8_t before[kSimBackgroundVoxelModelMaxFaces][4];
  memcpy(before, lit_first->brightness, (size_t)lit_model->face_count * 4);
  SimBackgroundVoxelModelShadingKey dusk = noon;
  dusk.light_azimuth_deg = 135;
  const SimBackgroundVoxelModelShading *relit = NULL;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &dusk, &relit) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(relit && stats.relights == 1);
  CHECK(memcmp(before, relit->brightness,
               (size_t)lit_model->face_count * 4) != 0);

  /* The biome selects snow surfaces, so it belongs in the key too. */
  SimBackgroundVoxelModelShadingKey snow = dusk;
  snow.biome = kSimBackgroundVoxelBiome_Snow;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &snow, &relit) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.relights == 2);

  /* Asking for geometry alone must neither relight nor hand back shading. */
  const SimBackgroundVoxelModelShading *none = lit_first;
  CHECK(SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, NULL, &none) == lit_model);
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(none == NULL && stats.relights == 2);

  SimBackgroundVoxelModelCache_Reset();
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.misses == 0 && stats.hits == 0 && stats.evictions == 0);
  CHECK(stats.relights == 0);

  /* Multi-town working sets preserve the same authored data when storage
   * grows, and retain thousands of identities without per-frame recompiles. */
  const SimBackgroundVoxelModelView *before_growth = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_Low,
      kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  const SimBackgroundVoxelModelView saved_model = *before_growth;
  SimBackgroundVoxelModelFace saved_faces[kSimBackgroundVoxelModelMaxFaces];
  memcpy(saved_faces, before_growth->faces,
      before_growth->face_count * sizeof(*saved_faces));
  CHECK(SimBackgroundVoxelModelCache_Reserve(4096));
  const SimBackgroundVoxelModelView *after_growth = SimBackgroundVoxelModelCache_Get(
      &house, kSimBackgroundVoxelDetail_Low,
      kSimBackgroundVoxelStyle_Varied, NULL, NULL);
  CHECK(memcmp(&saved_model, after_growth, sizeof(saved_model)) == 0);
  CHECK(memcmp(saved_faces, after_growth->faces,
      after_growth->face_count * sizeof(*saved_faces)) == 0);
  CHECK(SimBackgroundVoxelModelCache_Stats().hits == 1);
  enum { kGlobeObjects = 1600 };
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kGlobeObjects; i++) {
      SimBackgroundVoxelObject globe_object = {
        .town = (uint8_t)(i / 320 + 1),
        .kind = kSimBackgroundVoxel_House,
        .cell_x = (uint8_t)(i & 31),
        .cell_y = (uint8_t)((i >> 5) & 31),
        .record_slot = (uint8_t)i,
        .group = (uint16_t)i,
      };
      CHECK(SimBackgroundVoxelModelCache_Get(
          &globe_object, kSimBackgroundVoxelDetail_Low,
          kSimBackgroundVoxelStyle_Varied, NULL, NULL));
    }
  }
  stats = SimBackgroundVoxelModelCache_Stats();
  CHECK(stats.hits >= 1 + kGlobeObjects);
  CHECK(stats.evictions == 0);
  CHECK(stats.capacity == 4096);
  CHECK(stats.storage_bytes < 20 * 1024 * 1024);
  printf("compact cache: %u entries, %zu bytes with %d Low models\n",
      stats.capacity, stats.storage_bytes, kGlobeObjects);
  SimBackgroundVoxelModelCache_Reset();
  CHECK(SimBackgroundVoxelModelCache_Stats().hits == 0);

  if (failures) {
    fprintf(stderr, "%d sim background voxel cache checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel cache checks passed");
  return 0;
}
