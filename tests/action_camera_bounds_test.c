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

static void CheckAuthoredSceneryRelaxesCameraStop(void) {
  int before = 120, after = 120;
  ActionCameraAxisBounds bounds;
  /* Fillmore 0104: six added columns on the left of a 512px arena. The
   * old 120..136 interval blocked all leftward travel from the snapshot. */
  ActionCameraAxisBounds_ApplySceneryPadding(512, -96, 608, &before, &after);
  CHECK(before == 24 && after == 120);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      120, 0, 512, 256, before, after, &bounds) == 120);
  CHECK(bounds.minimum == 24 && bounds.maximum == 136);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      120, -16, 512, 256, before, after, &bounds) == 104);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      30, -16, 512, 256, before, after, &bounds) == 24);
  CHECK(ActionCameraAxisBounds_EffectiveDelta(&bounds, 30, 24, -16) == -6);

  before = after = 120;
  ActionCameraAxisBounds_ApplySceneryPadding(512, 0, 576, &before, &after);
  CHECK(before == 120 && after == 56);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      136, 32, 512, 256, before, after, &bounds) == 168);
  CHECK(bounds.minimum == 120 && bounds.maximum == 200);

  /* Large extensions can restore the whole native interval, never create
   * gameplay camera positions outside it (including a one-screen arena). */
  before = after = 120;
  ActionCameraAxisBounds_ApplySceneryPadding(512, -256, 1024, &before, &after);
  CHECK(before == 0 && after == 0);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      0, -16, 512, 256, before, after, &bounds) == 0);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      250, 16, 512, 256, before, after, &bounds) == 256);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      0, 16, 256, 256, before, after, &bounds) == 0);

  before = after = 120;
  ActionCameraAxisBounds_ApplySceneryPadding(512, 0, 512, &before, &after);
  CHECK(before == 120 && after == 120);
  ActionCameraAxisBounds_ApplySceneryPadding(512, -96, 0, &before, &after);
  CHECK(before == 120 && after == 120);
  ActionCameraAxisBounds_ApplySceneryPadding(512, -96, 512, &before, &after);
  CHECK(before == 120 && after == 120);
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

  /* The native API preserves zero-delta transition state even before valid
   * dimensions arrive, while still clearing diagnostic bounds. */
  bounds = (ActionCameraAxisBounds){ 1, 2, true };
  CHECK(ActionCameraAxisBounds_UpdateNativeCamera(
      120, 0, 0, 225, &bounds) == 120);
  CHECK(bounds.minimum == 0);
  CHECK(bounds.maximum == 0);
  CHECK(!bounds.includes_requested_margins);
  CHECK(ActionCameraAxisBounds_UpdateNativeCamera(
      120, 0, 0, 225, NULL) == 120);
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

static void CheckVerticalCaptureDoesNotFitGameplayCamera(void) {
  ActionCameraAxisBounds bounds = { 0 };

  /* A 512px-tall action room has native vertical range 0..287. Diorama may
   * capture up to 32 real rows around that camera, but presentation must not
   * shrink the gameplay-camera range to 32..255. At Bloodpool's floor this is
   * the difference between the moving logs being at native screen y=193 and
   * being drawn but inactive just beyond line 224. */
  CHECK(ActionCameraAxisBounds_UpdateNativeCamera(
      287, 0, 512, 225, &bounds) == 287);
  CHECK(!bounds.includes_requested_margins);
  CHECK(bounds.minimum == 0);
  CHECK(bounds.maximum == 287);

  /* If an older savestate contains the retired floor clamp, the next native
   * tracking request can reach the real floor instead of being blocked at
   * 255 again. */
  CHECK(ActionCameraAxisBounds_UpdateNativeCamera(
      255, 32, 512, 225, &bounds) == 287);

  /* Keep the old generic fitted result explicit: production Y uses the
   * native-only API above and therefore cannot accept this retired budget. */
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      287, 0, 512, 225, 32, 32, &bounds) == 255);
  CHECK(bounds.includes_requested_margins);
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
  CheckAuthoredSceneryRelaxesCameraStop();
  CheckNarrowRoomFallsBackToNative();
  CheckVerticalBoundsAndFallback();
  CheckAsymmetricAndAuthenticBounds();
  CheckTunedPlayfieldExtent();
  CheckInvalidInput();
  CheckNativeStationaryCameraIsUntouched();
  CheckCorrectedCameraClampsWithoutMotion();
  CheckVerticalCaptureDoesNotFitGameplayCamera();
  CheckPresentationDeltaReconciliation();
  puts("action_camera_bounds_test: PASS");
  return 0;
}
