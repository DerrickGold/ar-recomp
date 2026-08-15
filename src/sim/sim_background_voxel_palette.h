#ifndef SIM_BACKGROUND_VOXEL_PALETTE_H
#define SIM_BACKGROUND_VOXEL_PALETTE_H

#include <stdint.h>

#include "sim_background_voxel_models.h"

enum { kSimBackgroundVoxelPaletteRampLevels = 4 };

typedef struct SimBackgroundVoxelPalette {
  uint32_t material[kSimVoxelMaterial_Count]
                   [kSimBackgroundVoxelPaletteRampLevels];
} SimBackgroundVoxelPalette;

void SimBackgroundVoxelPalette_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelPalette *palette);
uint32_t SimBackgroundVoxelPalette_Base(
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelMaterial material);
uint32_t SimBackgroundVoxelPalette_Ramp(
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelMaterial material,
    uint8_t brightness);

#endif  /* SIM_BACKGROUND_VOXEL_PALETTE_H */
