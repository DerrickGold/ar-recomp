#include "sim/sim3d/sim3d_camera.h"

#include <math.h>
#include "app/settings.h" /* Camera-mode enum only; no live settings. */
#include "constants.h"
#include "render/camera_velocity.h"

/* The near-overhead town view needs gentler, slower lean than action stages:
 * excessive angles make the ground appear to swim under the camera. */
static const float kSimLeanYaw = 0.045f;    /* rad at full lean */
static const float kSimLeanPitch = 0.055f;  /* rad at full lean */
static const float kSimDampTau = 0.22f;     /* s; slower than action mode */
static const float kSimKickPitch = 0.030f;  /* rad */
static const float kSimKickZoom = -0.09f;   /* fraction; slight punch in */
static const float kSimKickTau = 0.18f;     /* s */

Sim3DCameraMotion Sim3DCamera_Observe(
    Sim3DCameraObserver *observer, const Sim3DCameraObservation *input,
    int elapsed_ticks) {
  if (!input->in_town) {
    observer->previous_in_town = false;
    return (Sim3DCameraMotion){0};
  }
  Sim3DCameraMotion motion = {
    .lean_yaw = CameraVelocity_Normalize(
        input->velocity_x, &observer->velocity_x_average, elapsed_ticks),
    .lean_pitch = CameraVelocity_Normalize(
        input->velocity_y, &observer->velocity_y_average, elapsed_ticks),
    /* A new town seeds HP without a jolt, even if the previous visit ended
     * with more HP. Damage is visible before the invulnerability flag. */
    .event_hit = observer->previous_in_town && input->hp < observer->previous_hp,
  };
  observer->previous_hp = input->hp;
  observer->previous_in_town = true;
  return motion;
}

void Sim3DCamera_ApplyMotion(
    Sim3DCameraPresenter *presenter, const Sim3DCameraFrame *frame,
    uint64_t capture_ns, uint64_t now_ns, Scene3DCamera *camera) {
  bool dynamic = frame->mode == kSimCam_Dynamic;
  bool reactive = dynamic && frame->reactive_strength > 0;

  /* A mode change snaps rather than eases. Easing across it would swing the
   * camera from the free pose to the baseline over a visible fraction of a
   * second, which reads as the camera being knocked rather than as the player
   * having switched modes. Same rule the diorama camera uses. */
  bool mode_changed = presenter->previous_mode != frame->mode;
  presenter->previous_mode = frame->mode;

  float dt = 0.0f;
  if (presenter->last_ns != 0) {
    dt = (float)(now_ns - presenter->last_ns) / 1e9f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;   /* resuming from a pause is not a huge step */
  }
  presenter->last_ns = now_ns;

  if (!dynamic) {
    /* Cleared rather than left to decay, so switching the feature off is
     * immediate and switching it back on starts level instead of resuming a
     * lean from whenever it was turned off. */
    *presenter = (Sim3DCameraPresenter){
      .last_ns = now_ns, .previous_mode = frame->mode,
    };
    return;
  }

  float gain = (float)frame->reactive_strength / (float)kPercentScale;
  float target_x = kSimLeanPitch * gain * frame->motion.lean_pitch;
  float target_y = kSimLeanYaw * gain * frame->motion.lean_yaw;

  if (!reactive) {
    presenter->lean_x = 0.0f;
    presenter->lean_y = 0.0f;
    presenter->kick_pitch = 0.0f;
    presenter->kick_zoom = 0.0f;
    presenter->active = false;
  } else if (!presenter->active || mode_changed || dt <= 0.0f) {
    presenter->lean_x = target_x;
    presenter->lean_y = target_y;
    presenter->active = true;
  } else {
    float alpha = 1.0f - expf(-dt / kSimDampTau);
    presenter->lean_x += (target_x - presenter->lean_x) * alpha;
    presenter->lean_y += (target_y - presenter->lean_y) * alpha;
  }

  /* Impulses fire only on a genuinely new capture. Re-presenting a slot already
   * processed must not re-trigger, or a paused frame would
   * shake forever. Stacking is additive so a hit taken mid-jolt reads as
   * stronger rather than restarting. */
  if (reactive && capture_ns != presenter->last_slot_ns) {
    presenter->last_slot_ns = capture_ns;
    if (frame->motion.event_hit) {
      presenter->kick_pitch += kSimKickPitch * gain;
      presenter->kick_zoom += kSimKickZoom * gain;
    }
  }
  if (reactive && dt > 0.0f) {
    float decay = expf(-dt / kSimKickTau);
    presenter->kick_pitch *= decay;
    presenter->kick_zoom *= decay;
  }

  camera->tilt_x += presenter->lean_x + presenter->kick_pitch +
      frame->orbit_pitch;
  camera->tilt_y += presenter->lean_y + frame->orbit_yaw;
  camera->distance *= 1.0f + presenter->kick_zoom;
  if (camera->distance < 2.0f) camera->distance = 2.0f;
}
