#include "diorama/diorama_camera.h"

#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app/settings.h"

static void Near(float actual, float expected) {
  assert(fabsf(actual - expected) < 0.00001f);
}

static DioramaCameraFrame Frame(int mode) {
  return (DioramaCameraFrame){
    .controls = {
      .mode = mode,
      .free_pose = {.tilt_x = -.3f, .tilt_y = .4f, .distance = 5.0f},
      .dynamic_baseline = {.tilt_x = .2f, .tilt_y = 0.0f, .distance = 0.0f},
    },
    .reactive_strength = 100,
  };
}

static void CheckCaptureTimingAndIsolation(void) {
  DioramaCameraObserver batched = DIORAMA_CAMERA_OBSERVER_INIT;
  DioramaCameraObserver stepped = DIORAMA_CAMERA_OBSERVER_INIT;
  const DioramaCameraObservation running = {.velocity_x = 12, .velocity_y = -8, .hp = 8};
  DioramaCameraMotion batch = DioramaCamera_Observe(&batched, &running, 12);
  DioramaCameraMotion step = {0};
  for (int i = 0; i < 12; i++) step = DioramaCamera_Observe(&stepped, &running, 1);
  assert(batch.lean_yaw == step.lean_yaw && batch.lean_pitch == step.lean_pitch);
  assert(batched.velocity_x_average == stepped.velocity_x_average);
  assert(batched.velocity_y_average == stepped.velocity_y_average);
  assert(batch.lean_yaw > 0 && batch.lean_pitch < 0);
  const float average = batched.velocity_x_average;
  DioramaCameraMotion retained = DioramaCamera_Observe(&batched, &running, 0);
  assert(retained.lean_yaw == batch.lean_yaw && batched.velocity_x_average == average);
  assert(!retained.event_hit && !retained.event_land && !retained.event_boost);

  /* A separate view/session must not inherit another observer's calibration. */
  DioramaCameraObserver fresh = DIORAMA_CAMERA_OBSERVER_INIT;
  const DioramaCameraObservation extremes = {.velocity_x = INT16_MAX, .velocity_y = INT16_MIN};
  DioramaCameraMotion clamped = DioramaCamera_Observe(&fresh, &extremes, 0);
  assert(clamped.lean_yaw == 1.0f && clamped.lean_pitch == -1.0f);
  assert(fresh.velocity_x_average == 4.0f && fresh.velocity_y_average == 4.0f);
}

static void CheckCapturedEdges(void) {
  DioramaCameraObserver observer = DIORAMA_CAMERA_OBSERVER_INIT;
  DioramaCameraObservation input = {.velocity_y = 80, .hp = 10};
  DioramaCameraMotion motion = DioramaCamera_Observe(&observer, &input, 60);
  assert(!motion.event_hit && !motion.event_land && !motion.event_boost);
  input.velocity_y = 0;
  input.hp = 9;
  input.boost = true;
  motion = DioramaCamera_Observe(&observer, &input, 1);
  assert(motion.event_hit && motion.event_land && motion.event_boost);
  motion = DioramaCamera_Observe(&observer, &input, 0);
  assert(!motion.event_hit && !motion.event_land && !motion.event_boost);
  input.hp = 10;
  input.boost = false;
  motion = DioramaCamera_Observe(&observer, &input, 1);
  assert(!motion.event_hit && !motion.event_boost);
  input.boost = true;
  motion = DioramaCamera_Observe(&observer, &input, 1);
  assert(motion.event_boost && !motion.event_hit);
}

static void CheckFreeControlAndModeChanges(void) {
  DioramaCameraPresenter presenter = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraFrame frame = Frame(kDioramaCam_Free);
  frame.motion = (DioramaCameraMotion){.lean_yaw = 1, .lean_pitch = 1,
      .event_hit = true, .event_land = true, .event_boost = true};
  frame.controls.orbit_pitch = .1f;
  frame.controls.orbit_yaw = -.2f;
  frame.controls.zoom_offset = 2.0f;
  frame.controls.framing_override = 1.0f;
  const DioramaCameraFrame original = frame;
  DioramaCameraView view = DioramaCamera_Present(&presenter, &frame, 10, 1000000000);
  Near(view.pose.tilt_x, -.3f);
  Near(view.pose.tilt_y, .4f);
  Near(view.pose.distance, 5.0f);
  Near(view.distance_scale, 1.0f);
  Near(view.distance_offset, 0.0f);
  Near(view.framing_weight, 0.0f);
  assert(!memcmp(&original, &frame, sizeof(frame)));
  frame.controls.free_pose.tilt_y = -.4f;
  view = DioramaCamera_Present(&presenter, &frame, 10, 1000000000);
  Near(view.pose.tilt_y, -.4f); /* A retained frame follows manual controls immediately. */

  frame.controls.mode = kDioramaCam_Dynamic;
  view = DioramaCamera_Present(&presenter, &frame, 10, 1000000000);
  Near(view.pose.tilt_x, .2f + .12f + .1f);
  Near(view.pose.tilt_y, .1f - .2f);
  Near(view.pose.distance, 0);
  Near(view.distance_scale, 1); /* A mode switch cannot replay this capture's hit. */
  Near(view.distance_offset, 2.0f);
  Near(view.framing_weight, 0.0f);
  frame.controls.mode = kDioramaCam_Free;
  view = DioramaCamera_Present(&presenter, &frame, 11, 1000000000);
  Near(view.pose.tilt_x, -.3f);
  Near(view.pose.tilt_y, -.4f);
  Near(view.distance_offset, 0.0f);
  Near(view.framing_weight, 0.0f);
}

static DioramaCameraManualState ManualReturnAtRate(int rate) {
  DioramaCameraManualState manual = {
    .offset = {.tilt_x = .2f, .tilt_y = -.3f, .distance = 2.0f},
  };
  DioramaCameraManual_Input(&manual);
  (void)DioramaCameraManual_Update(&manual, .01f, false);
  for (int i = 0; i < rate; i++)
    (void)DioramaCameraManual_Update(&manual, 1.1f / (float)rate, false);
  return manual;
}

static void CheckManualReturn(void) {
  /* Zoom alone releases framing, and sustained zoom cannot decay mid-input. */
  DioramaCameraManualState manual = {.offset = {.distance = 2.0f}};
  DioramaCameraManual_Input(&manual);
  assert(DioramaCameraManual_Update(&manual, .01f, false));
  for (int i = 0; i < 10; i++)
    assert(DioramaCameraManual_Update(&manual, .1f, true));
  Near(manual.offset.distance, 2.0f);
  Near(manual.framing_override, 1.0f);
  assert(!DioramaCameraManual_Update(&manual, .3f, false));
  Near(manual.offset.distance, 2.0f);
  assert(DioramaCameraManual_Update(&manual, .275f, false));
  Near(manual.offset.distance, 2.0f * expf(-.5f));
  Near(manual.framing_override, expf(-.5f));

  /* A new wheel/drag event also restarts the grace period without a held key. */
  const float zoom = manual.offset.distance;
  DioramaCameraManual_Input(&manual);
  assert(DioramaCameraManual_Update(&manual, .1f, false));
  Near(manual.offset.distance, zoom);
  Near(manual.framing_override, 1.0f);
  assert(!DioramaCameraManual_Update(&manual, .3f, false));
  Near(manual.offset.distance, zoom);

  const DioramaCameraManualState sixty = ManualReturnAtRate(60);
  const DioramaCameraManualState twice = ManualReturnAtRate(120);
  Near(sixty.offset.tilt_x, twice.offset.tilt_x);
  Near(sixty.offset.tilt_y, twice.offset.tilt_y);
  Near(sixty.offset.distance, twice.offset.distance);
  Near(sixty.framing_override, twice.framing_override);
  Near(sixty.framing_override, expf(-2.0f));
  for (int i = 0; i < 50; i++)
    (void)DioramaCameraManual_Update(&manual, .1f, false);
  Near(manual.offset.tilt_x, 0.0f);
  Near(manual.offset.tilt_y, 0.0f);
  Near(manual.offset.distance, 0.0f);
  Near(manual.framing_override, 0.0f);
  assert(!DioramaCameraManual_Update(&manual, .1f, false));
}

static void CheckManualPresentation(void) {
  DioramaCameraPresenter presenter = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraFrame frame = Frame(kDioramaCam_Dynamic);
  frame.controls.orbit_pitch = -.1f;
  frame.controls.orbit_yaw = .3f;
  frame.controls.zoom_offset = -1.0f;
  frame.controls.framing_override = 1.0f;
  const DioramaCameraFrame original = frame;
  DioramaCameraView view = DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  Near(view.pose.tilt_x, .1f);
  Near(view.pose.tilt_y, .3f);
  Near(view.pose.distance, 0.0f); /* Manual zoom preserves the auto-fit default. */
  Near(view.distance_offset, -1.0f);
  Near(view.framing_weight, 0.0f);
  assert(!memcmp(&original, &frame, sizeof(frame)));

  /* Retained frames restore pose and framing together without a mode switch. */
  frame.controls.orbit_pitch *= .5f;
  frame.controls.orbit_yaw *= .5f;
  frame.controls.zoom_offset *= .5f;
  frame.controls.framing_override = .5f;
  view = DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  Near(view.pose.tilt_x, .15f);
  Near(view.pose.tilt_y, .15f);
  Near(view.distance_offset, -.5f);
  Near(view.framing_weight, .5f);
  frame.controls.orbit_pitch = 0.0f;
  frame.controls.orbit_yaw = 0.0f;
  frame.controls.zoom_offset = 0.0f;
  frame.controls.framing_override = 0.0f;
  view = DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  Near(view.pose.tilt_x, .2f);
  Near(view.pose.tilt_y, 0.0f);
  Near(view.pose.distance, 0.0f);
  Near(view.distance_offset, 0.0f);
  Near(view.framing_weight, 1.0f);
}

static DioramaCameraView EaseAtRate(int rate) {
  DioramaCameraPresenter presenter = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraFrame frame = Frame(kDioramaCam_Dynamic);
  (void)DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  frame.motion.lean_yaw = 1;
  frame.motion.lean_pitch = -1;
  DioramaCameraView view = {0};
  for (int i = 1; i <= rate; i++)
    view = DioramaCamera_Present(&presenter, &frame, 2,
        UINT64_C(1000000000) + (UINT64_C(1000000000) / rate) * i);
  return view;
}

static void CheckTimeBasedEasing(void) {
  const DioramaCameraView sixty = EaseAtRate(60), twice = EaseAtRate(120);
  Near(sixty.pose.tilt_x, twice.pose.tilt_x);
  Near(sixty.pose.tilt_y, twice.pose.tilt_y);
  assert(sixty.pose.tilt_x > .08f && sixty.pose.tilt_x < .081f);
  assert(sixty.pose.tilt_y > .099f && sixty.pose.tilt_y < .1f);

  DioramaCameraPresenter a = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraPresenter b = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraFrame frame = Frame(kDioramaCam_Dynamic);
  (void)DioramaCamera_Present(&a, &frame, 1, 1000000000);
  (void)DioramaCamera_Present(&b, &frame, 1, 1000000000);
  frame.motion.lean_yaw = 1;
  const DioramaCameraView stopped = DioramaCamera_Present(&a, &frame, 2, 1000000000);
  Near(stopped.pose.tilt_y, 0);
  const DioramaCameraView gap = DioramaCamera_Present(&a, &frame, 2, UINT64_C(11000000000));
  const DioramaCameraView bounded = DioramaCamera_Present(&b, &frame, 2, 2000000000);
  Near(gap.pose.tilt_y, bounded.pose.tilt_y); /* Long stalls contribute at most one second. */
}

static void CheckImpulseLifetime(void) {
  DioramaCameraPresenter presenter = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraFrame frame = Frame(kDioramaCam_Dynamic);
  frame.motion.event_hit = frame.motion.event_land = true;
  const DioramaCameraFrame original = frame;
  DioramaCameraView view = DioramaCamera_Present(&presenter, &frame, 10, 1000000000);
  Near(view.pose.tilt_x, .25f); /* A simultaneous landing and hit produce one pitch kick. */
  Near(view.pose.distance, 0); /* Auto-fit sentinel survives; zoom follows fitting. */
  Near(view.distance_scale, .85f);
  for (int i = 0; i < 5; i++) {
    view = DioramaCamera_Present(&presenter, &frame, 10, 1000000000);
    Near(view.pose.tilt_x, .25f);
    Near(view.distance_scale, .85f);
  }
  view = DioramaCamera_Present(&presenter, &frame, 11, 1000000000);
  Near(view.pose.tilt_x, .30f); /* A new capture can stack an impulse. */
  Near(view.distance_scale, .70f);
  view = DioramaCamera_Present(&presenter, &frame, 11, 1200000000);
  Near(view.pose.tilt_x, .2f + .1f * expf(-1));
  Near(view.distance_scale, 1 - .3f * expf(-1));
  assert(!memcmp(&original, &frame, sizeof(frame)));

  frame.controls.mode = kDioramaCam_Free;
  (void)DioramaCamera_Present(&presenter, &frame, 11, 1200000000);
  frame.controls.mode = kDioramaCam_Dynamic;
  view = DioramaCamera_Present(&presenter, &frame, 11, 1200000000);
  Near(view.pose.tilt_x, .2f);
  Near(view.distance_scale, 1);
}

static void CheckLandingBoostAndStrength(void) {
  DioramaCameraPresenter presenter = DIORAMA_CAMERA_PRESENTER_INIT;
  DioramaCameraFrame frame = Frame(kDioramaCam_Dynamic);
  frame.motion.event_land = true;
  DioramaCameraView view = DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  Near(view.pose.tilt_x, .25f);
  Near(view.distance_scale, 1);

  presenter = (DioramaCameraPresenter)DIORAMA_CAMERA_PRESENTER_INIT;
  frame.motion.event_land = false;
  frame.motion.event_boost = true;
  view = DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  Near(view.pose.tilt_x, .2f);
  Near(view.distance_scale, 1); /* Boost is logged but never drives the camera. */

  presenter = (DioramaCameraPresenter)DIORAMA_CAMERA_PRESENTER_INIT;
  frame.motion.event_hit = true;
  frame.motion.lean_yaw = 1;
  frame.reactive_strength = 0;
  view = DioramaCamera_Present(&presenter, &frame, 1, 1000000000);
  Near(view.pose.tilt_x, .2f);
  Near(view.pose.tilt_y, 0);
  Near(view.distance_scale, 1);
}

int main(void) {
  CheckCaptureTimingAndIsolation();
  CheckCapturedEdges();
  CheckFreeControlAndModeChanges();
  CheckManualReturn();
  CheckManualPresentation();
  CheckTimeBasedEasing();
  CheckImpulseLifetime();
  CheckLandingBoostAndStrength();
  puts("diorama camera: capture edges, clock domains, retained frames and mode changes passed");
  return 0;
}
