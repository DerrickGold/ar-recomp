#ifndef SIM_BACKGROUND_BRIDGE_H
#define SIM_BACKGROUND_BRIDGE_H

#include "sim_background_voxel_types.h"
#include "sim_town_terrain.h"

/* A bridge owns the water opening, with only a one-pixel masonry key into
 * each bank. The former bank-centre-to-bank-centre footprint put two
 * perpendicular Bloodpool bridges through the same land cell and made the
 * model almost twice as long as the native crossing art. */
enum {
  kSimBackgroundBridgeBankEmbedPixels = 1,
  kSimBackgroundBridgeEastWestDepthPixels = 10,
  kSimBackgroundBridgeNorthSouthWidthPixels = 8,
  kSimBackgroundBridgeCrossAxisInsetPixels = 4,
};

static inline float SimBackgroundBridge_AuthoredHeight(void) {
  return 3.75f;
}

typedef struct SimBackgroundBridgeBounds {
  float origin_x, origin_y;
  float width, depth;
} SimBackgroundBridgeBounds;

static inline SimBackgroundBridgeBounds SimBackgroundBridge_ResolveBounds(
    const SimBackgroundVoxelObject *object) {
  SimBackgroundBridgeBounds bounds = {0};
  if (!object) return bounds;
  bounds.origin_x = object->cell_x * (float)kSimBackgroundCellPixels;
  bounds.origin_y = object->cell_y * (float)kSimBackgroundCellPixels;
  if (object->bridge_axis == kSimBackgroundBridgeAxis_EastWest) {
    bounds.depth = kSimBackgroundBridgeEastWestDepthPixels;
    int water_cells =
        (int)object->bridge_bank_b_x - object->bridge_bank_a_x - 1;
    bounds.origin_x =
        (object->bridge_bank_a_x + 1) *
            (float)kSimBackgroundCellPixels -
        kSimBackgroundBridgeBankEmbedPixels;
    bounds.origin_y += kSimBackgroundBridgeCrossAxisInsetPixels;
    if (water_cells > 0)
      bounds.width =
          water_cells * (float)kSimBackgroundCellPixels +
          2.0f * kSimBackgroundBridgeBankEmbedPixels;
  } else if (object->bridge_axis ==
             kSimBackgroundBridgeAxis_NorthSouth) {
    bounds.width = kSimBackgroundBridgeNorthSouthWidthPixels;
    int water_cells =
        (int)object->bridge_bank_b_y - object->bridge_bank_a_y - 1;
    bounds.origin_x += kSimBackgroundBridgeCrossAxisInsetPixels;
    bounds.origin_y =
        (object->bridge_bank_a_y + 1) *
            (float)kSimBackgroundCellPixels -
        kSimBackgroundBridgeBankEmbedPixels;
    if (water_cells > 0)
      bounds.depth =
          water_cells * (float)kSimBackgroundCellPixels +
          2.0f * kSimBackgroundBridgeBankEmbedPixels;
  }
  return bounds;
}

/* Share the bridge datums between the legacy planar and continuous-town
 * presenters. The bank approach sets visual placement; the whole footprint
 * supplies a conservative depth envelope on transverse slopes. */
static inline bool SimBackgroundBridge_TerrainHeights(
    const SimBackgroundVoxelObject *object, uint8_t town,
    float *approach, float *envelope) {
  if (!object || !approach || !envelope) return false;
  *approach = *envelope = 0.0f;
  const bool east_west = object->bridge_axis == kSimBackgroundBridgeAxis_EastWest;
  if (!east_west && object->bridge_axis != kSimBackgroundBridgeAxis_NorthSouth)
    return false;
  if (!SimTownTerrain_LevelPairUnits(town,
          object->bridge_bank_a_x, object->bridge_bank_a_y,
          east_west ? 1.0f : 0.5f, east_west ? 0.5f : 1.0f,
          object->bridge_bank_b_x, object->bridge_bank_b_y,
          east_west ? 0.0f : 0.5f, east_west ? 0.5f : 0.0f, approach))
    return false;
  const SimBackgroundBridgeBounds bounds = SimBackgroundBridge_ResolveBounds(object);
  if (bounds.width <= 0 || bounds.depth <= 0 ||
      !SimTownTerrain_MaximumUnitsInRect(town, bounds.origin_x, bounds.origin_y,
          bounds.origin_x + bounds.width, bounds.origin_y + bounds.depth, envelope))
    return false;
  if (*envelope < *approach) *envelope = *approach;
  return true;
}

#endif  /* SIM_BACKGROUND_BRIDGE_H */
