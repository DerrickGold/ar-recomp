#ifndef SIM_BACKGROUND_VOXEL_BIOME_H
#define SIM_BACKGROUND_VOXEL_BIOME_H

#include <stdint.h>

#include "sim_background_voxel_models.h"

typedef enum SimBackgroundVoxelBiome {
  kSimBackgroundVoxelBiome_Temperate = 0,
  kSimBackgroundVoxelBiome_Wetland,
  kSimBackgroundVoxelBiome_Desert,
  kSimBackgroundVoxelBiome_Volcanic,
  kSimBackgroundVoxelBiome_Tropical,
  kSimBackgroundVoxelBiome_Snow,
  kSimBackgroundVoxelBiome_Count,
} SimBackgroundVoxelBiome;

SimBackgroundVoxelBiome SimBackgroundVoxelBiome_ForTown(uint8_t town);

/* Quality-aware surface substitution. Northwall receives snow only on broad
 * upward roof/foliage planes at High+, keeping Performance's material and
 * submission path identical to other towns. */
SimBackgroundVoxelMaterial SimBackgroundVoxelBiome_SurfaceMaterial(
    SimBackgroundVoxelBiome biome,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelMaterial material,
    const SimBackgroundVoxelModelFace *face);

#endif  /* SIM_BACKGROUND_VOXEL_BIOME_H */
