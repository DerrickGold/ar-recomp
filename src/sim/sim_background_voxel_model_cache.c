#include "sim_background_voxel_model_cache.h"

#include <stdbool.h>
#include <string.h>

typedef struct SimBackgroundVoxelModelCacheKey {
  uint16_t group;
  uint8_t kind, flags;
  uint8_t cell_x, cell_y;
  uint8_t tree_edges, record_slot;
  uint8_t detail, style;
} SimBackgroundVoxelModelCacheKey;

typedef struct SimBackgroundVoxelModelCacheEntry {
  bool valid;
  uint32_t last_use;
  SimBackgroundVoxelModelCacheKey key;
  SimBackgroundVoxelModel model;
} SimBackgroundVoxelModelCacheEntry;

static struct {
  SimBackgroundVoxelModelCacheEntry entries[
      kSimBackgroundVoxelModelCacheCapacity];
  SimBackgroundVoxelModelCacheStats stats;
} g_model_cache;

static SimBackgroundVoxelModelCacheKey MakeKey(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style) {
  return (SimBackgroundVoxelModelCacheKey){
    .group = object->group,
    .kind = object->kind,
    .flags = object->flags,
    .cell_x = object->cell_x,
    .cell_y = object->cell_y,
    .tree_edges = object->tree_edges,
    .record_slot = object->record_slot,
    .detail = (uint8_t)detail,
    .style = (uint8_t)style,
  };
}

static bool KeyEquals(const SimBackgroundVoxelModelCacheKey *left,
                      const SimBackgroundVoxelModelCacheKey *right) {
  return left->group == right->group && left->kind == right->kind &&
      left->flags == right->flags && left->cell_x == right->cell_x &&
      left->cell_y == right->cell_y && left->tree_edges == right->tree_edges &&
      left->record_slot == right->record_slot && left->detail == right->detail &&
      left->style == right->style;
}

const SimBackgroundVoxelModel *SimBackgroundVoxelModelCache_Get(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    uint32_t stamp) {
  if (!object) return NULL;
  SimBackgroundVoxelModelCacheKey key = MakeKey(object, detail, style);
  int free_entry = -1;
  int oldest_entry = -1;
  uint32_t oldest_age = 0;
  for (int entry = 0; entry < kSimBackgroundVoxelModelCacheCapacity; entry++) {
    SimBackgroundVoxelModelCacheEntry *candidate =
        &g_model_cache.entries[entry];
    if (candidate->valid && KeyEquals(&candidate->key, &key)) {
      candidate->last_use = stamp;
      g_model_cache.stats.hits++;
      return &candidate->model;
    }
    if (!candidate->valid) {
      if (free_entry < 0) free_entry = entry;
      continue;
    }
    uint32_t age = stamp - candidate->last_use;
    if (oldest_entry < 0 || age > oldest_age) {
      oldest_entry = entry;
      oldest_age = age;
    }
  }
  int replacement = free_entry >= 0 ? free_entry : oldest_entry;
  if (replacement < 0) replacement = 0;
  SimBackgroundVoxelModelCacheEntry *entry =
      &g_model_cache.entries[replacement];
  if (entry->valid) g_model_cache.stats.evictions++;
  entry->valid = true;
  entry->last_use = stamp;
  entry->key = key;
  SimBackgroundVoxelModel_BuildStyled(object, detail, style, &entry->model);
  g_model_cache.stats.misses++;
  return &entry->model;
}

SimBackgroundVoxelModelCacheStats SimBackgroundVoxelModelCache_Stats(void) {
  return g_model_cache.stats;
}

void SimBackgroundVoxelModelCache_Reset(void) {
  memset(&g_model_cache, 0, sizeof(g_model_cache));
}
