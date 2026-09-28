#ifndef AR_SIM_BACKGROUND_VOXEL_BIOME_H
#define AR_SIM_BACKGROUND_VOXEL_BIOME_H
/* SimBackgroundVoxelBiome: each town's biome (temperate, wetland, desert,
 * volcanic, tropical, snow) and the material substitutions it applies, such
 * as snow on Northwall's roofs and broad upward foliage faces.
 * Phase: pure.
 * Tests: tests/sim_background_voxel_biome_test.c */

#include <stdint.h>

#include "sim/voxels/sim_background_voxel_models.h"

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

/* Snow covers pitched roofs at every detail level, including steep entrance
 * hoods. Foliage retains the High+ broad-upward-face rule. Vertical gables
 * and downward-facing surfaces are not snow-covered. */
SimBackgroundVoxelMaterial SimBackgroundVoxelBiome_SurfaceMaterial(
    SimBackgroundVoxelBiome biome,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelMaterial material,
    const SimBackgroundVoxelModelFace *face);

#endif  /* AR_SIM_BACKGROUND_VOXEL_BIOME_H */
