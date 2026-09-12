/* Minimal ArRenderDevice stubs so the REAL presentation_upload_mirror.c
 * translation unit links into the benchmark unchanged.
 *
 * The benchmark only exercises PresentationUploadMirror_FindDirtyRect, which
 * touches no device state. These stubs exist solely to satisfy the linker for
 * the sibling PresentationUploadMirror_UploadArgb8888 in the same TU. Linking
 * the real file (rather than copying the kernel) is deliberate: the benchmark
 * must calibrate the shipping scan, not a transcription of it that can drift.
 *
 * UpdateTexture counts calls and bytes instead of touching a GPU, so a future
 * upload-path benchmark can reuse these without a renderer. */

#include <stddef.h>
#include <stdint.h>

#include "render/render_device.h"

uint64_t g_stub_update_calls;
uint64_t g_stub_update_bytes;

bool ArRenderDevice_IsReady(const ArRenderDevice *device) {
  (void)device;
  return true;
}

bool ArRenderDevice_UpdateTexture(ArRenderDevice *device,
                                  ArRenderTexture texture,
                                  const ArRenderRectI *destination,
                                  const void *pixels, int pitch_bytes) {
  (void)device;
  (void)texture;
  (void)pixels;
  (void)pitch_bytes;
  g_stub_update_calls++;
  if (destination && destination->w > 0 && destination->h > 0)
    g_stub_update_bytes +=
        (uint64_t)destination->w * (uint64_t)destination->h * 4u;
  return true;
}
