#include "sim_background_voxel_model_cache.h"

#include <stdbool.h>
#include <string.h>

#include "sim_background_voxel_lighting.h"

typedef struct SimBackgroundVoxelModelCacheKey {
  uint16_t group;
  uint8_t kind, flags;
  uint8_t town, development_level;
  uint8_t cell_x, cell_y;
  uint8_t tree_edges, record_slot;
  uint8_t source_cells_w, source_cells_h;
  uint8_t bridge_axis;
  uint8_t bridge_bank_a_x, bridge_bank_a_y;
  uint8_t bridge_bank_b_x, bridge_bank_b_y;
  uint8_t detail, style;
  /* Animated families compile one model per frame. Leaving this out of the
   * key froze a windmill on whichever blade position compiled first. */
  uint8_t animation_phase;
} SimBackgroundVoxelModelCacheKey;

typedef struct SimBackgroundVoxelModelCacheEntry {
  bool valid;
  bool shading_valid;
  uint32_t last_use;
  SimBackgroundVoxelModelCacheKey key;
  SimBackgroundVoxelModelShadingKey shading_key;
  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModelShading shading;
} SimBackgroundVoxelModelCacheEntry;

static struct {
  SimBackgroundVoxelModelCacheEntry entries[
      kSimBackgroundVoxelModelCacheSetCount]
      [kSimBackgroundVoxelModelCacheWays];
  SimBackgroundVoxelModelCacheStats stats;
} g_model_cache;

_Static_assert(
    kSimBackgroundVoxelModelCacheCapacity %
        kSimBackgroundVoxelModelCacheWays == 0,
    "voxel model cache capacity must contain complete sets");
_Static_assert(
    kSimBackgroundVoxelModelCacheSetCount > 0 &&
        (kSimBackgroundVoxelModelCacheSetCount &
         (kSimBackgroundVoxelModelCacheSetCount - 1)) == 0,
    "voxel model cache set count must be a power of two");

static SimBackgroundVoxelModelCacheKey MakeKey(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style) {
  return (SimBackgroundVoxelModelCacheKey){
    .group = object->group,
    .kind = object->kind,
    .flags = object->flags,
    .town = object->town,
    .development_level = object->development_level,
    .cell_x = object->cell_x,
    .cell_y = object->cell_y,
    .tree_edges = object->tree_edges,
    .record_slot = object->record_slot,
    .source_cells_w = object->source_cells_w,
    .source_cells_h = object->source_cells_h,
    .bridge_axis = object->bridge_axis,
    .bridge_bank_a_x = object->bridge_bank_a_x,
    .bridge_bank_a_y = object->bridge_bank_a_y,
    .bridge_bank_b_x = object->bridge_bank_b_x,
    .bridge_bank_b_y = object->bridge_bank_b_y,
    .detail = (uint8_t)detail,
    .style = (uint8_t)style,
    .animation_phase = object->animation_phase,
  };
}

static bool KeyEquals(const SimBackgroundVoxelModelCacheKey *left,
                      const SimBackgroundVoxelModelCacheKey *right) {
  return left->group == right->group && left->kind == right->kind &&
      left->flags == right->flags && left->cell_x == right->cell_x &&
      left->town == right->town &&
      left->development_level == right->development_level &&
      left->cell_y == right->cell_y && left->tree_edges == right->tree_edges &&
      left->record_slot == right->record_slot && left->detail == right->detail &&
      left->source_cells_w == right->source_cells_w &&
      left->source_cells_h == right->source_cells_h &&
      left->bridge_axis == right->bridge_axis &&
      left->bridge_bank_a_x == right->bridge_bank_a_x &&
      left->bridge_bank_a_y == right->bridge_bank_a_y &&
      left->bridge_bank_b_x == right->bridge_bank_b_x &&
      left->bridge_bank_b_y == right->bridge_bank_b_y &&
      left->style == right->style &&
      left->animation_phase == right->animation_phase;
}

static bool ShadingKeyEquals(const SimBackgroundVoxelModelShadingKey *left,
                             const SimBackgroundVoxelModelShadingKey *right) {
  return left->light_azimuth_deg == right->light_azimuth_deg &&
      left->light_elevation_deg == right->light_elevation_deg &&
      left->shading == right->shading && left->biome == right->biome;
}

static void ResolveShading(SimBackgroundVoxelModelCacheEntry *entry) {
  SimBackgroundVoxelLightDirection light;
  SimBackgroundVoxelLighting_ResolveDirection(
      entry->shading_key.light_azimuth_deg,
      entry->shading_key.light_elevation_deg, &light);
  SimBackgroundVoxelShading shading =
      (SimBackgroundVoxelShading)entry->shading_key.shading;
  SimBackgroundVoxelBiome biome =
      (SimBackgroundVoxelBiome)entry->shading_key.biome;
  SimBackgroundVoxelDetail detail =
      (SimBackgroundVoxelDetail)entry->key.detail;
  for (uint16_t face = 0; face < entry->model.face_count; face++) {
    const SimBackgroundVoxelModelFace *source = &entry->model.faces[face];
    entry->shading.material[face] =
        (uint8_t)SimBackgroundVoxelBiome_SurfaceMaterial(
            biome, detail, (SimBackgroundVoxelMaterial)source->material,
            source);
    uint8_t directional =
        SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
            source, shading, &light);
    SimBackgroundVoxelLighting_VertexBrightnesses(
        source, &entry->model, directional, shading,
        entry->shading.brightness[face]);
  }
  entry->shading_valid = true;
}

static uint32_t HashByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * 16777619u;
}

static uint32_t HashKey(const SimBackgroundVoxelModelCacheKey *key) {
  /* FNV-1a over explicit fields avoids hashing struct padding, whose bytes are
  * unspecified and can differ between compilers and architectures. */
  uint32_t hash = 2166136261u;
  hash = HashByte(hash, (uint8_t)key->group);
  hash = HashByte(hash, (uint8_t)(key->group >> 8));
  hash = HashByte(hash, key->kind);
  hash = HashByte(hash, key->flags);
  hash = HashByte(hash, key->town);
  hash = HashByte(hash, key->development_level);
  hash = HashByte(hash, key->cell_x);
  hash = HashByte(hash, key->cell_y);
  hash = HashByte(hash, key->tree_edges);
  hash = HashByte(hash, key->record_slot);
  hash = HashByte(hash, key->source_cells_w);
  hash = HashByte(hash, key->source_cells_h);
  hash = HashByte(hash, key->bridge_axis);
  hash = HashByte(hash, key->bridge_bank_a_x);
  hash = HashByte(hash, key->bridge_bank_a_y);
  hash = HashByte(hash, key->bridge_bank_b_x);
  hash = HashByte(hash, key->bridge_bank_b_y);
  hash = HashByte(hash, key->detail);
  hash = HashByte(hash, key->style);
  hash = HashByte(hash, key->animation_phase);
  /* The set count is a power of two, so avalanche the structured coordinates
   * before selecting its low bits. This keeps adjacent town cells and the new
   * regional identity bytes from clustering into a handful of four-way sets. */
  hash ^= hash >> 16;
  hash *= 0x7FEB352Du;
  hash ^= hash >> 15;
  hash *= 0x846CA68Bu;
  hash ^= hash >> 16;
  return hash;
}

/* Bring `entry`'s stored shading up to date with the requested lighting,
 * without touching its geometry. */
static const SimBackgroundVoxelModelShading *EntryShading(
    SimBackgroundVoxelModelCacheEntry *entry,
    const SimBackgroundVoxelModelShadingKey *key) {
  if (!key) return NULL;
  if (!entry->shading_valid || !ShadingKeyEquals(&entry->shading_key, key)) {
    if (entry->shading_valid) g_model_cache.stats.relights++;
    entry->shading_key = *key;
    ResolveShading(entry);
  }
  return &entry->shading;
}

const SimBackgroundVoxelModel *SimBackgroundVoxelModelCache_Get(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    uint32_t stamp,
    const SimBackgroundVoxelModelShadingKey *shading_key,
    const SimBackgroundVoxelModelShading **out_shading) {
  if (out_shading) *out_shading = NULL;
  if (!object) return NULL;
  SimBackgroundVoxelModelCacheKey key = MakeKey(object, detail, style);
  uint32_t set = HashKey(&key) &
      (kSimBackgroundVoxelModelCacheSetCount - 1);
  int free_entry = -1;
  int oldest_entry = -1;
  uint32_t oldest_age = 0;
  for (int entry = 0; entry < kSimBackgroundVoxelModelCacheWays; entry++) {
    SimBackgroundVoxelModelCacheEntry *candidate =
        &g_model_cache.entries[set][entry];
    if (candidate->valid && KeyEquals(&candidate->key, &key)) {
      candidate->last_use = stamp;
      g_model_cache.stats.hits++;
      if (out_shading) *out_shading = EntryShading(candidate, shading_key);
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
      &g_model_cache.entries[set][replacement];
  if (entry->valid) g_model_cache.stats.evictions++;
  entry->valid = true;
  entry->shading_valid = false;
  entry->last_use = stamp;
  entry->key = key;
  SimBackgroundVoxelModel_BuildStyled(object, detail, style, &entry->model);
  g_model_cache.stats.misses++;
  if (out_shading) *out_shading = EntryShading(entry, shading_key);
  return &entry->model;
}

SimBackgroundVoxelModelCacheStats SimBackgroundVoxelModelCache_Stats(void) {
  return g_model_cache.stats;
}

void SimBackgroundVoxelModelCache_Reset(void) {
  memset(&g_model_cache, 0, sizeof(g_model_cache));
}
