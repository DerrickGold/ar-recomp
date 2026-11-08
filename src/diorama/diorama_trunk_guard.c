#include "diorama_trunk_guard.h"
#include "action/action_trunk.h"

#include <limits.h>
#include <string.h>

static bool RangesOverlap(const void *a, size_t a_bytes,
                          const void *b, size_t b_bytes) {
  const uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
  /* Reject overflowing ranges rather than relying on wrapped pointer sums. */
  if (a_bytes > UINTPTR_MAX - aa || b_bytes > UINTPTR_MAX - bb) return true;
  return aa < bb + b_bytes && bb < aa + a_bytes;
}

bool DioramaTrunkGuard_Split(const ActionTrunkAssets *assets,
    const DioramaTrunkGuardSource *source,
    uint32_t *const output[3], const size_t output_pixel_count[3],
    unsigned brightness) {
  if (!assets || !assets->height || assets->height > kActionTrunkMaxHeight ||
      assets->side_width > kActionTrunkSideWidth || !source || !source->pixels ||
      !output || !output_pixel_count || brightness > 15 ||
      source->pitch_pixels < (size_t)source->layout.strips[0].width ||
      source->pitch_pixels > kDioramaTrunkGuardWidth ||
      source->pixel_count < source->pitch_pixels * kDioramaTrunkGuardHeight)
    return false;
  DioramaTrunkGuardLayout expected;
  if (!DioramaTrunkGuard_Resolve(source->layout.strips[0].world_x,
      source->layout.primary_y, source->layout.strips[0].width,
      source->layout.primary_height, source->layout.source, &expected) ||
      memcmp(&expected, &source->layout, sizeof(expected))) return false;
  const size_t output_bytes = kDioramaTrunkGuardPixels * sizeof(uint32_t);
  const size_t input_bytes = source->pitch_pixels * kDioramaTrunkGuardHeight * sizeof(uint32_t);
  for (unsigned i = 0; i < 3; ++i) {
    if (!output[i] || output_pixel_count[i] < kDioramaTrunkGuardPixels ||
        RangesOverlap(source->pixels, input_bytes, output[i], output_bytes)) return false;
    for (unsigned j = 0; j < i; ++j)
      if (RangesOverlap(output[j], output_bytes, output[i], output_bytes)) return false;
  }
  for (unsigned i = 0; i < kDioramaTrunkGuardStrips; ++i) {
    const DioramaTrunkGuardStrip *strip = &source->layout.strips[i];
    const uint32_t *input[3] = {
      source->pixels + strip->atlas_y * source->pitch_pixels, NULL, NULL
    };
    const size_t input_pitch[3] = {source->pitch_pixels, 0, 0};
    uint32_t *part[3];
    for (unsigned band = 0; band < 3; ++band)
      part[band] = output[band] + strip->atlas_y * kDioramaTrunkGuardWidth;
    if (!strip->valid.width || !strip->valid.height) {
      for (unsigned band = 0; band < 3; ++band)
        memset(part[band], 0, kDioramaTrunkGuardWidth *
            kDioramaTrunkGuardRows * sizeof(uint32_t));
      continue;
    }
    ActionTrunk_Split(assets, strip->world_x, strip->world_y, strip->width,
        kDioramaTrunkGuardRows, kDioramaTrunkGuardWidth,
        input, input_pitch, part, brightness);
    /* A reconstructed core wall is valid only inside the caller's finite
     * source. Clear all bands, including its synthetic Far copy, at the same
     * boundaries, rather than extending it into an empty guard. */
    for (unsigned band = 0; band < 3; ++band)
      for (int y = 0; y < kDioramaTrunkGuardRows; ++y) {
        uint32_t *row = part[band] + y * kDioramaTrunkGuardWidth;
        if (!strip->valid.width || y < strip->valid.y ||
            y >= strip->valid.y + strip->valid.height) {
          memset(row, 0, kDioramaTrunkGuardWidth * sizeof(*row));
        } else {
          memset(row, 0, (size_t)strip->valid.x * sizeof(*row));
          const int end = strip->valid.x + strip->valid.width;
          memset(row + end, 0, (size_t)(kDioramaTrunkGuardWidth - end) * sizeof(*row));
        }
      }
  }
  return true;
}
