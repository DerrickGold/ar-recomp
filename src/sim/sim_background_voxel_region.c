#include "sim_background_voxel_region.h"

#include <stdbool.h>
#include <stddef.h>

/* Transcription of the eight house visual families selected by the game's
 * $03:A085 lookup through the 6x3 table at $03:DCC6. The ROM values are even
 * byte offsets 0..14; dividing by two produces these family identifiers.
 *
 *              civilization level 0       1                    2
 * Fillmore     tent                       timber               Fillmore
 * Bloodpool    tent                       timber               Bloodpool
 * Kasandora    tent                       early stone          developed stone
 * Aitos        tent                       timber               Aitos
 * Marahna      tent                       Marahna              timber
 * Northwall    tent                       timber               developed stone
 */
static const uint8_t kHouseStyleByTownAndLevel[6][3] = {
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Fillmore},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Bloodpool},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_KasandoraEarlyStone,
   kSimBackgroundHouseStyle_KasandoraStone},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Aitos},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Marahna,
   kSimBackgroundHouseStyle_Timber},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_KasandoraStone},
};

SimBackgroundVoxelHouseStyle SimBackgroundVoxelRegion_HouseStyle(
    uint8_t town, uint8_t development_level) {
  /* Hand-authored test fixtures created before regional identity existed are
   * treated as the established Fillmore model. Classified game objects always
   * carry a valid 1-based town. */
  if (town < 1 || town > 6)
    return kSimBackgroundHouseStyle_Fillmore;
  if (development_level > 2) development_level = 2;
  return (SimBackgroundVoxelHouseStyle)
      kHouseStyleByTownAndLevel[town - 1][development_level];
}

SimBackgroundVoxelTreeStyle SimBackgroundVoxelRegion_TreeStyle(uint8_t town) {
  static const uint8_t styles[6] = {
    kSimBackgroundTreeStyle_Temperate,
    kSimBackgroundTreeStyle_Wetland,
    kSimBackgroundTreeStyle_Dryland,
    kSimBackgroundTreeStyle_Highland,
    kSimBackgroundTreeStyle_Tropical,
    kSimBackgroundTreeStyle_SnowFir,
  };
  return town >= 1 && town <= 6
      ? (SimBackgroundVoxelTreeStyle)styles[town - 1]
      : kSimBackgroundTreeStyle_Temperate;
}

SimBackgroundVoxelPaletteStyle SimBackgroundVoxelRegion_PaletteStyle(
    const SimBackgroundVoxelObject *object) {
  if (!object) return kSimBackgroundPaletteStyle_Common;
  if (object->kind != kSimBackgroundVoxel_House)
    return kSimBackgroundPaletteStyle_Common;
  switch (SimBackgroundVoxelRegion_HouseStyle(
      object->town, object->development_level)) {
    case kSimBackgroundHouseStyle_Tent:
      return kSimBackgroundPaletteStyle_Canvas;
    case kSimBackgroundHouseStyle_Timber:
      return kSimBackgroundPaletteStyle_Timber;
    case kSimBackgroundHouseStyle_Fillmore:
      return kSimBackgroundPaletteStyle_Fillmore;
    case kSimBackgroundHouseStyle_Bloodpool:
      return kSimBackgroundPaletteStyle_Bloodpool;
    case kSimBackgroundHouseStyle_KasandoraEarlyStone:
    case kSimBackgroundHouseStyle_KasandoraStone:
      return kSimBackgroundPaletteStyle_KasandoraStone;
    case kSimBackgroundHouseStyle_Aitos:
      return kSimBackgroundPaletteStyle_Aitos;
    case kSimBackgroundHouseStyle_Marahna:
      return kSimBackgroundPaletteStyle_Marahna;
    case kSimBackgroundHouseStyle_Count:
      break;
  }
  return kSimBackgroundPaletteStyle_Common;
}

float SimBackgroundVoxelRegion_AuthoredHeight(
    const SimBackgroundVoxelObject *object) {
  if (!object) return 16.0f;
  bool construction =
      (object->flags & kSimBackgroundVoxel_UnderConstruction) != 0;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      switch (SimBackgroundVoxelRegion_HouseStyle(
          object->town, object->development_level)) {
        case kSimBackgroundHouseStyle_Tent: return 9.5f;
        case kSimBackgroundHouseStyle_Timber: return 11.5f;
        case kSimBackgroundHouseStyle_Fillmore: return 15.6f;
        case kSimBackgroundHouseStyle_Bloodpool: return 15.0f;
        case kSimBackgroundHouseStyle_KasandoraEarlyStone: return 10.5f;
        case kSimBackgroundHouseStyle_KasandoraStone: return 13.5f;
        case kSimBackgroundHouseStyle_Aitos: return 14.0f;
        case kSimBackgroundHouseStyle_Marahna: return 12.5f;
        case kSimBackgroundHouseStyle_Count: return 15.6f;
      }
      break;
    case kSimBackgroundVoxel_Cathedral: return 24.0f;
    case kSimBackgroundVoxel_Windmill: return construction ? 24.0f : 31.0f;
    case kSimBackgroundVoxel_Factory: return construction ? 10.0f : 17.0f;
    case kSimBackgroundVoxel_Tree:
      return SimBackgroundVoxelRegion_TreeStyle(object->town) ==
          kSimBackgroundTreeStyle_SnowFir ? 16.0f : 15.0f;
  }
  return 16.0f;
}
