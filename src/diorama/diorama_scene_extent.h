#ifndef AR_DIORAMA_SCENE_EXTENT_H
#define AR_DIORAMA_SCENE_EXTENT_H
#include "diorama_layer_order.h"

/* Shared finite-scenery policy for gameplay and the isolated editor scene. */
static inline void DioramaScenery_AxisExtent(
    const DioramaRoomOverride *room, unsigned layer, int native_extent,
    bool vertical, int *world_start, int *world_extent) {
  int start = 0, end = native_extent;
  if (room && layer < 2) {
    const DioramaStampLayerOverride *stamps = &room->stamp_layers[layer];
    /* Map bounds describe editor workspace, which can outlive deleted tiles.
     * Only actual cells extend the finite scenery: empty workspace must not
     * consume capture rows or keep the Diorama camera away from an edge. */
    const DioramaMapBounds *bounds = &stamps->occupied_bounds;
    if (bounds->set) {
      const int first = (vertical ? bounds->y0 : bounds->x0) * 16;
      const int last = (vertical ? bounds->y1 : bounds->x1) * 16;
      if (first < start) start = first;
      if (last > end) end = last;
    } else for (size_t i = 0; i < stamps->count; i++) {
      const int cell_start = (vertical ? stamps->cells[i].y : stamps->cells[i].x) *
          16;
      const int cell_end = cell_start + 16;
      if (cell_start < start) start = cell_start;
      if (cell_end > end) end = cell_end;
    }
  }
  if (world_start) *world_start = start;
  if (world_extent) *world_extent = end - start;
}

#endif
