#include "sim_background_voxel_region.h"

#include <stdbool.h>
#include <stddef.h>

#include "sim_background_bridge.h"

/* Transcription of the eight house visual families selected by the game's
 * $03:A085 lookup through the 6x3 table at $03:DCC6. The ROM values are even
 * byte offsets 0..14; dividing by two produces these family identifiers.
 *
 *              civilization level 0       1                    2
 * Fillmore     tent                       timber               Fillmore
 * Bloodpool    tent                       timber               Bloodpool
 * Kasandora    brown yurt                 white tent           adobe
 * Aitos        tent                       timber               Aitos
 * Marahna      brown yurt                 stilt hut            log cabin
 * Northwall    tent                       timber               developed stone
 */
static const uint8_t kHouseStyleByTownAndLevel[kSimBackgroundTownCount]
    [kSimBackgroundDevelopmentLevelCount] = {
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Fillmore},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Bloodpool},
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_WhiteTent,
   kSimBackgroundHouseStyle_Adobe},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Aitos},
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_MarahnaStilt,
   kSimBackgroundHouseStyle_MarahnaLogCabin},
  {kSimBackgroundHouseStyle_Tent,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Stone},
};

SimBackgroundVoxelHouseStyle SimBackgroundVoxelRegion_HouseStyle(
    uint8_t town, uint8_t development_level) {
  /* Hand-authored test fixtures created before regional identity existed are
   * treated as the established Fillmore model. Classified game objects always
   * carry a valid 1-based town. */
  if (town < 1 || town > kSimBackgroundTownCount)
    return kSimBackgroundHouseStyle_Fillmore;
  if (development_level >= kSimBackgroundDevelopmentLevelCount)
    development_level = kSimBackgroundDevelopmentLevelCount - 1;
  return (SimBackgroundVoxelHouseStyle)
      kHouseStyleByTownAndLevel[town - 1][development_level];
}

SimBackgroundVoxelTreeStyle SimBackgroundVoxelRegion_TreeStyle(uint8_t town) {
  static const uint8_t styles[kSimBackgroundTownCount] = {
    kSimBackgroundTreeStyle_Temperate,
    kSimBackgroundTreeStyle_Wetland,
    kSimBackgroundTreeStyle_Dryland,
    kSimBackgroundTreeStyle_Highland,
    kSimBackgroundTreeStyle_Tropical,
    kSimBackgroundTreeStyle_SnowFir,
  };
  return town >= 1 && town <= kSimBackgroundTownCount
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
    case kSimBackgroundHouseStyle_Yurt:
      return kSimBackgroundPaletteStyle_Yurt;
    case kSimBackgroundHouseStyle_WhiteTent:
      return kSimBackgroundPaletteStyle_WhiteCanvas;
    case kSimBackgroundHouseStyle_Adobe:
      return kSimBackgroundPaletteStyle_Adobe;
    case kSimBackgroundHouseStyle_Stone:
      return kSimBackgroundPaletteStyle_Stone;
    case kSimBackgroundHouseStyle_Aitos:
      return kSimBackgroundPaletteStyle_Aitos;
    case kSimBackgroundHouseStyle_MarahnaStilt:
      return kSimBackgroundPaletteStyle_MarahnaStilt;
    case kSimBackgroundHouseStyle_MarahnaLogCabin:
      return kSimBackgroundPaletteStyle_MarahnaLogCabin;
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
        case kSimBackgroundHouseStyle_Yurt: return 8.8f;
        case kSimBackgroundHouseStyle_WhiteTent: return 10.0f;
        case kSimBackgroundHouseStyle_Adobe: return 11.0f;
        case kSimBackgroundHouseStyle_Stone: return 13.5f;
        case kSimBackgroundHouseStyle_Aitos: return 14.0f;
        case kSimBackgroundHouseStyle_MarahnaStilt: return 12.5f;
        case kSimBackgroundHouseStyle_MarahnaLogCabin: return 12.0f;
        case kSimBackgroundHouseStyle_Count: return 15.6f;
      }
      break;
    case kSimBackgroundVoxel_Cathedral: return 24.0f;
    case kSimBackgroundVoxel_Windmill: return construction ? 24.0f : 31.0f;
    case kSimBackgroundVoxel_Factory: return construction ? 10.0f : 17.0f;
    case kSimBackgroundVoxel_Tree:
      return SimBackgroundVoxelRegion_TreeStyle(object->town) ==
          kSimBackgroundTreeStyle_SnowFir ? 16.0f : 15.0f;
    case kSimBackgroundVoxel_BroadTree: return 14.0f;
    case kSimBackgroundVoxel_Palm: return 15.5f;
    case kSimBackgroundVoxel_Shrub: return 12.0f;
    /* The three unique landmarks each own a 2x2 plot, so their heights are
     * measured against a 32-pixel base rather than the old oversized cover. */
    case kSimBackgroundVoxel_StoryTree: return 30.0f;
    case kSimBackgroundVoxel_BloodpoolCastle: return 32.0f;
    case kSimBackgroundVoxel_MarahnaTemple: return 24.0f;
    case kSimBackgroundVoxel_Pyramid: return 28.0f;
    case kSimBackgroundVoxel_Bridge:
      return SimBackgroundBridge_AuthoredHeight();
  }
  return 16.0f;
}
