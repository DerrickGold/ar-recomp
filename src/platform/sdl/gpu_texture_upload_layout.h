#ifndef AR_SDL_GPU_TEXTURE_UPLOAD_LAYOUT_H
#define AR_SDL_GPU_TEXTURE_UPLOAD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

typedef struct ArSdlTextureUploadLayout {
  uint32_t offset;
  uint32_t row_pitch;
} ArSdlTextureUploadLayout;

/* D3D12 RGBA8 uploads use 256-byte rows and 512-byte offsets to avoid SDL's
 * temporary realignment buffers on older/integrated hardware. Other backends
 * retain tight packing: SDL 3.4's Metal upload path ignores pixels_per_row.
 * The cursor measures staging capacity, including padding between regions;
 * performance counters separately measure the requested texel payload. */
static inline bool ArSdlTextureUploadLayout_Append(
    int width, int height, bool align_for_d3d12,
    uint32_t *cursor, ArSdlTextureUploadLayout *out) {
  if (width <= 0 || height <= 0 || !cursor || !out) return false;
  const uint64_t row_bytes = (uint64_t)width * 4;
  const uint64_t row_pitch = align_for_d3d12
      ? (row_bytes + 255) & ~UINT64_C(255) : row_bytes;
  const uint64_t offset = align_for_d3d12
      ? ((uint64_t)*cursor + 511) & ~UINT64_C(511) : *cursor;
  const uint64_t end = offset + row_pitch * (uint64_t)height;
  /* SDL_ConvertPixels takes an int pitch; transfer buffers take Uint32 sizes. */
  if (row_pitch > INT_MAX || end > UINT32_MAX) return false;
  *out = (ArSdlTextureUploadLayout){(uint32_t)offset, (uint32_t)row_pitch};
  *cursor = (uint32_t)end;
  return true;
}

#endif
