#ifndef SIM_BACKGROUND_VOXEL_REGION_H
#define SIM_BACKGROUND_VOXEL_REGION_H

#include <stdint.h>

#include "sim_background_voxel_types.h"

/* These are architectural families, not quality levels. Several towns share
 * a family where the original game deliberately reuses one, while the ROM's
 * $03:DCC6 town/civilization lookup keeps the distinct regional progressions. */
typedef enum SimBackgroundVoxelHouseStyle {
  kSimBackgroundHouseStyle_Tent,
  kSimBackgroundHouseStyle_Timber,
  kSimBackgroundHouseStyle_Fillmore,
  kSimBackgroundHouseStyle_Bloodpool,
  kSimBackgroundHouseStyle_Yurt,
  kSimBackgroundHouseStyle_WhiteTent,
  kSimBackgroundHouseStyle_Adobe,
  kSimBackgroundHouseStyle_Stone,
  kSimBackgroundHouseStyle_Aitos,
  kSimBackgroundHouseStyle_MarahnaStilt,
  kSimBackgroundHouseStyle_MarahnaLogCabin,
  kSimBackgroundHouseStyle_Count,
} SimBackgroundVoxelHouseStyle;

typedef enum SimBackgroundVoxelTreeStyle {
  kSimBackgroundTreeStyle_Temperate,
  kSimBackgroundTreeStyle_Wetland,
  kSimBackgroundTreeStyle_Dryland,
  kSimBackgroundTreeStyle_Highland,
  kSimBackgroundTreeStyle_Tropical,
  kSimBackgroundTreeStyle_SnowFir,
} SimBackgroundVoxelTreeStyle;

typedef enum SimBackgroundVoxelPaletteStyle {
  kSimBackgroundPaletteStyle_Common,
  kSimBackgroundPaletteStyle_Canvas,
  kSimBackgroundPaletteStyle_Timber,
  kSimBackgroundPaletteStyle_Fillmore,
  kSimBackgroundPaletteStyle_Bloodpool,
  kSimBackgroundPaletteStyle_Yurt,
  kSimBackgroundPaletteStyle_WhiteCanvas,
  kSimBackgroundPaletteStyle_Adobe,
  kSimBackgroundPaletteStyle_Stone,
  kSimBackgroundPaletteStyle_Aitos,
  kSimBackgroundPaletteStyle_MarahnaStilt,
  kSimBackgroundPaletteStyle_MarahnaLogCabin,
} SimBackgroundVoxelPaletteStyle;

SimBackgroundVoxelHouseStyle SimBackgroundVoxelRegion_HouseStyle(
    uint8_t town, uint8_t development_level);
SimBackgroundVoxelTreeStyle SimBackgroundVoxelRegion_TreeStyle(uint8_t town);
SimBackgroundVoxelPaletteStyle SimBackgroundVoxelRegion_PaletteStyle(
    const SimBackgroundVoxelObject *object);

/* Conservative authored bounds used by viewport/Lod projection before a
 * cached model is requested. Keep these paired with the regional builders. */
float SimBackgroundVoxelRegion_AuthoredHeight(
    const SimBackgroundVoxelObject *object);

#endif  /* SIM_BACKGROUND_VOXEL_REGION_H */
