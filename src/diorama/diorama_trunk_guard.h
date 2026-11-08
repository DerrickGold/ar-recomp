#ifndef AR_DIORAMA_TRUNK_GUARD_H
#define AR_DIORAMA_TRUNK_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ActionTrunkAssets;

/* Additional native source, not a larger display capture. The two strips
 * never overlap the primary rectangle and retain fixed atlas positions while
 * the camera turns. A finite source may make either strip partly or wholly
 * empty. Native/editor producers must agree on this layout. */
enum {
  kDioramaTrunkGuardWidth = 640,
  kDioramaTrunkGuardRows = 64,
  kDioramaTrunkGuardStrips = 2,
  kDioramaTrunkGuardHeight = 128,
  kDioramaTrunkGuardPixels = 640 * 128
};

typedef struct DioramaTrunkGuardBounds {
  int x, y, width, height;
} DioramaTrunkGuardBounds;

typedef struct DioramaTrunkGuardStrip {
  int world_x, world_y, width;
  int atlas_y;
  /* Half-open valid source rectangle relative to this strip. Empty is a
   * successful finite-source result, not permission to repeat other art. */
  DioramaTrunkGuardBounds valid;
} DioramaTrunkGuardStrip;

typedef struct DioramaTrunkGuardLayout {
  int primary_y, primary_height;
  DioramaTrunkGuardBounds source;
  DioramaTrunkGuardStrip strips[kDioramaTrunkGuardStrips];
} DioramaTrunkGuardLayout;

/* Coordinates describe the already-resolved native/editor source. Source
 * bounds may include explicit negative-position stamps; the producer remains
 * responsible for preserving genuine native transparency outside them. */
bool DioramaTrunkGuard_Resolve(int world_x, int primary_y, int capture_width,
    int primary_height, DioramaTrunkGuardBounds source,
    DioramaTrunkGuardLayout *out);

typedef struct DioramaTrunkGuardSource {
  DioramaTrunkGuardLayout layout;
  /* Complete native BG1 union, including priority-1 and authored masks.
   * Do not use the legacy low-only auxiliary skybox decoder for these pixels.
   * Borrowed with the same lifetime as the selected primary FrameSlot. */
  const uint32_t *pixels;
  size_t pitch_pixels, pixel_count;
  uint64_t capture_generation;
} DioramaTrunkGuardSource;

/* Output is three nonaliasing, caller-owned 640x128 atlases: low/high/far.
 * Geometry and native priority use the existing trunk split unchanged.
 * False leaves outputs untouched; the caller must not publish stale guards.
 * No interpolation policy is selected here: presenter integration must pair
 * guards with the exact selected primary frame and its motion decision. */
bool DioramaTrunkGuard_Split(const struct ActionTrunkAssets *assets,
    const DioramaTrunkGuardSource *source,
    uint32_t *const output[3], const size_t output_pixel_count[3],
    unsigned brightness);

#endif
