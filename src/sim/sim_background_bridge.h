#ifndef SIM_BACKGROUND_BRIDGE_H
#define SIM_BACKGROUND_BRIDGE_H

#include "sim_background_voxel_types.h"

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

#endif  /* SIM_BACKGROUND_BRIDGE_H */
