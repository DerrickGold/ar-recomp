#include "sim3d.h"

#include <stddef.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "actraiser_game.h"
#include "scene3d_math.h"
#include "settings.h"
#include "user_data_dir.h"

enum {
  kSim3DCameraTiltMinimumMrad = -700,
  kSim3DCameraTiltMaximumMrad = 700,
  kMilliradiansPerRadian = 1000,
  kSim3DCameraDistanceScale = 100,
  kSim3DCameraSettingsSaveDelayMs = 500,
  kSettingsPathCapacity = 1024,
};

static const float kSim3DCameraDefaultSceneRadius = 0.4f;
static const float kSim3DCameraDistanceMinimum = 2.0f;
static const float kSim3DCameraDistanceMaximum = 20.0f;

static bool s_dragging;
static bool s_settings_dirty;
static uint64_t s_settings_dirty_at_ms;

static bool ProfileUsesGround(SimRenderFeatureMask features) {
  const SimRenderFeatureMask required =
      kSimFeature_SeparatedComposite | kSimFeature_GroundProjection;
  return (features & required) == required;
}

bool Sim3DCamera_FreeControlsAvailable(bool textures_ready) {
  /* Free-camera input edits the authored free pose. Dynamic Cam renders from
   * its baseline instead, so accepting input there would silently edit a pose
   * the player cannot see. */
  if (g_settings.sim3d_camera_mode != kSimCam_Free) return false;
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
  char settings_path[kSettingsPathCapacity];
  UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
  if (!Settings_Save(settings_path))
    fprintf(stderr, "[sim3d] failed to persist camera settings\n");
}
