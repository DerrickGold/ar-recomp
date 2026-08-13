#ifndef ACTION_CAMERA_BOUNDS_H
#define ACTION_CAMERA_BOUNDS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ActionCameraAxisBounds {
  uint16_t minimum;
  uint16_t maximum;
  bool includes_requested_margins;
} ActionCameraAxisBounds;

/* Resolve the camera-origin interval that keeps an authentic viewport plus
 * both requested margins inside one finite action-layer axis. If the finite
 * world cannot contain that complete view, preserve the native camera range;
 * the existing per-frame margin clamp will expose only the space available at
 * the current edge. */
bool ActionCameraAxisBounds_Resolve(
    uint16_t world_extent, uint16_t viewport_extent,
    int requested_before, int requested_after,
    ActionCameraAxisBounds *bounds);

uint16_t ActionCameraAxisBounds_Clamp(
    const ActionCameraAxisBounds *bounds, int32_t camera_origin);

/* Apply only the ROM's native camera motion/clamp and publish its unmodified
 * [0, world-viewport] interval for diagnostics. This API deliberately has no
 * presentation-margin parameters, making native gameplay axes structural
 * rather than a convention at each call site. */
uint16_t ActionCameraAxisBounds_UpdateNativeCamera(
    uint16_t camera_origin, int16_t delta,
    uint16_t world_extent, uint16_t viewport_extent,
    ActionCameraAxisBounds *resolved_bounds);

/* Apply one ROM-style camera delta, then add the presentation interval only
 * when the complete requested margins fit. Native/fallback zero-delta frames
 * deliberately retain the input origin, matching $02:B091 transition logic. */
uint16_t ActionCameraAxisBounds_UpdateCamera(
    uint16_t camera_origin, int16_t delta,
    uint16_t world_extent, uint16_t viewport_extent,
    int requested_before, int requested_after,
    ActionCameraAxisBounds *resolved_bounds);

/* Reconcile the ROM's requested delta with a fitted presentation interval.
 * Once the camera is inside that interval, downstream parallax and player
 * state must see only motion that actually occurred. An initial correction
 * from outside the interval is presentation setup, not player movement. */
int16_t ActionCameraAxisBounds_EffectiveDelta(
    const ActionCameraAxisBounds *bounds, uint16_t camera_origin,
    uint16_t updated_camera, int16_t requested_delta);

#endif
