#ifndef SIM_BACKGROUND_MOUNTAIN_OBJECTS_H
#define SIM_BACKGROUND_MOUNTAIN_OBJECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_mountains.h"

enum {
  kSimBackgroundMountainObjectMaxColumns = 8,
  kSimBackgroundMountainObjectMaxRows = 8,
  kSimBackgroundMountainMaxObjects = 64,
};

typedef enum SimBackgroundMountainObjectFlags {
  kSimBackgroundMountainObject_Volcano = 1u << 0,
} SimBackgroundMountainObjectFlags;

/* A mountain object is one complete transparent source stamp with an
 * independent ground contact. Ranges remain useful composition data, but do
 * not share transforms or depth anchors in the enhanced renderer. */
typedef struct SimBackgroundMountainObject {
  int8_t cell_x, cell_y;
  uint8_t width_cells, height_cells;
  uint8_t flags;
  uint8_t row_occupied_mask[kSimBackgroundMountainObjectMaxRows];
  /* Explicit terrain-metatile sources keep one object independent from the
   * already-composited range surrounding any on-map example. A zero entry is
   * transparent and must agree with row_occupied_mask. */
  uint8_t source_tile[kSimBackgroundMountainObjectMaxRows]
                     [kSimBackgroundMountainObjectMaxColumns];
} SimBackgroundMountainObject;

typedef struct SimBackgroundMountainObjectList {
  uint8_t count;
  SimBackgroundMountainObject objects[kSimBackgroundMountainMaxObjects];
} SimBackgroundMountainObjectList;

/* Returns false when a town has not yet supplied validated complete-stamp
 * oracles. Callers retain the generic connected-art fallback for those towns. */
bool SimBackgroundMountainObjects_Build(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps,
    SimBackgroundMountainObjectList *out);

#endif  /* SIM_BACKGROUND_MOUNTAIN_OBJECTS_H */
