#ifndef SIM_WORLD_NAVIGATION_CLIFFS_H
#define SIM_WORLD_NAVIGATION_CLIFFS_H

#include <stddef.h>
#include "sim_world_map.h"

enum { kSimWorldNavigationCliffMaximumFaces = kSimWorldMapTiles * kSimWorldMapTiles * 5 };

/* Sparse replacements for the shared overview grid: only cells with an
 * authored cliff material or a different owned corner need a private cap.
 * All heights are absolute registered world units, not model-local rises. */
typedef struct SimWorldNavigationCliffFace {
  float x[4], y[4], height[4], u[4], v[4];
  float shade;
} SimWorldNavigationCliffFace;

typedef struct SimWorldNavigationCliffScene {
  SimWorldNavigationCliffFace *faces;
  size_t face_count, face_capacity;
  /* One-based cap index, zero keeps the original shared-grid cell. */
  uint16_t replacement[kSimWorldMapBytes];
  uint8_t town_mask;
} SimWorldNavigationCliffScene;

/* Zero-initialize before first use. Rebuild only on geography/settings/source
 * changes. Allocation failure clears the scene and preserves the old grid. */
bool SimWorldNavigationCliffs_Build(
    uint8_t town_mask, SimWorldNavigationCliffScene *scene);
void SimWorldNavigationCliffs_Destroy(SimWorldNavigationCliffScene *scene);

#endif
