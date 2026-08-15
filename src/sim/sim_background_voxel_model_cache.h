#ifndef SIM_BACKGROUND_VOXEL_MODEL_CACHE_H
#define SIM_BACKGROUND_VOXEL_MODEL_CACHE_H

#include <stdint.h>

#include "sim_background_voxel_models.h"

enum {
  /* A fully developed, zoomed-out town can expose more than 256 independently
   * seeded houses and trees. Four-way sets keep lookup bounded while the
   * larger capacity avoids the sequential LRU thrash of the former linear
   * 256-entry table. */
  kSimBackgroundVoxelModelCacheCapacity = 512,
  kSimBackgroundVoxelModelCacheWays = 4,
  kSimBackgroundVoxelModelCacheSetCount =
      kSimBackgroundVoxelModelCacheCapacity /
      kSimBackgroundVoxelModelCacheWays,
};

typedef struct SimBackgroundVoxelModelCacheStats {
  uint32_t hits;
  uint32_t misses;
  uint32_t evictions;
} SimBackgroundVoxelModelCacheStats;

/* Render-thread cache for compiled, cleaned models. `stamp` is a monotonically
 * increasing render-pass identifier used only for LRU replacement. */
const SimBackgroundVoxelModel *SimBackgroundVoxelModelCache_Get(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    uint32_t stamp);
SimBackgroundVoxelModelCacheStats SimBackgroundVoxelModelCache_Stats(void);
void SimBackgroundVoxelModelCache_Reset(void);

#endif  /* SIM_BACKGROUND_VOXEL_MODEL_CACHE_H */
