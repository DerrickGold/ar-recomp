#include "action_camera_bounds.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do {                                                   \
  if (!(expr)) {                                                           \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    exit(1);                                                               \
  }                                                                        \
} while (0)

static ActionCameraAxisBounds Resolve(
    uint16_t world, uint16_t viewport, int before, int after) {
  ActionCameraAxisBounds bounds = { 0 };
  CHECK(ActionCameraAxisBounds_Resolve(
      world, viewport, before, after, &bounds));
  return bounds;
}

static void CheckBloodpoolRightEdge(void) {
  const ActionCameraAxisBounds bounds = Resolve(768, 256, 120, 120);
  CHECK(bounds.includes_requested_margins);
  CHECK(bounds.minimum == 120);
  CHECK(bounds.maximum == 392);
  CHECK(ActionCameraAxisBounds_Clamp(&bounds, 0) == 120);
  CHECK(ActionCameraAxisBounds_Clamp(&bounds, 256) == 256);
  CHECK(ActionCameraAxisBounds_Clamp(&bounds, 512) == 392);
}

static void CheckSmallestWideRoom(void) {
  const ActionCameraAxisBounds bounds = Resolve(512, 256, 120, 120);
  CHECK(bounds.includes_requested_margins);
  CHECK(bounds.minimum == 120);
  CHECK(bounds.maximum == 136);
}

static void CheckNarrowRoomFallsBackToNative(void) {
  const ActionCameraAxisBounds bounds = Resolve(256, 256, 120, 120);
  CHECK(!bounds.includes_requested_margins);
  CHECK(bounds.minimum == 0);
  CHECK(bounds.maximum == 0);
  CHECK(ActionCameraAxisBounds_Clamp(&bounds, 120) == 0);
}

static void CheckVerticalBoundsAndFallback(void) {
  ActionCameraAxisBounds bounds = Resolve(512, 225, 32, 32);
  CHECK(bounds.includes_requested_margins);
  CHECK(bounds.minimum == 32);
  CHECK(bounds.maximum == 255);

  bounds = Resolve(256, 225, 32, 32);
  CHECK(!bounds.includes_requested_margins);
  CHECK(bounds.minimum == 0);
  CHECK(bounds.maximum == 31);
}

static void CheckAsymmetricAndAuthenticBounds(void) {
  ActionCameraAxisBounds bounds = Resolve(768, 256, 76, 100);
  CHECK(bounds.includes_requested_margins);
  CHECK(bounds.minimum == 76);
  CHECK(bounds.maximum == 412);

  bounds = Resolve(768, 256, 0, 0);
  CHECK(!bounds.includes_requested_margins);
  CHECK(bounds.minimum == 0);
  CHECK(bounds.maximum == 512);
}

static void CheckTunedPlayfieldExtent(void) {
  const ActionCameraAxisBounds bounds = Resolve(512, 256, 16, 16);
  CHECK(bounds.includes_requested_margins);
  CHECK(bounds.minimum == 16);
  CHECK(bounds.maximum == 240);
}

static void CheckInvalidInput(void) {
  ActionCameraAxisBounds bounds = { 1, 2, true };
  CHECK(!ActionCameraAxisBounds_Resolve(224, 256, 0, 0, &bounds));
  CHECK(bounds.minimum == 0);
  CHECK(bounds.maximum == 0);
  CHECK(!bounds.includes_requested_margins);
  CHECK(!ActionCameraAxisBounds_Resolve(256, 0, 0, 0, &bounds));
  CHECK(!ActionCameraAxisBounds_Resolve(256, 256, -1, 0, &bounds));
  CHECK(!ActionCameraAxisBounds_Resolve(256, 256, 0, -1, &bounds));
  CHECK(!ActionCameraAxisBounds_Resolve(256, 256, 0, 0, NULL));
  CHECK(ActionCameraAxisBounds_Clamp(NULL, 100) == 0);
}

static void CheckNativeStationaryCameraIsUntouched(void) {
  ActionCameraAxisBounds bounds = { 0 };
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      600, 0, 768, 256, 0, 0, &bounds) == 600);
  CHECK(!bounds.includes_requested_margins);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      600, 1, 768, 256, 0, 0, &bounds) == 512);

  /* Requested margins do not fit, so this is the same native stationary
   * transition case rather than an unconditional clamp to the fallback. */
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      120, 0, 256, 256, 120, 120, &bounds) == 120);
  CHECK(!bounds.includes_requested_margins);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      120, 1, 256, 256, 120, 120, &bounds) == 0);
}

static void CheckCorrectedCameraClampsWithoutMotion(void) {
  ActionCameraAxisBounds bounds = { 0 };
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      0, 0, 768, 256, 120, 120, &bounds) == 120);
  CHECK(bounds.includes_requested_margins);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      512, 0, 768, 256, 120, 120, &bounds) == 392);
}

static void CheckPresentationDeltaReconciliation(void) {
  const ActionCameraAxisBounds bounds = Resolve(768, 256, 120, 120);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      &bounds, 120, 120, -120) == 0);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      &bounds, 121, 120, -2) == -1);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      &bounds, 120, 122, 2) == 2);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      &bounds, 0, 120, -120) == 0);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      &bounds, 512, 392, 120) == 0);

  ActionCameraAxisBounds native = Resolve(768, 256, 0, 0);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      &native, 0, 0, -120) == -120);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(
      NULL, 0, 0, -120) == -120);
}

int main(void) {
  CheckBloodpoolRightEdge();
  CheckSmallestWideRoom();
  CheckNarrowRoomFallsBackToNative();
  CheckVerticalBoundsAndFallback();
  CheckAsymmetricAndAuthenticBounds();
  CheckTunedPlayfieldExtent();
  CheckInvalidInput();
  CheckNativeStationaryCameraIsUntouched();
  CheckCorrectedCameraClampsWithoutMotion();
  CheckPresentationDeltaReconciliation();
  puts("action_camera_bounds_test: PASS");
  return 0;
}
