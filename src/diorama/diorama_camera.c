#include "diorama/diorama_camera.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/settings.h" /* Camera-mode constants only; never live settings. */
#include "constants.h"
#include "render/camera_velocity.h"

static const float kDioramaDampTau = 0.15f;
static const float kDioramaLeanYaw = 0.10f;
/* Pitch needs more range to read as clearly as yaw in the three-quarter view. */
static const float kDioramaLeanPitch = 0.12f;
static const float kDioramaKickPitch = 0.05f;
static const float kDioramaKickZoom = -0.15f;
static const float kDioramaKickTau = 0.20f;
static const float kDioramaManualIdleSeconds = 0.40f;
static const float kDioramaManualReturnSeconds = 0.35f;

void DioramaCameraManual_Input(DioramaCameraManualState *manual) {
  if (!manual) return;
  manual->framing_override = 1.0f;
  manual->idle_remaining = kDioramaManualIdleSeconds;
  manual->input_pending = true;
}

bool DioramaCameraManual_Update(DioramaCameraManualState *manual,
                                float elapsed_seconds, bool input_active) {
  if (!manual || !isfinite(elapsed_seconds) || elapsed_seconds <= 0.0f)
    return false;
  if (input_active || manual->input_pending) {
    DioramaCameraManual_Input(manual);
    manual->input_pending = false;
    return true;
  }
  if (manual->framing_override == 0.0f) return false;

  /* Spend only time after the idle grace period on the return animation. */
  if (manual->idle_remaining > 0.0f) {
    float idle_time = fminf(elapsed_seconds, manual->idle_remaining);
    manual->idle_remaining -= idle_time;
    elapsed_seconds -= idle_time;
    if (elapsed_seconds <= 0.0f) return false;
  }
  float decay = expf(-elapsed_seconds / kDioramaManualReturnSeconds);
  manual->offset.tilt_x *= decay;
  manual->offset.tilt_y *= decay;
  manual->offset.distance *= decay;
  manual->framing_override *= decay;
  if (fabsf(manual->offset.tilt_x) < 0.0001f) manual->offset.tilt_x = 0.0f;
  if (fabsf(manual->offset.tilt_y) < 0.0001f) manual->offset.tilt_y = 0.0f;
  if (fabsf(manual->offset.distance) < 0.001f) manual->offset.distance = 0.0f;
  if (manual->framing_override < 0.0001f)
    *manual = (DioramaCameraManualState){0};
  return true;
}

DioramaCameraMotion DioramaCamera_Observe(
    DioramaCameraObserver *observer, const DioramaCameraObservation *input,
    int elapsed_ticks) {
  DioramaCameraMotion motion = {
    .lean_yaw = CameraVelocity_Normalize(
        input->velocity_x, &observer->velocity_x_average, elapsed_ticks),
    .lean_pitch = CameraVelocity_Normalize(
        input->velocity_y, &observer->velocity_y_average, elapsed_ticks),
  };
  /* HP decreases identify the damage frame; the native invulnerability flag
   * arrives later. Landing uses the updated recent velocity average. */
  motion.event_hit = input->hp < observer->previous_hp;
  observer->previous_hp = input->hp;
  bool was_falling = observer->previous_velocity_y >
      (int16_t)(observer->velocity_y_average * 0.5f);
  bool now_settled = abs((int)input->velocity_y) <
      (int)(observer->velocity_y_average * 0.15f);
  motion.event_land = was_falling && now_settled;
  observer->previous_velocity_y = input->velocity_y;
  motion.event_boost = input->boost && !observer->previous_boost;
  observer->previous_boost = input->boost;
  return motion;
}

DioramaCameraView DioramaCamera_Present(
    DioramaCameraPresenter *presenter, const DioramaCameraFrame *frame,
    uint64_t capture_ns, uint64_t now_ns) {
  bool dynamic = frame->controls.mode == kDioramaCam_Dynamic;
  DioramaCameraPose target;
  if (dynamic) {
    float gain =
        (float)frame->reactive_strength / (float)kPercentScale;
    target = frame->controls.dynamic_baseline;
    target.tilt_y += kDioramaLeanYaw * gain * frame->motion.lean_yaw;
    target.tilt_x += kDioramaLeanPitch * gain * frame->motion.lean_pitch;
  } else {
    target = frame->controls.free_pose;
  }

  /* Manual control and mode changes snap. Otherwise ease using wall time,
   * so the response is independent of the monitor's present rate. */
  bool mode_changed = presenter->mode != frame->controls.mode;
  presenter->mode = frame->controls.mode;
  float dt = 0.0f;
  if (presenter->last_present_ns != 0) {
    dt = (float)(now_ns - presenter->last_present_ns) / 1e9f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;   /* sanity clamp (e.g. resuming after a pause) */
  }
  if (!dynamic || mode_changed || presenter->last_present_ns == 0) {
    presenter->pose = target;
  } else {
    float damping_alpha = 1.0f - expf(-dt / kDioramaDampTau);
    presenter->pose.tilt_x +=
        (target.tilt_x - presenter->pose.tilt_x) * damping_alpha;
    presenter->pose.tilt_y +=
        (target.tilt_y - presenter->pose.tilt_y) * damping_alpha;
    presenter->pose.distance +=
        (target.distance - presenter->pose.distance) * damping_alpha;
  }
  presenter->last_present_ns = now_ns;

  /* A retained frame can be presented repeatedly. Consume each captured
   * event once, but decay its impulse on every presentation. */
  bool new_slot = dynamic && capture_ns != presenter->last_capture_ns;
  presenter->last_capture_ns = capture_ns;
  if (new_slot) {
    float gain =
        (float)frame->reactive_strength / (float)kPercentScale;
    if (frame->motion.event_hit || frame->motion.event_land)
      presenter->kick_pitch += kDioramaKickPitch * gain;
    /* Hit adds a zoom punch; landing adds only a pitch jolt. */
    if (frame->motion.event_hit)
      presenter->kick_zoom += kDioramaKickZoom * gain;
    /* Boost remains diagnostic only: its native byte cycles during ordinary
     * movement, so treating it as an ability edge causes constant jolts. */
  }
  if (!dynamic) {
    presenter->kick_pitch = 0.0f;
    presenter->kick_zoom = 0.0f;
  } else if (dt > 0.0f) {
    float kick_decay = expf(-dt / kDioramaKickTau);
    presenter->kick_pitch *= kick_decay;
    presenter->kick_zoom *= kick_decay;
  }
  DioramaCameraPose final_cam = presenter->pose;
  float distance_scale = 1.0f;
  if (dynamic) {
    final_cam.tilt_x += presenter->kick_pitch +
        frame->controls.orbit_pitch;
    final_cam.tilt_y += frame->controls.orbit_yaw;
    distance_scale = 1.0f + presenter->kick_zoom;
  }

  /* Keep the visible-response diagnostic beside the behavior it reports. */
  static int dyncam_log_on = -1;
  if (dyncam_log_on < 0) {
    const char *e = getenv("AR_DYNCAM_LOG");
    dyncam_log_on = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  if (dyncam_log_on && dynamic) {
    fprintf(stderr,
      "[dyncam] mode=%d gain=%.3f lean_yaw=%.3f lean_pitch=%.3f "
      "target(x=%.4f y=%.4f d=%.3f) render(x=%.4f y=%.4f d=%.3f) "
      "kick(pitch=%.4f zoom=%.4f) evt(hit=%d land=%d boost=%d) "
      "manual(yaw=%.4f pitch=%.4f zoom=%.4f framing=%.4f) "
      "present-ns=%llu capture-ns=%llu\n",
      frame->controls.mode,
      (double)frame->reactive_strength / (double)kPercentScale,
      (double)frame->motion.lean_yaw,
      (double)frame->motion.lean_pitch,
      (double)target.tilt_x, (double)target.tilt_y,
      (double)target.distance,
      (double)presenter->pose.tilt_x,
      (double)presenter->pose.tilt_y,
      (double)presenter->pose.distance,
      (double)presenter->kick_pitch, (double)presenter->kick_zoom,
      frame->motion.event_hit, frame->motion.event_land,
      frame->motion.event_boost,
      (double)frame->controls.orbit_yaw, (double)frame->controls.orbit_pitch,
      (double)frame->controls.zoom_offset, (double)frame->controls.framing_override,
      (unsigned long long)now_ns, (unsigned long long)capture_ns);
  }

  return (DioramaCameraView){
    .pose = final_cam,
    .distance_scale = distance_scale,
    .distance_offset = dynamic ? frame->controls.zoom_offset : 0.0f,
    .framing_weight = dynamic ? 1.0f - frame->controls.framing_override : 0.0f,
  };
}
