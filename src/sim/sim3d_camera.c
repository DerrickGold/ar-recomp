#include "sim3d.h"

#include <stddef.h>
#include <stdio.h>
#include <math.h>

#include "actraiser_game.h"
#include "camera_orbit.h"
#include "constants.h"
#include "host/host_clock.h"
#include "scene3d_math.h"
#include "settings.h"
#include "sim3d_camera_limits.h"
#include "user_data_dir.h"

enum {
  kMilliradiansPerRadian = kPermilleScale,
  kSim3DCameraDistanceScale = kPercentScale,
  kSim3DCameraSettingsSaveDelayMs = 500,
};

static const float kSim3DCameraDefaultSceneRadius = 0.4f;
static const float kSim3DCameraDistanceMinimum =
    (float)kSim3DCameraDistanceMinimumX100 / kPercentScale;
static const float kSim3DCameraDistanceMaximum =
    (float)kSim3DCameraDistanceMaximumX100 / kPercentScale;
static const float kSim3DCameraOrbitReturnTimeSeconds = 0.35f;

static bool s_dragging;
static bool s_settings_dirty;
static uint64_t s_settings_dirty_at_ms;
static CameraOrbit s_dynamic_orbit;
/* Inspection is local to a world-map visit, not the persisted town pose. */
static CameraOrbit s_world_orbit;
static float s_world_zoom;
static bool s_world_active;

static bool WorldNavigationActive(void) {
  return g_settings.sim3d_world_navigation &&
      g_ram[kActRaiserWram_MapGroup] == kActRaiserMapGroup_NonAction &&
      g_ram[kActRaiserWram_CurrentMap] == kActRaiserNonActionMap_WorldMap;
}

static bool SyncWorldNavigationCamera(void) {
  const bool active = WorldNavigationActive();
  if (active != s_world_active) {
    CameraOrbit_Reset(&s_world_orbit);
    s_world_zoom = 0;
    s_world_active = active;
  }
  return active;
}

static bool ProfileUsesGround(SimRenderFeatureMask features) {
  const SimRenderFeatureMask required =
      kSimFeature_SeparatedComposite | kSimFeature_GroundProjection;
  return (features & required) == required;
}

typedef struct TownCameraLimits {
  int distance_maximum_x100;
  int yaw_maximum_mrad;
} TownCameraLimits;

static TownCameraLimits ResolveTownCameraLimits(void) {
  const SimRenderFeatureMask required = kSimFeature_SeparatedComposite |
      kSimFeature_GroundProjection | kSimFeature_WorldUnderlay | kSimFeature_GlobeUnderlay;
  const SimRenderFeatureMask features =
      Settings_Sim3DRequestedFeatures() & Sim3D_ImplementedFeatures();
  const bool connected = g_settings.sim3d_mode &&
      ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
          g_ram[kActRaiserWram_CurrentMap]) && (features & required) == required;
  return (TownCameraLimits){
    connected ? kSim3DConnectedCameraDistanceMaximumX100 : kSim3DCameraDistanceMaximumX100,
    connected ? kSim3DConnectedCameraYawMaximumMrad : kSim3DCameraYawMaximumMrad,
  };
}

static int ClampInt(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static float ClampFloat(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static float ClampTownOrbitYaw(float baseline, float offset, int maximum_mrad) {
  const float maximum = (float)maximum_mrad / kMilliradiansPerRadian;
  return ClampFloat(baseline + offset, -maximum, maximum) - baseline;
}

bool Sim3DCamera_ControlsAvailable(bool textures_ready) {
  if (WorldNavigationActive()) return textures_ready;
  if (!g_settings.sim3d_mode || !textures_ready ||
      !(Sim3D_ImplementedFeatures() & kSimFeature_GroundProjection) ||
      !ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                  g_ram[kActRaiserWram_CurrentMap]) ||
      ActRaiser_SimMapPickerActive())
    return false;
  return ProfileUsesGround(Settings_Sim3DRequestedFeatures());
}

void Sim3DCamera_CapturePresentationState(
    Sim3DCameraPresentationState *state) {
  if (!state) return;
  const bool dynamic = g_settings.sim3d_camera_mode == kSimCam_Dynamic;
  const bool world = SyncWorldNavigationCamera();
  *state = (Sim3DCameraPresentationState){
    .mode = g_settings.sim3d_camera_mode,
    .pitch_mrad = dynamic
        ? g_settings.sim3d_dyncam_baseline_tilt_x_mrad
        : g_settings.sim3d_tilt_x_mrad,
    .yaw_mrad = dynamic
        ? g_settings.sim3d_dyncam_baseline_tilt_y_mrad
        : g_settings.sim3d_tilt_y_mrad,
    .distance_x100 = world ? 0 : dynamic
        ? g_settings.sim3d_dyncam_baseline_distance_x100
        : g_settings.sim3d_distance_x100,
    .orbit_yaw = world ? s_world_orbit.yaw : s_dynamic_orbit.yaw,
    .orbit_pitch = world ? s_world_orbit.pitch : s_dynamic_orbit.pitch,
  };
  if (world && s_world_zoom != 0) {
    /* The native Mode-7 matrix owns navigation zoom. Zero resolves to its
     * scale-matched camera, never the persisted free/dynamic town distance.
     * Manual inspection remains a visit-local offset from that baseline. */
    const float base = Scene3D_AutoFitDistance(kSim3DCameraDefaultSceneRadius);
    state->distance_x100 = (int)lroundf(fminf(kSim3DCameraDistanceMaximum,
        fmaxf(kSim3DCameraDistanceMinimum, base + s_world_zoom)) * kSim3DCameraDistanceScale);
  } else if (!world) {
    /* Resolve old saved poses without rewriting settings during capture.
     * Reactive motion and the next input must start from the visible pose. */
    const TownCameraLimits limits = ResolveTownCameraLimits();
    if (state->distance_x100 > limits.distance_maximum_x100)
      state->distance_x100 = limits.distance_maximum_x100;
    state->yaw_mrad = ClampInt(state->yaw_mrad,
        -limits.yaw_maximum_mrad, limits.yaw_maximum_mrad);
    state->orbit_yaw = ClampTownOrbitYaw(
        (float)state->yaw_mrad / kMilliradiansPerRadian,
        state->orbit_yaw, limits.yaw_maximum_mrad);
  }
}

static void MarkSettingsDirty(void) {
  s_settings_dirty = true;
  s_settings_dirty_at_ms = HostClock_Milliseconds();
}

void Sim3DCamera_Adjust(float yaw_delta, float pitch_delta,
                        float zoom_delta) {
  if (!isfinite(yaw_delta) || !isfinite(pitch_delta) || !isfinite(zoom_delta)) return;
  if (SyncWorldNavigationCamera()) {
    const float pi = 3.14159265358979323846f;
    s_world_orbit.yaw = remainderf(s_world_orbit.yaw + yaw_delta, 2 * pi);
    s_world_orbit.pitch = ClampFloat(s_world_orbit.pitch + pitch_delta, -pi * .5f, pi * .5f);
    if (zoom_delta != 0) {
      Sim3DCameraPresentationState state;
      Sim3DCamera_CapturePresentationState(&state);
      const float current = state.distance_x100 > 0 ? state.distance_x100 / 100.0f
          : Scene3D_AutoFitDistance(kSim3DCameraDefaultSceneRadius);
      s_world_zoom += ClampFloat(current + zoom_delta,
          kSim3DCameraDistanceMinimum, kSim3DCameraDistanceMaximum) - current;
    }
    return;
  }
  const TownCameraLimits limits = ResolveTownCameraLimits();
  const float maximum_distance = (float)limits.distance_maximum_x100 / kSim3DCameraDistanceScale;
  const float maximum_yaw = (float)limits.yaw_maximum_mrad / kMilliradiansPerRadian;
  if (g_settings.sim3d_camera_mode == kSimCam_Dynamic) {
    const float baseline_yaw =
        (float)ClampInt(g_settings.sim3d_dyncam_baseline_tilt_y_mrad,
            -limits.yaw_maximum_mrad, limits.yaw_maximum_mrad) /
        (float)kMilliradiansPerRadian;
    const float baseline_pitch =
        (float)g_settings.sim3d_dyncam_baseline_tilt_x_mrad /
        (float)kMilliradiansPerRadian;
    /* A saved baseline or held orbit can predate enabling the connected
     * world. Apply the new limit before the delta, not after invisible travel. */
    s_dynamic_orbit.yaw = ClampTownOrbitYaw(
        baseline_yaw, s_dynamic_orbit.yaw, limits.yaw_maximum_mrad);
    CameraOrbit_Adjust(
        &s_dynamic_orbit, yaw_delta, pitch_delta,
        baseline_yaw, baseline_pitch,
        -maximum_yaw, maximum_yaw,
        (float)kSim3DCameraPitchMinimumMrad / kMilliradiansPerRadian,
        (float)kSim3DCameraPitchMaximumMrad / kMilliradiansPerRadian);

    if (zoom_delta == 0.0f) return;
    float distance = g_settings.sim3d_dyncam_baseline_distance_x100 > 0
        ? (float)g_settings.sim3d_dyncam_baseline_distance_x100 /
            (float)kSim3DCameraDistanceScale
        : Scene3D_AutoFitDistance(kSim3DCameraDefaultSceneRadius);
    distance = ClampFloat(
        ClampFloat(distance, kSim3DCameraDistanceMinimum, maximum_distance) + zoom_delta,
        kSim3DCameraDistanceMinimum,
        maximum_distance);
    g_settings.sim3d_dyncam_baseline_distance_x100 =
        (int)(distance * (float)kSim3DCameraDistanceScale);
    MarkSettingsDirty();
    return;
  }

  const int yaw_mrad = ClampInt(g_settings.sim3d_tilt_y_mrad,
      -limits.yaw_maximum_mrad, limits.yaw_maximum_mrad) +
      (int)(yaw_delta * (float)kMilliradiansPerRadian);
  const int pitch_mrad = g_settings.sim3d_tilt_x_mrad +
      (int)(pitch_delta * (float)kMilliradiansPerRadian);
  g_settings.sim3d_tilt_y_mrad = ClampInt(
      yaw_mrad, -limits.yaw_maximum_mrad, limits.yaw_maximum_mrad);
  g_settings.sim3d_tilt_x_mrad = ClampInt(
      pitch_mrad, kSim3DCameraPitchMinimumMrad,
      kSim3DCameraPitchMaximumMrad);

  if (zoom_delta != 0.0f) {
    float distance = g_settings.sim3d_distance_x100 > 0
        ? (float)g_settings.sim3d_distance_x100 /
            (float)kSim3DCameraDistanceScale
        : Scene3D_AutoFitDistance(kSim3DCameraDefaultSceneRadius);
    distance = ClampFloat(
        ClampFloat(distance, kSim3DCameraDistanceMinimum, maximum_distance) + zoom_delta,
        kSim3DCameraDistanceMinimum,
        maximum_distance);
    g_settings.sim3d_distance_x100 =
        (int)(distance * (float)kSim3DCameraDistanceScale);
  }
  MarkSettingsDirty();
}

bool Sim3DCamera_UpdateDynamic(float elapsed_seconds, bool orbit_held) {
  if (SyncWorldNavigationCamera()) {
    if (!isfinite(elapsed_seconds) || elapsed_seconds <= 0) return false;
    return CameraOrbit_Update(
        &s_world_orbit, elapsed_seconds, orbit_held, .65f);
  }
  if (g_settings.sim3d_camera_mode != kSimCam_Dynamic) {
    bool changed = s_dynamic_orbit.yaw != 0.0f ||
                   s_dynamic_orbit.pitch != 0.0f;
    CameraOrbit_Reset(&s_dynamic_orbit);
    return changed;
  }
  Sim3DCameraPresentationState state;
  Sim3DCamera_CapturePresentationState(&state);
  s_dynamic_orbit.yaw = state.orbit_yaw;
  return CameraOrbit_Update(
      &s_dynamic_orbit, elapsed_seconds, orbit_held,
      kSim3DCameraOrbitReturnTimeSeconds);
}

void Sim3DCamera_GetDynamicOrbit(float *yaw, float *pitch) {
  Sim3DCameraPresentationState state;
  Sim3DCamera_CapturePresentationState(&state);
  if (yaw) *yaw = state.orbit_yaw;
  if (pitch) *pitch = state.orbit_pitch;
}

void Sim3DCamera_Reset(void) {
  if (SyncWorldNavigationCamera()) {
    CameraOrbit_Reset(&s_world_orbit);
    s_world_zoom = 0;
    return;
  }
  /* Reset the pose currently in use. Resetting the hidden free pose while
   * Dynamic Cam is active would make the action appear unresponsive. */
  static const char *const kFreeCameraSettingKeys[] = {
    "sim3d_tilt_x_mrad",
    "sim3d_tilt_y_mrad",
    "sim3d_distance_x100",
  };
  static const char *const kDynamicCameraSettingKeys[] = {
    "sim3d_dyncam_baseline_tilt_x_mrad",
    "sim3d_dyncam_baseline_tilt_y_mrad",
    "sim3d_dyncam_baseline_distance_x100",
  };
  _Static_assert(
      sizeof(kFreeCameraSettingKeys) == sizeof(kDynamicCameraSettingKeys),
      "SIM 3D camera modes must reset the same number of pose fields");
  const char *const *keys =
      g_settings.sim3d_camera_mode == kSimCam_Dynamic
          ? kDynamicCameraSettingKeys
          : kFreeCameraSettingKeys;
  const size_t key_count =
      sizeof(kFreeCameraSettingKeys) / sizeof(kFreeCameraSettingKeys[0]);
  for (size_t i = 0; i < key_count; i++) {
    const SettingDesc *setting = Settings_Find(keys[i]);
    if (setting) Settings_Reset(setting);
  }
  CameraOrbit_Reset(&s_dynamic_orbit);
  MarkSettingsDirty();
}

bool Sim3DCamera_IsDragging(void) {
  return s_dragging;
}

void Sim3DCamera_SetDragging(bool dragging) {
  s_dragging = dragging;
}

void Sim3DCamera_FlushSettingsIfDirty(void) {
  if (!s_settings_dirty || s_dragging ||
      HostClock_Milliseconds() - s_settings_dirty_at_ms <=
          kSim3DCameraSettingsSaveDelayMs)
    return;

  s_settings_dirty = false;
  char settings_path[kHostPathCapacity];
  UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
  if (!Settings_Save(settings_path))
    fprintf(stderr, "[sim3d] failed to persist camera settings\n");
}
