#include "action_camera_bounds.h"

#include <limits.h>

bool ActionCameraAxisBounds_Resolve(
    uint16_t world_extent, uint16_t viewport_extent,
    int requested_before, int requested_after,
    ActionCameraAxisBounds *bounds) {
  if (!bounds) return false;
  *bounds = (ActionCameraAxisBounds){ 0 };
  if (!viewport_extent || world_extent < viewport_extent ||
      requested_before < 0 || requested_after < 0)
    return false;

  const uint32_t native_maximum = world_extent - viewport_extent;
  const uint32_t requested_extent =
      (uint32_t)requested_before + viewport_extent +
      (uint32_t)requested_after;
  bounds->maximum = (uint16_t)native_maximum;

  if (requested_extent <= world_extent &&
      requested_before <= UINT16_MAX && requested_after <= UINT16_MAX) {
    bounds->minimum = (uint16_t)requested_before;
    bounds->maximum = (uint16_t)(native_maximum - requested_after);
    bounds->includes_requested_margins =
        requested_before != 0 || requested_after != 0;
  }
  return true;
}

uint16_t ActionCameraAxisBounds_Clamp(
    const ActionCameraAxisBounds *bounds, int32_t camera_origin) {
  if (!bounds) return 0;
  if (camera_origin <= bounds->minimum) return bounds->minimum;
  if (camera_origin >= bounds->maximum) return bounds->maximum;
  return (uint16_t)camera_origin;
}

static uint16_t MoveNative(
    uint16_t camera_origin, int16_t delta,
    uint16_t world_extent, uint16_t viewport_extent) {
  if (!delta) return camera_origin;
  const int32_t candidate = (int32_t)camera_origin + delta;
  if (candidate <= 0) return 0;

  /* Valid action maps are never smaller than their native viewport. Preserve
   * the ROM's unsigned subtraction during transient load state nevertheless:
   * a zero dimension produces a large maximum rather than forcing the camera
   * to zero before the room assets have installed their extent. */
  const uint16_t native_maximum =
      (uint16_t)(world_extent - viewport_extent);
  return candidate >= native_maximum
      ? native_maximum : (uint16_t)candidate;
}

uint16_t ActionCameraAxisBounds_UpdateCamera(
    uint16_t camera_origin, int16_t delta,
    uint16_t world_extent, uint16_t viewport_extent,
    int requested_before, int requested_after,
    ActionCameraAxisBounds *resolved_bounds) {
  const uint16_t native = MoveNative(
      camera_origin, delta, world_extent, viewport_extent);
  ActionCameraAxisBounds bounds;
  if (!ActionCameraAxisBounds_Resolve(
          world_extent, viewport_extent,
          requested_before, requested_after, &bounds)) {
    if (resolved_bounds)
      *resolved_bounds = (ActionCameraAxisBounds){ 0 };
    return native;
  }
  if (resolved_bounds) *resolved_bounds = bounds;

  /* The ROM only enters its clamp path when this axis has a non-zero delta.
   * Do not turn the native/fallback interval into an unconditional clamp:
   * scripts may position an otherwise stationary camera during transitions.
   * A fitted presentation margin is the sole intentional exception. */
  return bounds.includes_requested_margins
      ? ActionCameraAxisBounds_Clamp(&bounds, native) : native;
}

int16_t ActionCameraAxisBounds_EffectiveDelta(
    const ActionCameraAxisBounds *bounds, uint16_t camera_origin,
    uint16_t updated_camera, int16_t requested_delta) {
  if (!bounds || !bounds->includes_requested_margins)
    return requested_delta;
  if (camera_origin < bounds->minimum || camera_origin > bounds->maximum)
    return 0;
  return (int16_t)(updated_camera - camera_origin);
}
