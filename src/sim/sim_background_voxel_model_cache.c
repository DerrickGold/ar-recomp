#include "sim_background_voxel_model_cache.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "deterministic_hash.h"
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
  uint8_t animation_phase, visual_state;
} SimBackgroundVoxelModelCacheKey;

typedef struct SimBackgroundVoxelModelCacheEntry {
  bool valid;
  bool shading_valid;
  uint64_t last_use;
  SimBackgroundVoxelModelCacheKey key;
  SimBackgroundVoxelModelShadingKey shading_key;
  SimBackgroundVoxelModelView model;
  SimBackgroundVoxelModelShading shading;
  void *storage;
  size_t storage_bytes;
} SimBackgroundVoxelModelCacheEntry;

static struct {
  SimBackgroundVoxelModelCacheEntry entries[
      kSimBackgroundVoxelModelCacheSetCount]
      [kSimBackgroundVoxelModelCacheWays];
  SimBackgroundVoxelModelCacheStats stats;
  uint64_t clock;
} g_model_cache;

static SimBackgroundVoxelModelCacheEntry *s_expanded_entries;
static uint32_t s_set_count = kSimBackgroundVoxelModelCacheSetCount;

static SimBackgroundVoxelModelCacheEntry *CacheSet(uint32_t set) {
  return s_expanded_entries
      ? s_expanded_entries + set * kSimBackgroundVoxelModelCacheWays
      : g_model_cache.entries[set];
}

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
    .visual_state = object->visual_state,
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
      left->animation_phase == right->animation_phase &&
      left->visual_state == right->visual_state;
}

static bool ShadingKeyEquals(const SimBackgroundVoxelModelShadingKey *left,
                             const SimBackgroundVoxelModelShadingKey *right) {
  return left->light_azimuth_deg == right->light_azimuth_deg &&
      left->light_elevation_deg == right->light_elevation_deg &&
      left->shading == right->shading && left->biome == right->biome;
}

static void ResolveShading(SimBackgroundVoxelModelCacheEntry *entry) {
  if (!entry->model.face_count) {
    entry->shading = (SimBackgroundVoxelModelShading){0};
    entry->shading_valid = true;
    return;
  }
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
  uint8_t *material = (uint8_t *)(entry->model.faces + entry->model.face_count);
  uint8_t (*brightness)[4] = (uint8_t (*)[4])(material + entry->model.face_count);
  for (uint16_t face = 0; face < entry->model.face_count; face++) {
    const SimBackgroundVoxelModelFace *source = &entry->model.faces[face];
    material[face] =
        (uint8_t)SimBackgroundVoxelBiome_SurfaceMaterial(
            biome, detail, (SimBackgroundVoxelMaterial)source->material,
            source);
    uint8_t directional =
        SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
            source, shading, &light);
    SimBackgroundVoxelLighting_VertexBrightnessesInRange(
        source, entry->model.min_z, entry->model.max_z, directional, shading,
        brightness[face]);
  }
  entry->shading = (SimBackgroundVoxelModelShading){material,
      (const uint8_t (*)[4])brightness};
  entry->shading_valid = true;
}

static uint32_t HashKey(const SimBackgroundVoxelModelCacheKey *key) {
  /* FNV-1a over explicit fields avoids hashing struct padding, whose bytes are
  * unspecified and can differ between compilers and architectures. */
  uint32_t hash = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  hash = DeterministicHash_Fnv1a32Byte(hash, (uint8_t)key->group);
  hash = DeterministicHash_Fnv1a32Byte(
      hash, (uint8_t)(key->group >> 8));
  hash = DeterministicHash_Fnv1a32Byte(hash, key->kind);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->flags);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->town);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->development_level);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->cell_x);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->cell_y);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->tree_edges);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->record_slot);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->source_cells_w);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->source_cells_h);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_axis);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_a_x);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_a_y);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_b_x);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_b_y);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->detail);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->style);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->animation_phase);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->visual_state);
  /* The set count is a power of two, so avalanche the structured coordinates
   * before selecting its low bits. This keeps adjacent town cells and the new
   * regional identity bytes from clustering into a handful of four-way sets. */
  return DeterministicHash_Mix32(hash);
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

const SimBackgroundVoxelModelView *SimBackgroundVoxelModelCache_Get(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    const SimBackgroundVoxelModelShadingKey *shading_key,
    const SimBackgroundVoxelModelShading **out_shading) {
  if (out_shading) *out_shading = NULL;
  if (!object) return NULL;
  /* Unsigned age remains correct across wrap; 64 bits also keep entries
   * unused for an entire human-scale session within the comparison window. */
  const uint64_t stamp = ++g_model_cache.clock;
  SimBackgroundVoxelModelCacheKey key = MakeKey(object, detail, style);
  uint32_t set = HashKey(&key) &
      (s_set_count - 1);
  int free_entry = -1;
  int oldest_entry = -1;
  uint64_t oldest_age = 0;
  for (int entry = 0; entry < kSimBackgroundVoxelModelCacheWays; entry++) {
    SimBackgroundVoxelModelCacheEntry *candidate =
        &CacheSet(set)[entry];
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
    uint64_t age = stamp - candidate->last_use;
    if (oldest_entry < 0 || age > oldest_age) {
      oldest_entry = entry;
      oldest_age = age;
    }
  }
  int replacement = free_entry >= 0 ? free_entry : oldest_entry;
  if (replacement < 0) replacement = 0;
  SimBackgroundVoxelModelCacheEntry *entry =
      &CacheSet(set)[replacement];
  SimBackgroundVoxelModel compiled;
  SimBackgroundVoxelModel_BuildStyled(object, detail, style, &compiled);
  const size_t bytes = compiled.face_count *
      (sizeof(SimBackgroundVoxelModelFace) + 5 * sizeof(uint8_t));
  /* One correctly aligned allocation for actual faces and byte-valued
   * shading. Authoring boxes and unused Ultra slots never enter the cache.
   * Allocate before releasing the victim so a failed miss is transactional. */
  void *storage = bytes ? malloc(bytes) : NULL;
  if (bytes && !storage) {
    g_model_cache.stats.allocation_failures++;
    return NULL;
  }
  if (bytes) memcpy(storage, compiled.faces,
      compiled.face_count * sizeof(compiled.faces[0]));
  g_model_cache.stats.storage_bytes -= entry->storage_bytes;
  free(entry->storage);
  entry->storage = storage;
  entry->storage_bytes = bytes;
  g_model_cache.stats.storage_bytes += bytes;
  if (entry->valid) g_model_cache.stats.evictions++;
  entry->valid = true;
  entry->shading_valid = false;
  entry->last_use = stamp;
  entry->key = key;
  entry->model = (SimBackgroundVoxelModelView){
    .face_count = compiled.face_count, .overflow = compiled.overflow,
    .min_x = compiled.min_x, .min_y = compiled.min_y, .min_z = compiled.min_z,
    .max_x = compiled.max_x, .max_y = compiled.max_y, .max_z = compiled.max_z,
    .faces = storage,
  };
  g_model_cache.stats.misses++;
  if (out_shading) *out_shading = EntryShading(entry, shading_key);
  return &entry->model;
}

SimBackgroundVoxelModelCacheStats SimBackgroundVoxelModelCache_Stats(void) {
  SimBackgroundVoxelModelCacheStats stats = g_model_cache.stats;
  stats.capacity = s_set_count * kSimBackgroundVoxelModelCacheWays;
  stats.storage_bytes += sizeof(g_model_cache) +
      (s_expanded_entries ? stats.capacity * sizeof(*s_expanded_entries) : 0);
  return stats;
}

void SimBackgroundVoxelModelCache_Reset(void) {
  for (uint32_t set = 0; set < s_set_count; set++)
    for (int way = 0; way < kSimBackgroundVoxelModelCacheWays; way++)
      free(CacheSet(set)[way].storage);
  free(s_expanded_entries);
  s_expanded_entries = NULL;
  s_set_count = kSimBackgroundVoxelModelCacheSetCount;
  memset(&g_model_cache, 0, sizeof(g_model_cache));
}

bool SimBackgroundVoxelModelCache_Reserve(uint32_t minimum_entries) {
  enum { kMaximumEntries = 8192 };
  if (minimum_entries > kMaximumEntries) minimum_entries = kMaximumEntries;
  uint32_t new_sets = s_set_count;
  while (new_sets * kSimBackgroundVoxelModelCacheWays < minimum_entries)
    new_sets *= 2;
  if (new_sets == s_set_count) return true;
  SimBackgroundVoxelModelCacheEntry *expanded = calloc(
      (size_t)new_sets * kSimBackgroundVoxelModelCacheWays, sizeof(*expanded));
  if (!expanded) {
    g_model_cache.stats.allocation_failures++;
    return false;
  }
  /* Splitting a power-of-two hash set cannot create new collisions: each
   * destination receives a subset of a previous set. */
  for (uint32_t set = 0; set < s_set_count; set++) {
    for (int way = 0; way < kSimBackgroundVoxelModelCacheWays; way++) {
      const SimBackgroundVoxelModelCacheEntry *source = &CacheSet(set)[way];
      if (!source->valid) continue;
      const uint32_t destination_set = HashKey(&source->key) & (new_sets - 1);
      SimBackgroundVoxelModelCacheEntry *destination =
          expanded + destination_set * kSimBackgroundVoxelModelCacheWays;
      for (int i = 0; i < kSimBackgroundVoxelModelCacheWays; i++) {
        if (!destination[i].valid) {
          destination[i] = *source;
          break;
        }
      }
    }
  }
  free(s_expanded_entries);
  /* The old directory no longer owns the moved allocations. */
  memset(g_model_cache.entries, 0, sizeof(g_model_cache.entries));
  s_expanded_entries = expanded;
  s_set_count = new_sets;
  return true;
}
