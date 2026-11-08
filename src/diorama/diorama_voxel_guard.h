#ifndef AR_DIORAMA_VOXEL_GUARD_H
#define AR_DIORAMA_VOXEL_GUARD_H

#include "diorama_trunk_guard.h"

/* One finite native strip below the primary capture. This is extra source,
 * never an extra native scanline, repeated map edge, or enlarged scanout. */
enum {
  kDioramaVoxelGuardWidth = 640,
  kDioramaVoxelGuardRows = 64,
  kDioramaVoxelGuardPixels = 640 * 64
};
typedef struct DioramaVoxelGuardLayout {
  int primary_y, primary_height;
  DioramaTrunkGuardBounds source;
  DioramaTrunkGuardStrip lower;
} DioramaVoxelGuardLayout;

static inline bool DioramaVoxelGuard_Resolve(int world_x, int primary_y,
    int width, int primary_height, DioramaTrunkGuardBounds source,
    DioramaVoxelGuardLayout *out) {
  if (!out) return false;
  *out = (DioramaVoxelGuardLayout){0};
  DioramaTrunkGuardLayout layout;
  if (!DioramaTrunkGuard_Resolve(world_x, primary_y, width, primary_height,
      source, &layout)) return false;
  out->primary_y = primary_y;
  out->primary_height = primary_height;
  out->source = source;
  out->lower = layout.strips[1];
  out->lower.atlas_y = 0;
  return true;
}

typedef struct DioramaVoxelGuardSource {
  DioramaVoxelGuardLayout layout;
  /* Raw native Low, High, Far. Keep them separate even when edited lanes
   * overlap. Raw Far is NOT the renderer's derived Voxel foreground plane. */
  const uint32_t *pixels[3];
  size_t pitch_pixels, pixel_count;
  uint64_t capture_generation;
} DioramaVoxelGuardSource;

static inline bool DioramaVoxelGuard_Valid(const DioramaVoxelGuardSource *source) {
  if (!source || !source->capture_generation ||
      source->pitch_pixels != kDioramaVoxelGuardWidth ||
      source->pixel_count < kDioramaVoxelGuardPixels) return false;
  for (unsigned i = 0; i < 3; ++i) if (!source->pixels[i]) return false;
  DioramaVoxelGuardLayout expected;
  if (!DioramaVoxelGuard_Resolve(source->layout.lower.world_x,
      source->layout.primary_y, source->layout.lower.width,
      source->layout.primary_height, source->layout.source, &expected)) return false;
  const DioramaTrunkGuardStrip *a = &source->layout.lower, *b = &expected.lower;
  return a->world_y == b->world_y && a->atlas_y == b->atlas_y &&
      a->valid.x == b->valid.x && a->valid.y == b->valid.y &&
      a->valid.width == b->valid.width && a->valid.height == b->valid.height;
}
#endif
