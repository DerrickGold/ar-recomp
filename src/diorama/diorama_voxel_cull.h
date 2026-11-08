#ifndef ACTRAISER_DIORAMA_VOXEL_CULL_H
#define ACTRAISER_DIORAMA_VOXEL_CULL_H

#include <math.h>
#include "render/render_types.h"

/* Projected triangle interiors cannot cross a viewport side when all four
 * quad vertices lie beyond that side. Keep a small raster/AA fringe and fail
 * open on invalid input. Texture blur samples inside the existing polygon;
 * it does not enlarge its coverage. This never changes source clipping. */
static inline bool DioramaVoxelQuadOutsideViewport(const ArRenderVertex2D vertices[4],
                                                  int width, int height) {
  if (width <= 0 || height <= 0) return false;
  unsigned outside = 15;
  for (unsigned i = 0; i < 4; ++i) {
    const float x = vertices[i].position.x, y = vertices[i].position.y;
    if (!isfinite(x) || !isfinite(y)) return false;
    outside &= (x < -2.f ? 1u : 0) | (x > width + 2.f ? 2u : 0) |
               (y < -2.f ? 4u : 0) | (y > height + 2.f ? 8u : 0);
  }
  return outside != 0;
}

#endif
