#include "sim/sim3d/sim3d_camera.h"
#include "app/settings.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void Near(float actual, float expected) {
  assert(fabsf(actual - expected) < .00001f);
}

static Scene3DCamera Present(Sim3DCameraPresenter *presenter,
                              const Sim3DCameraFrame *frame,
                              uint64_t capture_ns, uint64_t now_ns) {
  Scene3DCamera camera = {.tilt_x = .7f, .tilt_y = -.1f, .distance = 3, .fov_y = .4f};
  Sim3DCamera_ApplyMotion(presenter, frame, capture_ns, now_ns, &camera);
  assert(camera.fov_y == .4f);
  return camera;
}

static void ObservationTimingAndTownEntry(void) {
  Sim3DCameraObserver observer = SIM3D_CAMERA_OBSERVER_INIT;
  Sim3DCameraObserver stepped = SIM3D_CAMERA_OBSERVER_INIT;
  Sim3DCameraObservation input = {.in_town = true, .velocity_x = 12, .velocity_y = -8, .hp = 9};
  const Sim3DCameraMotion batched = Sim3DCamera_Observe(&observer, &input, 12);
  Sim3DCameraMotion motion = {0};
  for (int i = 0; i < 12; i++) motion = Sim3DCamera_Observe(&stepped, &input, 1);
  assert(batched.lean_yaw == motion.lean_yaw && batched.lean_pitch == motion.lean_pitch);
  assert(observer.velocity_x_average == stepped.velocity_x_average);
  assert(observer.velocity_y_average == stepped.velocity_y_average);
  assert(!batched.event_hit && batched.lean_yaw > 0 && batched.lean_pitch < 0);
  const float average = observer.velocity_x_average;
  input.hp = 8;
  motion = Sim3DCamera_Observe(&observer, &input, 0);
  assert(motion.event_hit && observer.velocity_x_average == average);
  motion = Sim3DCamera_Observe(&observer, &input, 0);
  assert(!motion.event_hit);
  input.in_town = false;
  input.velocity_x = INT16_MAX;
  input.hp = 0;
  motion = Sim3DCamera_Observe(&observer, &input, 60);
  assert(motion.lean_yaw == 0 && motion.lean_pitch == 0 && !motion.event_hit);
  assert(observer.velocity_x_average == average && !observer.previous_in_town);
  input.in_town = true;
  input.hp = 2;
  input.velocity_x = 12;
  motion = Sim3DCamera_Observe(&observer, &input, 0);
  assert(!motion.event_hit && motion.lean_yaw == batched.lean_yaw);
  input.hp = 3;
  motion = Sim3DCamera_Observe(&observer, &input, 1);
  assert(!motion.event_hit); /* Healing cannot cause a damage jolt. */

  Sim3DCameraObserver independent = SIM3D_CAMERA_OBSERVER_INIT;
  input.velocity_x = INT16_MAX;
  input.velocity_y = INT16_MIN;
  motion = Sim3DCamera_Observe(&independent, &input, 0);
  assert(motion.lean_yaw == 1 && motion.lean_pitch == -1);
  assert(independent.velocity_x_average == 4);
}

static void ImpulsesAndRetainedControls(void) {
  Sim3DCameraPresenter presenter = SIM3D_CAMERA_PRESENTER_INIT;
  Sim3DCameraFrame frame = {.mode = kSimCam_Dynamic, .reactive_strength = 100,
      .motion = {.event_hit = true}};
  Scene3DCamera camera = Present(&presenter, &frame, 1, 1000000000);
  Near(camera.tilt_x, .73f);
  Near(camera.distance, 2.73f);
  unsigned char saved[sizeof(frame)];
  memcpy(saved, &frame, sizeof(frame));
  for (int i = 0; i < 4; i++) {
    camera = Present(&presenter, &frame, 1, 1000000000);
    Near(camera.tilt_x, .73f);
    Near(camera.distance, 2.73f);
  }
  camera = Present(&presenter, &frame, 2, 1000000000);
  Near(camera.tilt_x, .76f);
  Near(camera.distance, 2.46f);
  camera = Present(&presenter, &frame, 2, 1180000000);
  Near(camera.tilt_x, .7f + .06f * expf(-1));
  Near(camera.distance, 3 * (1 - .18f * expf(-1)));
  assert(!memcmp(saved, &frame, sizeof(frame)));
  frame.orbit_pitch = .1f;
  frame.orbit_yaw = -.2f;
  camera = Present(&presenter, &frame, 2, 1180000000);
  Near(camera.tilt_x, .8f + .06f * expf(-1));
  Near(camera.tilt_y, -.3f);
  frame.reactive_strength = 0;
  camera = Present(&presenter, &frame, 2, 1180000000);
  Near(camera.tilt_x, .8f);
  Near(camera.tilt_y, -.3f);
  Near(camera.distance, 3);
  frame.mode = kSimCam_Free;
  camera = Present(&presenter, &frame, 2, 1180000000);
  Near(camera.tilt_x, .7f);
  Near(camera.tilt_y, -.1f);
  assert(!presenter.active && presenter.last_slot_ns == 0);
  frame.mode = kSimCam_Dynamic;
  frame.reactive_strength = 100;
  camera = Present(&presenter, &frame, 2, 1180000000);
  Near(camera.tilt_x, .83f); /* Free mode clears the SIM capture history. */
  for (uint64_t capture = 3; capture < 12; capture++)
    camera = Present(&presenter, &frame, capture, 1180000000);
  Near(camera.distance, 2); /* Stacked hits respect the camera's near limit. */
}

static Scene3DCamera EaseAtRate(int rate) {
  Sim3DCameraPresenter presenter = SIM3D_CAMERA_PRESENTER_INIT;
  Sim3DCameraFrame frame = {.mode = kSimCam_Dynamic, .reactive_strength = 100};
  (void)Present(&presenter, &frame, 1, 1000000000);
  frame.motion.lean_yaw = 1;
  frame.motion.lean_pitch = -1;
  Scene3DCamera camera = {0};
  for (int i = 1; i <= rate; i++)
    camera = Present(&presenter, &frame, 2,
        UINT64_C(1000000000) + UINT64_C(1000000000) * i / rate);
  return camera;
}

static void EasingAndModeChanges(void) {
  const Scene3DCamera sixty = EaseAtRate(60), twice = EaseAtRate(120);
  Near(sixty.tilt_x, twice.tilt_x);
  Near(sixty.tilt_y, twice.tilt_y);
  assert(sixty.tilt_x > .645f && sixty.tilt_x < .646f);
  Sim3DCameraPresenter a = SIM3D_CAMERA_PRESENTER_INIT;
  Sim3DCameraPresenter b = SIM3D_CAMERA_PRESENTER_INIT;
  Sim3DCameraFrame frame = {.mode = kSimCam_Dynamic, .reactive_strength = 100};
  (void)Present(&a, &frame, 1, 1000000000);
  (void)Present(&b, &frame, 1, 1000000000);
  frame.motion.lean_yaw = 1;
  Scene3DCamera gap = Present(&a, &frame, 2, UINT64_C(11000000000));
  Scene3DCamera bounded = Present(&b, &frame, 2, 2000000000);
  Near(gap.tilt_y, bounded.tilt_y);
  frame.motion.lean_yaw = -1;
  gap = Present(&a, &frame, 3, UINT64_C(11000000000));
  Near(gap.tilt_y, -.145f); /* A zero-time SIM response snaps to its target. */
  frame.mode = kSimCam_Free;
  gap = Present(&a, &frame, 3, UINT64_C(11000000000));
  Near(gap.tilt_y, -.1f);
  frame.mode = kSimCam_Dynamic;
  frame.motion.lean_yaw = 1;
  gap = Present(&a, &frame, 3, UINT64_C(11010000000));
  Near(gap.tilt_y, -.055f); /* Re-enable starts at the current target, not stale lean. */
}

int main(void) {
  ObservationTimingAndTownEntry();
  ImpulsesAndRetainedControls();
  EasingAndModeChanges();
  puts("SIM camera motion: town transitions, capture timing, retained frames and response passed");
  return 0;
}
