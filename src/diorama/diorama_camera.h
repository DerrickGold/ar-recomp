#ifndef AR_DIORAMA_CAMERA_H
#define AR_DIORAMA_CAMERA_H
/* Reactive action-camera behavior, from captured player motion to the pose
 * drawn on screen. Calculations use explicit inputs and separate capture and
 * presentation histories; this module never reads live game state.
 * Host settings/manual controls are captured by diorama.c; diorama_host.c
 * supplies WRAM observations. Tests: tests/diorama_camera_test.c. */

#include <stdbool.h>
#include <stdint.h>

/* Distance <= 0 requests scene auto-fit. Hit zoom is applied separately after
 * that fit; multiplying a zero distance here would lose the zoom impulse. */
typedef struct DioramaCameraPose {
  float tilt_x;
  float tilt_y;
  float distance;
} DioramaCameraPose;

/* Transient host input, independent of the saved camera defaults. Distance is
 * an additive offset applied after auto-fit; framing returns with the pose. */
typedef struct DioramaCameraManualState {
  DioramaCameraPose offset;
  float framing_override;
  float idle_remaining;
  bool input_pending;
} DioramaCameraManualState;

void DioramaCameraManual_Input(DioramaCameraManualState *manual);
bool DioramaCameraManual_Update(DioramaCameraManualState *manual,
                                float elapsed_seconds, bool input_active);

/* Host controls can be refreshed on retained frames without changing motion,
 * event flags or captured strength. Mode is DioramaCameraMode in settings.h. */
typedef struct DioramaCameraPresentationState {
  int mode;
  DioramaCameraPose free_pose;
  DioramaCameraPose dynamic_baseline;
  float orbit_yaw;
  float orbit_pitch;
  float zoom_offset;
  float framing_override;
} DioramaCameraPresentationState;

typedef struct DioramaCameraMotion {
  float lean_yaw;
  float lean_pitch;
  bool event_hit;
  bool event_land;
  bool event_boost; /* Diagnostic only: the native byte is not an ability edge. */
} DioramaCameraMotion;

/* One value snapshot travels through FrameSlot. */
typedef struct DioramaCameraFrame {
  DioramaCameraPresentationState controls;
  int reactive_strength;
  DioramaCameraMotion motion;
} DioramaCameraFrame;

typedef struct DioramaCameraObservation {
  int16_t velocity_x;
  int16_t velocity_y;
  uint8_t hp;
  bool boost;
} DioramaCameraObservation;

/* One observer per game session, including captures while Diorama is off.
 * Graphics resets and room changes do not reset the original history. */
typedef struct DioramaCameraObserver {
  float velocity_x_average;
  float velocity_y_average;
  bool previous_boost;
  int16_t previous_velocity_y;
  uint8_t previous_hp;
} DioramaCameraObserver;
#define DIORAMA_CAMERA_OBSERVER_INIT \
  { .velocity_x_average = 4.0f, .velocity_y_average = 4.0f }

/* The presenter owns the damped pose and impulses. The two timestamps serve
 * different purposes: easing advances on each present; events fire once per
 * captured frame, even when that frame is presented more than once. */
typedef struct DioramaCameraPresenter {
  DioramaCameraPose pose;
  int mode;
  uint64_t last_present_ns;
  uint64_t last_capture_ns;
  float kick_pitch;
  float kick_zoom;
} DioramaCameraPresenter;
#define DIORAMA_CAMERA_PRESENTER_INIT { .mode = -1 }

typedef struct DioramaCameraView {
  DioramaCameraPose pose;
  float distance_scale;
  float distance_offset;
  /* 0 permits free framing; 1 fully centers and clamps the dynamic view. */
  float framing_weight;
} DioramaCameraView;

/* Sample once per capture, using elapsed emulation ticks (zero on a host-
 * paused redraw). HP loss triggers hit; falling then settling triggers land. */
DioramaCameraMotion DioramaCamera_Observe(
    DioramaCameraObserver *observer, const DioramaCameraObservation *input,
    int elapsed_ticks);
/* Resolve only from the captured frame and the presentation clock. Free Cam
 * and mode changes snap; Dynamic Cam eases and adds one-shot hit/land kicks. */
DioramaCameraView DioramaCamera_Present(
    DioramaCameraPresenter *presenter, const DioramaCameraFrame *frame,
    uint64_t capture_ns, uint64_t now_ns);

/* Host adapters: controls from settings/manual orbit, then motion from WRAM.
 * CaptureFrame is called for every frame, even when Diorama is not active. */
void Diorama_CaptureCameraPresentationState(DioramaCameraPresentationState *state);
void DioramaCamera_CaptureFrame(DioramaCameraFrame *frame, int elapsed_ticks);

#endif /* AR_DIORAMA_CAMERA_H */
