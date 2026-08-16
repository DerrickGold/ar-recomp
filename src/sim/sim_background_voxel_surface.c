#include "sim_background_voxel_surface.h"

#include <math.h>

bool SimBackgroundVoxelSurface_OutwardNormal(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelSurfaceNormal *out) {
  if (!face || !out) return false;
  const SimBackgroundVoxelModelPoint *a = &face->points[0];
  const SimBackgroundVoxelModelPoint *b = &face->points[1];
  const SimBackgroundVoxelModelPoint *d = &face->points[3];
  float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
  float vx = d->x - a->x, vy = d->y - a->y, vz = d->z - a->z;
  float nx = uy * vz - uz * vy;
  float ny = uz * vx - ux * vz;
  float nz = ux * vy - uy * vx;
  float length = sqrtf(nx * nx + ny * ny + nz * nz);
  if (length < 0.0001f) return false;
  nx /= length;
  ny /= length;
  nz /= length;

  /* AddBox authors side faces from the inside winding and top faces from the
   * outside winding. Sloped roofs follow the top-face convention. */
  if (fabsf(nz) < 0.5f || nz < 0.0f) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  }
  *out = (SimBackgroundVoxelSurfaceNormal){nx, ny, nz};
  return true;
}
