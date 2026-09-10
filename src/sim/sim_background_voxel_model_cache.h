#ifndef SIM_BACKGROUND_VOXEL_MODEL_CACHE_H
#define SIM_BACKGROUND_VOXEL_MODEL_CACHE_H

#include <stdint.h>
#include <stddef.h>

#include "sim_background_voxel_biome.h"
#include "sim_background_voxel_models.h"

enum {
  /* A fully developed, zoomed-out town can expose more than 256 independently
   * seeded houses and trees. Sixteen-way sets keep lookup tightly bounded while
   * retaining regional/tier variants that would otherwise collide in a
   * four-way set; the larger capacity avoids the sequential LRU thrash of the
   * former linear 256-entry table. */
  kSimBackgroundVoxelModelCacheCapacity = 512,
  kSimBackgroundVoxelModelCacheWays = 16,
  kSimBackgroundVoxelModelCacheSetCount =
      kSimBackgroundVoxelModelCacheCapacity /
      kSimBackgroundVoxelModelCacheWays,
};

/* Everything a face's shading depends on besides the model itself. The model
 * is already keyed on its own geometry, so a matching key means the stored
 * result is the one this frame would have recomputed. */
typedef struct SimBackgroundVoxelModelShadingKey {
  uint16_t light_azimuth_deg;
  uint8_t light_elevation_deg;
  uint8_t shading;
  uint8_t biome;
} SimBackgroundVoxelModelShadingKey;

/* Per-face lighting, resolved once per model per lighting state.
 *
 * It looked like per-frame work because it is issued per frame, but none of
 * its inputs move with the camera: the face geometry and corner occlusion are
 * fixed when the model is compiled, and the light direction, shading mode and
 * biome are settings. Recomputing it every frame cost 1.1ms of a 16.7ms frame
 * in a developed Bloodpool at Ultra - more than projecting the same geometry,
 * which genuinely does depend on the camera. */
typedef struct SimBackgroundVoxelModelShading {
  const uint8_t *material;
  const uint8_t (*brightness)[4];
} SimBackgroundVoxelModelShading;

/* Read-only compiled surface, without the author's unused face capacity or
 * construction boxes. Array extents are exactly face_count. This is a CPU
 * value view, not a renderer resource or a runner ABI record. */
typedef struct SimBackgroundVoxelModelView {
  uint16_t face_count;
  bool overflow;
  float min_x, min_y, min_z, max_x, max_y, max_z;
  const SimBackgroundVoxelModelFace *faces;
} SimBackgroundVoxelModelView;

typedef struct SimBackgroundVoxelModelCacheStats {
  uint32_t hits;
  uint32_t misses;
  uint32_t evictions;
  /* Times a cached model kept its geometry but had to relight. Expected to
   * spike for one frame after a light or quality change and be zero
   * otherwise; a steady non-zero count means the key is missing an input. */
  uint32_t relights;
  uint32_t allocation_failures;
  uint32_t capacity;
  size_t storage_bytes;
} SimBackgroundVoxelModelCacheStats;

/* Render-thread cache for compiled, cleaned models and their resolved
 * shading. Recency belongs to the cache, so independent SIM/globe callers
 * cannot rewind each other's replacement clock. Views are borrowed until an
 * evicting Get, Reserve or Reset; do not retain them across rendering passes.
 * `shading_key` and `out_shading` are optional together. Allocation failure
 * returns NULL without evicting a usable entry or publishing partial data. */
const SimBackgroundVoxelModelView *SimBackgroundVoxelModelCache_Get(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    const SimBackgroundVoxelModelShadingKey *shading_key,
    const SimBackgroundVoxelModelShading **out_shading);
SimBackgroundVoxelModelCacheStats SimBackgroundVoxelModelCache_Stats(void);
void SimBackgroundVoxelModelCache_Reset(void);

/* Render-thread working-set reservation for multi-town views. Grows the
 * portable CPU cache (bounded to 8192 entries), preserving cached models.
 * Call before fetching models: a successful growth invalidates borrowed
 * pointers. Allocation failure leaves the existing cache fully usable.
 * Reset releases extra storage and returns to the single-town capacity. */
bool SimBackgroundVoxelModelCache_Reserve(uint32_t minimum_entries);

#endif  /* SIM_BACKGROUND_VOXEL_MODEL_CACHE_H */
