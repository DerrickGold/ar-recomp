#include "sim3d.h"

#include <stddef.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "actraiser_game.h"
#include "camera_orbit.h"
#include "constants.h"
#include "scene3d_math.h"
#include "settings.h"
#include "user_data_dir.h"

enum {
  kSim3DCameraTiltMinimumMrad = -700,
  kSim3DCameraTiltMaximumMrad = 700,
  kMilliradiansPerRadian = kPermilleScale,
  kSim3DCameraDistanceScale = kPercentScale,
  kSim3DCameraSettingsSaveDelayMs = 500,
};

static const float kSim3DCameraDefaultSceneRadius = 0.4f;
static const float kSim3DCameraDistanceMinimum = 2.0f;
static const float kSim3DCameraDistanceMaximum = 20.0f;
static const float kSim3DCameraOrbitReturnTimeSeconds = 0.35f;

static bool s_dragging;
static bool s_settings_dirty;
static uint64_t s_settings_dirty_at_ms;
static CameraOrbit s_dynamic_orbit;

static bool ProfileUsesGround(SimRenderFeatureMask features) {
  const SimRenderFeatureMask required =
      kSimFeature_SeparatedComposite | kSimFeature_GroundProjection;
  return (features & required) == required;
}

bool Sim3DCamera_ControlsAvailable(bool textures_ready) {
  if (!g_settings.sim3d_mode || !Diorama_NewPpuCapable() ||
      !textures_ready ||
      !(Sim3D_ImplementedFeatures() & kSimFeature_GroundProjection) ||
      !ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                  g_ram[kActRaiserWram_CurrentMap]) ||
      ActRaiser_SimMapPickerActive())
    return false;
  return ProfileUsesGround(Settings_Sim3DRequestedFeatures());
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

static void MarkSettingsDirty(void) {
  s_settings_dirty = true;
  s_settings_dirty_at_ms = SDL_GetTicks();
}

void Sim3DCamera_Adjust(float yaw_delta, float pitch_delta,
                        float zoom_delta) {
  if (g_settings.sim3d_camera_mode == kSimCam_Dynamic) {
    const float baseline_yaw =
        (float)g_settings.sim3d_dyncam_baseline_tilt_y_mrad /
        (float)kMilliradiansPerRadian;
    const float baseline_pitch =
        (float)g_settings.sim3d_dyncam_baseline_tilt_x_mrad /
        (float)kMilliradiansPerRadian;
    CameraOrbit_Adjust(
        &s_dynamic_orbit, yaw_delta, pitch_delta,
        baseline_yaw, baseline_pitch,
        (float)kSim3DCameraTiltMinimumMrad / kMilliradiansPerRadian,
        (float)kSim3DCameraTiltMaximumMrad / kMilliradiansPerRadian);

    if (zoom_delta == 0.0f) return;
    float distance = g_settings.sim3d_dyncam_baseline_distance_x100 > 0
        ? (float)g_settings.sim3d_dyncam_baseline_distance_x100 /
            (float)kSim3DCameraDistanceScale
        : Scene3D_AutoFitDistance(kSim3DCameraDefaultSceneRadius);
    distance = ClampFloat(
        distance + zoom_delta,
        kSim3DCameraDistanceMinimum,
        kSim3DCameraDistanceMaximum);
    g_settings.sim3d_dyncam_baseline_distance_x100 =
        (int)(distance * (float)kSim3DCameraDistanceScale);
    MarkSettingsDirty();
    return;
  }

  const int yaw_mrad = g_settings.sim3d_tilt_y_mrad +
      (int)(yaw_delta * (float)kMilliradiansPerRadian);
  const int pitch_mrad = g_settings.sim3d_tilt_x_mrad +
      (int)(pitch_delta * (float)kMilliradiansPerRadian);
  g_settings.sim3d_tilt_y_mrad = ClampInt(
      yaw_mrad, kSim3DCameraTiltMinimumMrad, kSim3DCameraTiltMaximumMrad);
  g_settings.sim3d_tilt_x_mrad = ClampInt(
      pitch_mrad, kSim3DCameraTiltMinimumMrad, kSim3DCameraTiltMaximumMrad);

  if (zoom_delta != 0.0f) {
    float distance = g_settings.sim3d_distance_x100 > 0
        ? (float)g_settings.sim3d_distance_x100 /
            (float)kSim3DCameraDistanceScale
        : Scene3D_AutoFitDistance(kSim3DCameraDefaultSceneRadius);
    distance = ClampFloat(
        distance + zoom_delta,
        kSim3DCameraDistanceMinimum,
        kSim3DCameraDistanceMaximum);
    g_settings.sim3d_distance_x100 =
        (int)(distance * (float)kSim3DCameraDistanceScale);
  }
  MarkSettingsDirty();
}

bool Sim3DCamera_UpdateDynamic(float elapsed_seconds, bool orbit_held) {
  if (g_settings.sim3d_camera_mode != kSimCam_Dynamic) {
    bool changed = s_dynamic_orbit.yaw != 0.0f ||
                   s_dynamic_orbit.pitch != 0.0f;
    CameraOrbit_Reset(&s_dynamic_orbit);
    return changed;
  }
  return CameraOrbit_Update(
      &s_dynamic_orbit, elapsed_seconds, orbit_held,
      kSim3DCameraOrbitReturnTimeSeconds);
}

void Sim3DCamera_GetDynamicOrbit(float *yaw, float *pitch) {
  if (yaw) *yaw = s_dynamic_orbit.yaw;
  if (pitch) *pitch = s_dynamic_orbit.pitch;
}

void Sim3DCamera_Reset(void) {
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
      SDL_GetTicks() - s_settings_dirty_at_ms <=
          kSim3DCameraSettingsSaveDelayMs)
    return;

  s_settings_dirty = false;
  char settings_path[kHostPathCapacity];
  UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
  if (!Settings_Save(settings_path))
    fprintf(stderr, "[sim3d] failed to persist camera settings\n");
}
