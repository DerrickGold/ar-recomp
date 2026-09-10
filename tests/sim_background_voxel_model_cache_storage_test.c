/* White-box lifetime/failure tests keep allocator controls and recency probes
 * out of the production cache contract. The authored compiler is unchanged. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool fail_allocation;
static size_t live_allocations;
static void *CacheMalloc(size_t bytes) {
  if (fail_allocation) { fail_allocation = false; return NULL; }
  void *p = malloc(bytes);
  if (p) live_allocations++;
  return p;
}
static void *CacheCalloc(size_t count, size_t bytes) {
  if (fail_allocation) { fail_allocation = false; return NULL; }
  void *p = calloc(count, bytes);
  if (p) live_allocations++;
  return p;
}
static void CacheFree(void *p) {
  if (p) { assert(live_allocations); live_allocations--; }
  free(p);
}
#define malloc CacheMalloc
#define calloc CacheCalloc
#define free CacheFree
#include "../src/sim/sim_background_voxel_model_cache.c"
#undef malloc
#undef calloc
#undef free

static bool Contains(const SimBackgroundVoxelObject *object) {
  const SimBackgroundVoxelModelCacheKey key = MakeKey(object,
      kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Basic);
  const uint32_t set = HashKey(&key) & (s_set_count - 1);
  for (int i = 0; i < kSimBackgroundVoxelModelCacheWays; i++)
    if (CacheSet(set)[i].valid && KeyEquals(&key, &CacheSet(set)[i].key)) return true;
  return false;
}

static void TestRecencyAndFailures(void) {
  SimBackgroundVoxelObject objects[kSimBackgroundVoxelModelCacheWays + 1];
  int count = 0;
  for (unsigned group = 0; group <= UINT16_MAX && count < kSimBackgroundVoxelModelCacheWays + 1; group++) {
    const SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_House,
        .group = (uint16_t)group, .town = 1, .cell_x = 4, .cell_y = 7,
        .source_cells_w = 1, .source_cells_h = 1};
    const SimBackgroundVoxelModelCacheKey key = MakeKey(&object,
        kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Basic);
    if (!(HashKey(&key) & (s_set_count - 1))) objects[count++] = object;
  }
  assert(count == kSimBackgroundVoxelModelCacheWays + 1);
  for (int wrap = 0; wrap < 2; wrap++) {
    SimBackgroundVoxelModelCache_Reset();
    if (wrap) g_model_cache.clock = UINT64_MAX - 8;
    for (int i = 0; i < kSimBackgroundVoxelModelCacheWays; i++)
      assert(SimBackgroundVoxelModelCache_Get(&objects[i],
          kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Basic, NULL, NULL));
    /* Interleaved consumers cannot supply independent clocks any more. */
    assert(SimBackgroundVoxelModelCache_Get(&objects[0],
        kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Basic, NULL, NULL));
    const size_t before = SimBackgroundVoxelModelCache_Stats().storage_bytes;
    const SimBackgroundVoxelModelShading *shading = (void *)1;
    fail_allocation = true;
    assert(!SimBackgroundVoxelModelCache_Get(&objects[count - 1],
        kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Basic, NULL, &shading));
    assert(!shading && Contains(&objects[0]) && Contains(&objects[1]));
    assert(SimBackgroundVoxelModelCache_Stats().storage_bytes == before);
    assert(SimBackgroundVoxelModelCache_Stats().evictions == 0);
    assert(SimBackgroundVoxelModelCache_Get(&objects[count - 1],
        kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Basic, NULL, NULL));
    assert(Contains(&objects[0]) && !Contains(&objects[1]));
    fail_allocation = true;
    assert(!SimBackgroundVoxelModelCache_Reserve(4096));
    assert(Contains(&objects[0]));
    assert(SimBackgroundVoxelModelCache_Reserve(4096));
    assert(SimBackgroundVoxelModelCache_Reserve(8192));
    assert(Contains(&objects[0]) && Contains(&objects[count - 1]));
    SimBackgroundVoxelModelCache_Reset();
    assert(live_allocations == 0);
  }
}

static void TestCompiledParity(void) {
  for (int kind = 0; kind < kSimBackgroundVoxelKindCount; kind++)
    for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
      SimBackgroundVoxelObject object = {.kind = (uint8_t)kind, .town = 2,
          .cell_x = 4, .cell_y = 7, .source_cells_w = 2, .source_cells_h = 2};
      SimBackgroundVoxelModel compiled;
      SimBackgroundVoxelModel_BuildStyled(&object, detail,
          kSimBackgroundVoxelStyle_Varied, &compiled);
      const SimBackgroundVoxelModelShadingKey light = {.light_azimuth_deg = 315,
          .light_elevation_deg = 40, .shading = kSimBackgroundVoxelShading_AmbientOcclusion,
          .biome = kSimBackgroundVoxelBiome_Wetland};
      const SimBackgroundVoxelModelShading *shading = NULL;
      const SimBackgroundVoxelModelView *view = SimBackgroundVoxelModelCache_Get(
          &object, detail, kSimBackgroundVoxelStyle_Varied, &light, &shading);
      assert(view && shading && view->face_count == compiled.face_count);
      assert(view->overflow == compiled.overflow && view->min_x == compiled.min_x &&
          view->min_y == compiled.min_y && view->min_z == compiled.min_z &&
          view->max_x == compiled.max_x && view->max_y == compiled.max_y && view->max_z == compiled.max_z);
      if (view->face_count) assert(!memcmp(view->faces, compiled.faces,
          view->face_count * sizeof(*view->faces)));
      for (int f = 0; f < view->face_count; f++) {
        assert(shading->material[f] == SimBackgroundVoxelBiome_SurfaceMaterial(
            light.biome, detail, compiled.faces[f].material, &compiled.faces[f]));
        uint8_t expected[4];
        const uint8_t directional = SimBackgroundVoxelLighting_FaceBrightness(
            &compiled.faces[f], light.shading, light.light_azimuth_deg, light.light_elevation_deg);
        SimBackgroundVoxelLighting_VertexBrightnesses(&compiled.faces[f], &compiled,
            directional, light.shading, expected);
        assert(!memcmp(expected, shading->brightness[f], sizeof(expected)));
      }
    }
  SimBackgroundVoxelModelCache_Reset();
  assert(live_allocations == 0);
}

int main(void) {
  TestRecencyAndFailures();
  TestCompiledParity();
  puts("compact cache recency, wrap, allocation recovery and compiled parity: PASS");
  return 0;
}
