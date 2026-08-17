#ifndef SIM_BACKGROUND_VOXEL_MODEL_CACHE_H
#define SIM_BACKGROUND_VOXEL_MODEL_CACHE_H

#include <stdint.h>

#include "sim_background_voxel_biome.h"
#include "sim_background_voxel_models.h"

enum {
  /* A fully developed, zoomed-out town can expose more than 256 independently
   * seeded houses and trees. Eight-way sets keep lookup tightly bounded while
   * retaining regional/tier variants that would otherwise collide in a
   * four-way set; the larger capacity avoids the sequential LRU thrash of the
   * former linear 256-entry table. */
  kSimBackgroundVoxelModelCacheCapacity = 512,
  kSimBackgroundVoxelModelCacheWays = 8,
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
  uint8_t material[kSimBackgroundVoxelModelMaxFaces];
  uint8_t brightness[kSimBackgroundVoxelModelMaxFaces][4];
} SimBackgroundVoxelModelShading;

typedef struct SimBackgroundVoxelModelCacheStats {
  uint32_t hits;
  uint32_t misses;
  uint32_t evictions;
  /* Times a cached model kept its geometry but had to relight. Expected to
   * spike for one frame after a light or quality change and be zero
   * otherwise; a steady non-zero count means the key is missing an input. */
  uint32_t relights;
} SimBackgroundVoxelModelCacheStats;

/* Render-thread cache for compiled, cleaned models and their resolved
 * shading. `stamp` is a monotonically increasing render-pass identifier used
 * only for LRU replacement. `shading_key` and `out_shading` are optional
 * together: pass neither to fetch geometry alone. */
const SimBackgroundVoxelModel *SimBackgroundVoxelModelCache_Get(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    uint32_t stamp,
    const SimBackgroundVoxelModelShadingKey *shading_key,
    const SimBackgroundVoxelModelShading **out_shading);
SimBackgroundVoxelModelCacheStats SimBackgroundVoxelModelCache_Stats(void);
void SimBackgroundVoxelModelCache_Reset(void);

#endif  /* SIM_BACKGROUND_VOXEL_MODEL_CACHE_H */
