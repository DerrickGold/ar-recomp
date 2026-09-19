#include "presentation_upload_mirror.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "performance_metrics.h"

enum { kArgb8888BytesPerPixel = (int)sizeof(uint32_t) };

/* Byte pointers can have arbitrary alignment and pitch. Let memcmp use the
 * platform's block implementation without introducing aliasing/alignment or
 * CPU-instruction requirements into this portable upload policy. */
static size_t EqualPrefix(const uint8_t *a, const uint8_t *b, size_t bytes, uint64_t *scanned) {
  size_t at = 0;
  while (bytes - at >= 32) {
    *scanned += 64;
    if (memcmp(a + at, b + at, 32)) break;
    at += 32;
  }
  while (at < bytes) {
    *scanned += 2;
    if (a[at] != b[at]) break;
    at++;
  }
  return at;
}

static size_t EqualSuffixStart(const uint8_t *a, const uint8_t *b,
                               size_t first, size_t end, uint64_t *scanned) {
  while (end - first >= 32) {
    *scanned += 64;
    if (memcmp(a + end - 32, b + end - 32, 32)) break;
    end -= 32;
  }
  while (end > first) {
    *scanned += 2;
    if (a[end - 1] != b[end - 1]) break;
    end--;
  }
  return end;
}

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
  /* Count BOTH comparison operands, including edge refinements, but not rows
   * skipped by the exact-bounds shortcut. This is requested comparison volume,
   * not DRAM traffic: libc may exit early and the CPU may reuse cached lines. */
  uint64_t scanned = 0;
  int x0 = width, y0 = height, x1 = 0, y1 = 0;
  for (int y = 0; y < height; y++) {
    const uint8_t *current_row = current + (size_t)y * (size_t)current_pitch;
    const uint8_t *previous_row =
        previous + (size_t)y * (size_t)previous_pitch;
    scanned += (uint64_t)row_bytes * 2;
    if (!memcmp(current_row,previous_row,row_bytes)) continue;
    /* Only pixels outside the accumulated horizontal bounds can expand the
     * rectangle. Full horizontal coverage ends the loop entirely. */
    if (x0) {
      const size_t first_byte = EqualPrefix(current_row, previous_row,
          (size_t)x0 * kArgb8888BytesPerPixel, &scanned);
      x0 = (int)(first_byte / kArgb8888BytesPerPixel);
    }
    if (x1 < width) {
      const size_t last_byte = EqualSuffixStart(current_row, previous_row,
          (size_t)x1 * kArgb8888BytesPerPixel, row_bytes, &scanned);
      x1 = (int)((last_byte + kArgb8888BytesPerPixel - 1u) / kArgb8888BytesPerPixel);
    }
    if (y < y0) y0 = y;
    y1 = y + 1;
    if (!x0 && x1 == width) {
      /* Horizontal bounds are final. Only the bottom dirty row can enlarge
       * this rectangle now; don't compare the already enclosed interior.
       * Keep the forward scan for narrow/sparse changes: reversing all clean
       * suffixes unconditionally harms streaming locality on those images. */
      int bottom = height;
      while (bottom > y1) {
        scanned += (uint64_t)row_bytes * 2;
        if (memcmp(current + (size_t)(bottom - 1) * current_pitch,
            previous + (size_t)(bottom - 1) * previous_pitch,row_bytes)) break;
        --bottom;
      }
      y1 = bottom;
      break;
    }
  }
  PerformanceMetrics_Add(kPerformanceCount_ScanBytes,scanned);
  if (x0 == width) {
    *dirty = (ArRenderRectI){0};
    return false;
  }
  *dirty = (ArRenderRectI){x0, y0, x1 - x0, y1 - y0};
  return true;
}

/* Grow-only: a changed extent invalidates the CONTENTS but keeps the
 * allocation when it already fits. Freeing and reallocating made every
 * dimension change cost a full reallocation and a complete re-upload on every
 * mirror at once, which is a frame-time spike rather than a steady cost.
 * Capacity is tracked separately from extent so a shrink cannot later read
 * beyond what was allocated. */
static bool EnsureStorage(PresentationUploadMirror *mirror,
                          int width, int height) {
  const size_t row_bytes =
      (size_t)width * (size_t)kArgb8888BytesPerPixel;
  if (row_bytes > SIZE_MAX / (size_t)height) return false;
  const size_t needed = row_bytes * (size_t)height;
  if (mirror->width != width || mirror->height != height) {
    mirror->width = width;
    mirror->height = height;
    mirror->valid = false;
  }
  if (!mirror->pixels || mirror->capacity_bytes < needed) {
    free(mirror->pixels);
    mirror->pixels = malloc(needed);
    mirror->capacity_bytes = mirror->pixels ? needed : 0;
    mirror->valid = false;
    if (mirror->pixels)
      PerformanceMetrics_Add(kPerformanceCount_MirrorReallocs, 1);
  }
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
          width, height, &dirty)) {
    PerformanceMetrics_Add(kPerformanceCount_UploadSkipped, 1);
    return true;
  }

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
