#ifndef ACTRAISER_DIORAMA_VOXEL_CULL_H
#define ACTRAISER_DIORAMA_VOXEL_CULL_H

#include <math.h>
#include "render/render_types.h"
#include "render/scene3d_math.h"

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

/* Test a conservative scene-space box against the viewport before building
 * its faces. A plane's minimum over a box is at the corner selected by the
 * signs of its coefficients. Boxes crossing the camera plane stay visible. */
static inline bool DioramaVoxelBoxOutsideViewport(const float matrix[16],
    const float lower[3], const float upper[3], int width, int height) {
  if (width <= 0 || height <= 0) return false;
  Scene3DClipPoint center;
  const float half[3] = {(upper[0] - lower[0]) * .5f,
      (upper[1] - lower[1]) * .5f, (upper[2] - lower[2]) * .5f};
  if (!Scene3D_TransformToClip(matrix, lower[0] + half[0], lower[1] + half[1],
          lower[2] + half[2], &center)) return false;
  const float w_radius = fabsf(matrix[3]) * half[0] +
      fabsf(matrix[7]) * half[1] + fabsf(matrix[11]) * half[2];
  if (!(center.w - w_radius > kScene3DMinimumProjectionDepth)) return false;
  const float sides[] = {1 + 4.f / width, 1 + 4.f / height};
  const float coordinates[] = {center.x, center.y};
  for (int axis = 0; axis < 2; ++axis) {
    const float w = sides[axis];
    for (int sign = -1; sign <= 1; sign += 2) {
      const float radius = fabsf(sign * matrix[axis] - w * matrix[3]) * half[0] +
          fabsf(sign * matrix[axis + 4] - w * matrix[7]) * half[1] +
          fabsf(sign * matrix[axis + 8] - w * matrix[11]) * half[2];
      if (sign * coordinates[axis] - w * center.w > radius + .0001f) return true;
    }
  }
  return false;
}

#endif
