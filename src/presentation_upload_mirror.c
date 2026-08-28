#include "presentation_upload_mirror.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { kArgb8888BytesPerPixel = (int)sizeof(uint32_t) };

void PresentationUploadMirror_Reset(PresentationUploadMirror *mirror) {
  if (!mirror) return;
  free(mirror->pixels);
  *mirror = (PresentationUploadMirror){0};
}

static bool ValidRegion(const uint8_t *pixels, int pitch,
                        int width, int height) {
  return pixels && width > 0 && height > 0 &&
      width <= INT_MAX / kArgb8888BytesPerPixel &&
      pitch >= width * kArgb8888BytesPerPixel &&
      (size_t)pitch <= SIZE_MAX / (size_t)height;
}

bool PresentationUploadMirror_FindDirtyRect(
    const uint8_t *current, int current_pitch,
    const uint8_t *previous, int previous_pitch,
    int width, int height, ArRenderRectI *dirty) {
  if (!dirty) return false;
  *dirty = (ArRenderRectI){0, 0, width > 0 ? width : 0,
                          height > 0 ? height : 0};
  if (!ValidRegion(current, current_pitch, width, height) ||
      !ValidRegion(previous, previous_pitch, width, height))
    return width > 0 && height > 0;

  const size_t row_bytes =
      (size_t)width * (size_t)kArgb8888BytesPerPixel;
  int x0 = width;
  int y0 = height;
  int x1 = 0;
  int y1 = 0;
  for (int y = 0; y < height; y++) {
    const uint8_t *current_row = current + (size_t)y * (size_t)current_pitch;
    const uint8_t *previous_row =
        previous + (size_t)y * (size_t)previous_pitch;
    if (memcmp(current_row, previous_row, row_bytes) == 0)
      continue;

    size_t first_byte = 0;
    while (first_byte < row_bytes &&
           current_row[first_byte] == previous_row[first_byte])
      first_byte++;
    size_t last_byte = row_bytes;
    while (last_byte > first_byte &&
           current_row[last_byte - 1] == previous_row[last_byte - 1])
      last_byte--;
    const int row_x0 = (int)(first_byte / kArgb8888BytesPerPixel);
    const int row_x1 = (int)(
        (last_byte + (size_t)kArgb8888BytesPerPixel - 1u) /
        (size_t)kArgb8888BytesPerPixel);
    if (row_x0 < x0) x0 = row_x0;
    if (row_x1 > x1) x1 = row_x1;
    if (y < y0) y0 = y;
    y1 = y + 1;
  }
  if (x0 == width) {
    *dirty = (ArRenderRectI){0};
    return false;
  }
  *dirty = (ArRenderRectI){x0, y0, x1 - x0, y1 - y0};
  return true;
}

static bool EnsureStorage(PresentationUploadMirror *mirror,
                          int width, int height) {
  const size_t row_bytes =
      (size_t)width * (size_t)kArgb8888BytesPerPixel;
  if (row_bytes > SIZE_MAX / (size_t)height) return false;
  if (mirror->width != width || mirror->height != height) {
    free(mirror->pixels);
    mirror->pixels = NULL;
    mirror->width = width;
    mirror->height = height;
    mirror->valid = false;
  }
  if (!mirror->pixels)
    mirror->pixels = malloc(row_bytes * (size_t)height);
  return mirror->pixels != NULL;
}

bool PresentationUploadMirror_UploadArgb8888(
    PresentationUploadMirror *mirror, ArRenderDevice *device,
    ArRenderTexture texture,
    const uint8_t *source, int width, int height, int source_pitch,
    int destination_x, int destination_y, PresentationUploadResult *result) {
  if (result) *result = (PresentationUploadResult){0};
  if (!mirror || !ArRenderDevice_IsReady(device) ||
      !ArRenderTexture_IsValid(texture) ||
      !ValidRegion(source, source_pitch, width, height) ||
      destination_x < 0 || destination_y < 0 ||
      destination_x > INT_MAX - width || destination_y > INT_MAX - height)
    return false;

  if (!ArRenderTexture_Equals(mirror->texture, texture) ||
      mirror->destination_x != destination_x ||
      mirror->destination_y != destination_y) {
    mirror->texture = texture;
    mirror->destination_x = destination_x;
    mirror->destination_y = destination_y;
    mirror->valid = false;
  }

  const bool have_storage = EnsureStorage(mirror, width, height);
  ArRenderRectI dirty = {0, 0, width, height};
  const int mirror_pitch = width * kArgb8888BytesPerPixel;
  if (have_storage && mirror->valid &&
      !PresentationUploadMirror_FindDirtyRect(
          source, source_pitch, mirror->pixels, mirror_pitch,
          width, height, &dirty))
    return true;

  ArRenderRectI destination = {
    destination_x + dirty.x,
    destination_y + dirty.y,
    dirty.w,
    dirty.h,
  };
  const uint8_t *dirty_source =
      source + (size_t)dirty.y * (size_t)source_pitch +
      (size_t)dirty.x * (size_t)kArgb8888BytesPerPixel;
  if (result) {
    result->destination = destination;
    result->changed = true;
  }
  if (!ArRenderDevice_UpdateTexture(
          device, texture, &destination, dirty_source, source_pitch)) {
    mirror->valid = false;
    return false;
  }

  if (have_storage) {
    for (int y = dirty.y; y < dirty.y + dirty.h; y++) {
      memcpy(mirror->pixels + (size_t)y * (size_t)mirror_pitch +
                 (size_t)dirty.x * (size_t)kArgb8888BytesPerPixel,
             source + (size_t)y * (size_t)source_pitch +
                 (size_t)dirty.x * (size_t)kArgb8888BytesPerPixel,
             (size_t)dirty.w * (size_t)kArgb8888BytesPerPixel);
    }
    mirror->valid = true;
  }
  if (result) {
    result->uploaded_bytes =
        (uint64_t)dirty.w * (uint64_t)dirty.h *
        (uint64_t)kArgb8888BytesPerPixel;
  }
  return true;
}
