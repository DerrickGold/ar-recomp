#ifndef AR_PRESENTATION_SURFACE_H
#define AR_PRESENTATION_SURFACE_H
/* Validate borrowed ARGB8888 runner surfaces and locate bounded regions.
 * These helpers do not retain pixels or decide which feature consumes them. */

#include <limits.h>
#include <stdbool.h>
#include "snesrecomp/runner.h"

static inline const SrPpuSurfaceView *PresentationSurface_Bound(
    const SrPpuSurfaceView *surface) {
  return surface && surface->data &&
      (surface->flags & SR_PPU_SURFACE_BOUND) != 0u &&
      surface->pixel_format == SR_PPU_PIXEL_FORMAT_ARGB8888_U32 &&
      surface->pitch_bytes != 0u &&
      surface->pitch_bytes <= INT_MAX &&
      surface->width_pixels == surface->pitch_bytes / sizeof(uint32_t) &&
      surface->byte_size >=
          surface->pitch_bytes * (uint64_t)surface->height_pixels
      ? surface : NULL;
}

static inline bool PresentationSurface_Holds(
    const SrPpuSurfaceView *surface, int width, int height) {
  surface = PresentationSurface_Bound(surface);
  return surface && width > 0 && height > 0 &&
      (uint32_t)width <= surface->width_pixels &&
      (uint32_t)height <= surface->height_pixels;
}

static inline const uint8_t *PresentationSurface_Region(
    const SrPpuSurfaceView *surface, int x, int y, int width, int height) {
  surface = PresentationSurface_Bound(surface);
  if (!surface || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      (uint64_t)(uint32_t)x + (uint32_t)width > surface->width_pixels ||
      (uint64_t)(uint32_t)y + (uint32_t)height > surface->height_pixels)
    return NULL;
  return surface->data + (size_t)y * (size_t)surface->pitch_bytes +
      (size_t)x * sizeof(uint32_t);
}

#endif
