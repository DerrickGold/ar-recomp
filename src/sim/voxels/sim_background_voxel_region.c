#include "sim/voxels/sim_background_voxel_region.h"

#include <stdbool.h>
#include <stddef.h>

#include "sim/voxels/sim_background_bridge.h"

/* Transcription of the eight house visual families selected by the game's
 * $03:A085 lookup through the 6x3 table at $03:DCC6. The ROM values are even
 * byte offsets 0..14; dividing by two produces these family identifiers.
 *
 *              civilization level 0       1                    2
 * Fillmore     straw hut                  timber               Fillmore
 * Bloodpool    straw hut                  timber               Bloodpool
 * Kasandora    straw hut                  white tent           adobe
 * Aitos        straw hut                  timber               Aitos
 * Marahna      straw hut                  stilt hut            log cabin
 * Northwall    straw hut                  timber               flat stone
 */
static const uint8_t kHouseStyleByTownAndLevel[kSimBackgroundTownCount]
    [kSimBackgroundDevelopmentLevelCount] = {
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Fillmore},
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Bloodpool},
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_WhiteTent,
   kSimBackgroundHouseStyle_Adobe},
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_Timber,
   kSimBackgroundHouseStyle_Aitos},
  {kSimBackgroundHouseStyle_Yurt,
   kSimBackgroundHouseStyle_MarahnaStilt,
   kSimBackgroundHouseStyle_MarahnaLogCabin},
  {kSimBackgroundHouseStyle_Yurt,
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

SimBackgroundVoxelHouseStyle SimBackgroundVoxelRegion_ObjectHouseStyle(
    const SimBackgroundVoxelObject *object) {
  if (!object) return kSimBackgroundHouseStyle_Fillmore;
  uint8_t tile = object->visual_metatile;
  if (object->visual_state == kSimStructureVisualState_Finished &&
      tile <= 0x3B && ((tile & 7u) == 2u || (tile & 7u) == 3u)) {
    static const SimBackgroundVoxelHouseStyle families[] = {
      kSimBackgroundHouseStyle_Yurt, kSimBackgroundHouseStyle_Timber,
      kSimBackgroundHouseStyle_Fillmore, kSimBackgroundHouseStyle_Bloodpool,
      kSimBackgroundHouseStyle_WhiteTent, kSimBackgroundHouseStyle_Adobe,
      kSimBackgroundHouseStyle_Aitos, kSimBackgroundHouseStyle_MarahnaStilt,
    };
    unsigned family = tile >> 3;
    if (family == 1 && object->town == 5)
      return kSimBackgroundHouseStyle_MarahnaLogCabin;
    if (family == 5 && object->town == 6)
      return kSimBackgroundHouseStyle_Stone;
    return families[family];
  }
  return SimBackgroundVoxelRegion_HouseStyle(
      object->town, object->development_level);
}

int SimBackgroundVoxelRegion_RockKind(uint8_t terrain_metatile) {
  switch (terrain_metatile) {
    case 0x61: return kSimBackgroundVoxel_Boulder;
    case 0x62: case 0x63: case 0x69: case 0x6A: case 0x6B:
      return kSimBackgroundVoxel_Rocks;
    default: return kSimBackgroundVoxelKindCount;
  }
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
  switch (SimBackgroundVoxelRegion_ObjectHouseStyle(object)) {
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
      if (construction) {
        SimBackgroundVoxelHouseStyle family = SimBackgroundVoxelRegion_ObjectHouseStyle(object);
        if (family == kSimBackgroundHouseStyle_Yurt)
          return object->flags & kSimBackgroundVoxel_AlternateFacing ? 12.8f : 13.8f;
        if (family == kSimBackgroundHouseStyle_WhiteTent || family == kSimBackgroundHouseStyle_Tent)
          return 11.5f;
        return object->animation_phase ? 12.45f : 8.45f;
      }
      switch (SimBackgroundVoxelRegion_ObjectHouseStyle(object)) {
        case kSimBackgroundHouseStyle_Tent: return 11.5f;
        case kSimBackgroundHouseStyle_Timber: return 11.5f;
        case kSimBackgroundHouseStyle_Fillmore: return 15.6f;
        case kSimBackgroundHouseStyle_Bloodpool: return 15.0f;
        case kSimBackgroundHouseStyle_Yurt:
          return object->flags & kSimBackgroundVoxel_AlternateFacing ? 12.8f : 14.6f;
        case kSimBackgroundHouseStyle_WhiteTent: return 11.5f;
        case kSimBackgroundHouseStyle_Adobe: return object->flags & kSimBackgroundVoxel_AlternateFacing ? 14.2f : 11.0f;
        case kSimBackgroundHouseStyle_Stone: return object->flags & kSimBackgroundVoxel_AlternateFacing ? 14.2f : 11.0f;
        case kSimBackgroundHouseStyle_Aitos: return object->flags & kSimBackgroundVoxel_AlternateFacing ? 13.6f : 11.0f;
        case kSimBackgroundHouseStyle_MarahnaStilt: return 12.5f;
        case kSimBackgroundHouseStyle_MarahnaLogCabin: return 12.0f;
        case kSimBackgroundHouseStyle_Count: return 15.6f;
      }
      break;
    case kSimBackgroundVoxel_Cathedral: return 24.0f;
    case kSimBackgroundVoxel_Windmill: return construction ? 31.0f : 32.0f;
    case kSimBackgroundVoxel_Factory: return construction ? 12.0f : 17.0f;
    case kSimBackgroundVoxel_Tree:
      return SimBackgroundVoxelRegion_TreeStyle(object->town) ==
          kSimBackgroundTreeStyle_SnowFir ? 19.0f : 18.0f;
    case kSimBackgroundVoxel_BroadTree: return 14.0f;
    case kSimBackgroundVoxel_Palm: return 15.5f;
    case kSimBackgroundVoxel_Shrub: return 13.6f;
    case kSimBackgroundVoxel_Boulder: return 8.0f;
    case kSimBackgroundVoxel_Rocks: return 4.5f;
    /* The three unique landmarks each own a 2x2 plot, so their heights are
     * measured against a 32-pixel base rather than the old oversized cover. */
    case kSimBackgroundVoxel_StoryTree: return 30.0f;
    case kSimBackgroundVoxel_BloodpoolCastle: return 32.0f;
    case kSimBackgroundVoxel_MarahnaTemple: return 24.0f;
    case kSimBackgroundVoxel_Pyramid: return 28.0f;
    case kSimBackgroundVoxel_Bridge:
      return construction ? 4.5f : SimBackgroundBridge_AuthoredHeight();
  }
  return 16.0f;
}
