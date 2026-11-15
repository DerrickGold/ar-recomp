#ifndef AR_DIORAMA_TERRAIN_SOURCE_H
#define AR_DIORAMA_TERRAIN_SOURCE_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include "render/render_types.h"

/* Published once, then shared by the producer, queued slots and retained
 * presentation. The lanes contain native low, high and far BG1 pixels. */
typedef struct DioramaTerrainSource {
  atomic_uint references;
  ArRenderRectI bounds;
  uint64_t geometry_revision;
  /* If nonzero, the intersection with this capture was copied verbatim. */
  uint64_t serial, overlap_serial;
  uint16_t fixed_color;
  uint8_t brightness, cgwsel, cgadsub;
  uint32_t pixels[];
} DioramaTerrainSource;

/* Margins use native pixels relative to the moving primary capture. Only
 * the latest presentation request matters, including on retained frames. */
typedef struct DioramaTerrainMargins {
  unsigned left, right, top, bottom;
} DioramaTerrainMargins;
void DioramaTerrain_Request(unsigned room, DioramaTerrainMargins margins);
DioramaTerrainMargins DioramaTerrain_Requested(unsigned room);

static inline const DioramaTerrainSource *DioramaTerrainSource_Retain(
    const DioramaTerrainSource *source) {
  if (source) atomic_fetch_add_explicit(&((DioramaTerrainSource *)source)->references,
      1, memory_order_relaxed);
  return source;
}

static inline void DioramaTerrainSource_Release(const DioramaTerrainSource *source) {
  if (source && atomic_fetch_sub_explicit(&((DioramaTerrainSource *)source)->references,
      1, memory_order_acq_rel) == 1)
    free((void *)source);
}
#endif
