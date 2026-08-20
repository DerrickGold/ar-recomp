#ifndef SIM_BACKGROUND_VOXEL_PALETTE_H
#define SIM_BACKGROUND_VOXEL_PALETTE_H

#include <stdint.h>

#include "sim_background_voxel_biome.h"
#include "sim_background_voxel_models.h"

enum {
  kSimBackgroundVoxelPaletteRampLevels = 4,
  kSimBackgroundVoxelPaletteBaseLevel = 2,
  kSimBackgroundVoxelPaletteLevel1Brightness = 128,
  kSimBackgroundVoxelPaletteLevel2Brightness = 178,
  kSimBackgroundVoxelPaletteLevel3Brightness = 224,
};

static inline int SimBackgroundVoxelPalette_LevelForBrightness(
    uint8_t brightness) {
  return brightness < kSimBackgroundVoxelPaletteLevel1Brightness ? 0
      : brightness < kSimBackgroundVoxelPaletteLevel2Brightness ? 1
      : brightness < kSimBackgroundVoxelPaletteLevel3Brightness ? 2 : 3;
}

typedef struct SimBackgroundVoxelPalette {
  uint32_t material[kSimVoxelMaterial_Count]
                   [kSimBackgroundVoxelPaletteRampLevels];
} SimBackgroundVoxelPalette;

void SimBackgroundVoxelPalette_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelBiome biome,
    SimBackgroundVoxelPalette *palette);
uint32_t SimBackgroundVoxelPalette_Base(
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelMaterial material);
uint32_t SimBackgroundVoxelPalette_Ramp(
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelMaterial material,
    uint8_t brightness);

#endif  /* SIM_BACKGROUND_VOXEL_PALETTE_H */
