#ifndef AR_SIM3D_CAMERA_H
#define AR_SIM3D_CAMERA_H
/* SIM camera controls and reactive town motion. sim3d_camera.c samples host
 * controls/WRAM; sim3d_camera_motion.c interprets explicit observations and
 * clocks. World navigation uses manual orbit without town motion. */

#include <stdbool.h>
#include <stdint.h>
#include "render/scene3d_math.h"

/* Host-side camera controls for enhanced towns and globe navigation. Render
 * textures remain host-owned, so availability accepts their current readiness
 * rather than reaching into main.c. In Dynamic mode, zoom edits the persisted
 * baseline while orbit is a transient offset that decays after release.
 * Globe inspection uses a separate visit-local orbit/zoom, never town settings;
 * orbit returns on release in either town-camera mode. Reset restores travel. */
bool Sim3DCamera_ControlsAvailable(bool textures_ready);

/* Camera fields are host-owned presentation state rather than emulated scene
 * content. A retained game frame may refresh this small snapshot between ticks
 * so high-rate presentation follows mouse orbit immediately without recapturing
 * mutable PPU/WRAM state. */
typedef struct Sim3DCameraPresentationState {
  int mode;
  int pitch_mrad;
  int yaw_mrad;
  int distance_x100;
  float orbit_yaw;
  float orbit_pitch;
} Sim3DCameraPresentationState;

void Sim3DCamera_CapturePresentationState(
    Sim3DCameraPresentationState *state);
void Sim3DCamera_Adjust(float yaw_delta, float pitch_delta, float zoom_delta);
bool Sim3DCamera_UpdateDynamic(float elapsed_seconds, bool orbit_held);
void Sim3DCamera_GetDynamicOrbit(float *yaw, float *pitch);
void Sim3DCamera_Reset(void);
bool Sim3DCamera_IsDragging(void);
void Sim3DCamera_SetDragging(bool dragging);
void Sim3DCamera_FlushSettingsIfDirty(void);

/* The game snapshot carries only reactive inputs and manual offsets here.
 * The resolved base pose remains in SimFrameData.projection_* for all scene
 * geometry. Retained presents refresh mode/orbit, never motion or strength. */
typedef struct Sim3DCameraMotion {
  float lean_yaw, lean_pitch;
  bool event_hit;
} Sim3DCameraMotion;

typedef struct Sim3DCameraFrame {
  int mode;
  int reactive_strength;
  float orbit_yaw, orbit_pitch;
  Sim3DCameraMotion motion;
} Sim3DCameraFrame;

typedef struct Sim3DCameraObservation {
  bool in_town;
  int16_t velocity_x, velocity_y;
  uint8_t hp;
} Sim3DCameraObservation;

typedef struct Sim3DCameraObserver {
  float velocity_x_average, velocity_y_average;
  uint8_t previous_hp;
  bool previous_in_town;
} Sim3DCameraObserver;
#define SIM3D_CAMERA_OBSERVER_INIT \
  { .velocity_x_average = 4.0f, .velocity_y_average = 4.0f }

/* Observer and presenter histories survive room changes and graphics resets.
 * Outside town, observation is neutral and only the HP-edge guard resets;
 * motion calibration survives the visit. */
Sim3DCameraMotion Sim3DCamera_Observe(
    Sim3DCameraObserver *observer, const Sim3DCameraObservation *input,
    int elapsed_ticks);
void Sim3DCamera_CaptureFrame(Sim3DCameraFrame *frame, int elapsed_ticks);

typedef struct Sim3DCameraPresenter {
  float lean_x, lean_y;
  float kick_pitch, kick_zoom;
  uint64_t last_ns, last_slot_ns;
  bool active;
  int previous_mode;
} Sim3DCameraPresenter;
#define SIM3D_CAMERA_PRESENTER_INIT { .previous_mode = -1 }

/* Apply to the resolved base camera before building any scene projections.
 * Easing uses presentation time; hit impulses fire once per captured frame.
 * Free Cam clears response history; Dynamic Cam still permits manual orbit
 * at zero reactive strength. The caller resolves auto-fit before this call. */
void Sim3DCamera_ApplyMotion(
    Sim3DCameraPresenter *presenter, const Sim3DCameraFrame *frame,
    uint64_t capture_ns, uint64_t now_ns, Scene3DCamera *camera);

#endif /* AR_SIM3D_CAMERA_H */
