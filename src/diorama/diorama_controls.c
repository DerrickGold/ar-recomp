/* Desktop settings and input adapter for the portable Diorama compositor. */
#include "diorama.h"
#include "diorama_layer_manifest.h"
#include "diorama_rom_skybox_resource.h"
#include "diorama_upload.h"
#include "app/settings.h"
#include "app/user_data_dir.h"
#include "constants.h"
#include "host/host_clock.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Camera constants (§5.6) ─────────────────────────────────────────── */

static const float kDioramaTiltMin = -0.7f, kDioramaTiltMax = 0.7f;
static const float kDioramaDistMin =  2.0f, kDioramaDistMax = 20.0f;
static const float kDioramaDragRadPerPx = 0.005f;
static const float kDioramaZoomStep     = 0.5f;

float Diorama_DragRadPerPx(void) { return kDioramaDragRadPerPx; }
float Diorama_ZoomStep(void)     { return kDioramaZoomStep; }

/* ── Camera state ────────────────────────────────────────────────────── */

typedef DioramaCameraPose DioramaCamera;

/* A3 (followup doc): zero-init, not a hand-tuned literal — every field here
 * is unconditionally overwritten by Diorama_SeedCameraFromSettings (below)
 * before first render (boot, camera-row menu edits, and Reset Camera all
 * call it), so the settings descriptors are the single source of truth for
 * defaults. A literal here would look load-bearing despite never being used. */
static DioramaCamera s_diorama_cam;
static float s_diorama_auto_distance = 5.0f;
static bool s_diorama_settings_dirty;
static uint64_t s_diorama_settings_dirty_at;
static bool s_diorama_dragging;
static DioramaCameraManualState s_diorama_manual;

bool Diorama_IsDragging(void)          { return s_diorama_dragging; }
void Diorama_SetDragging(bool dragging) { s_diorama_dragging = dragging; }

static float Clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Camera operations ───────────────────────────────────────────────── */

void Diorama_SeedCameraFromSettings(void) {
  s_diorama_manual = (DioramaCameraManualState){0};
  s_diorama_cam.tilt_x =
      (float)g_settings.diorama_tilt_x_mrad / (float)kPermilleScale;
  s_diorama_cam.tilt_y =
      (float)g_settings.diorama_tilt_y_mrad / (float)kPermilleScale;
  s_diorama_cam.distance =
      (float)g_settings.diorama_distance_x100 / (float)kPercentScale;
}

void Diorama_CaptureCameraPresentationState(
    DioramaCameraPresentationState *state) {
  if (!state) return;
  *state = (DioramaCameraPresentationState){
    .mode = g_settings.diorama_camera_mode,
    .free_pose = {
      .tilt_x =
          (float)g_settings.diorama_tilt_x_mrad / (float)kPermilleScale,
      .tilt_y =
          (float)g_settings.diorama_tilt_y_mrad / (float)kPermilleScale,
      .distance =
          (float)g_settings.diorama_distance_x100 / (float)kPercentScale,
    },
    .dynamic_baseline = {
      .tilt_x =
          (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad /
              (float)kPermilleScale,
      .tilt_y =
          (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad /
              (float)kPermilleScale,
      .distance =
          (float)g_settings.diorama_dyncam_baseline_distance_x100 /
              (float)kPercentScale,
    },
    .orbit_yaw = s_diorama_manual.offset.tilt_y,
    .orbit_pitch = s_diorama_manual.offset.tilt_x,
    .zoom_offset = s_diorama_manual.offset.distance,
    .framing_override = s_diorama_manual.framing_override,
  };
}

void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
  if (g_settings.diorama_camera_mode == kDioramaCam_Dynamic) {
    if (d_yaw == 0.0f && d_pitch == 0.0f && d_zoom == 0.0f) return;
    const float baseline_yaw =
        (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad /
        (float)kPermilleScale;
    const float baseline_pitch =
        (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad /
        (float)kPermilleScale;
    s_diorama_manual.offset.tilt_y = Clampf(
        baseline_yaw + s_diorama_manual.offset.tilt_y + d_yaw,
        kDioramaTiltMin, kDioramaTiltMax) - baseline_yaw;
    s_diorama_manual.offset.tilt_x = Clampf(
        baseline_pitch + s_diorama_manual.offset.tilt_x + d_pitch,
        kDioramaTiltMin, kDioramaTiltMax) - baseline_pitch;

    if (d_zoom != 0.0f) {
      float baseline_distance =
          g_settings.diorama_dyncam_baseline_distance_x100 > 0
              ? (float)g_settings.diorama_dyncam_baseline_distance_x100 /
                    (float)kPercentScale
              : s_diorama_auto_distance;
      s_diorama_manual.offset.distance = Clampf(
          baseline_distance + s_diorama_manual.offset.distance + d_zoom,
          kDioramaDistMin, kDioramaDistMax) - baseline_distance;
    }
    DioramaCameraManual_Input(&s_diorama_manual);
    return;
  }

  s_diorama_cam.tilt_y = Clampf(s_diorama_cam.tilt_y + d_yaw,
                                kDioramaTiltMin, kDioramaTiltMax);
  s_diorama_cam.tilt_x = Clampf(s_diorama_cam.tilt_x + d_pitch,
                                kDioramaTiltMin, kDioramaTiltMax);
  if (d_zoom != 0.0f) {
    float base = (s_diorama_cam.distance > 0.0f) ? s_diorama_cam.distance
                                                 : s_diorama_auto_distance;
    s_diorama_cam.distance = Clampf(base + d_zoom,
                                    kDioramaDistMin, kDioramaDistMax);
  }
  g_settings.diorama_tilt_x_mrad =
      (int)(s_diorama_cam.tilt_x * (float)kPermilleScale);
  g_settings.diorama_tilt_y_mrad =
      (int)(s_diorama_cam.tilt_y * (float)kPermilleScale);
  g_settings.diorama_distance_x100 =
      (int)(s_diorama_cam.distance * (float)kPercentScale);
  s_diorama_settings_dirty = true;
  s_diorama_settings_dirty_at = HostClock_Milliseconds();
}

bool Diorama_UpdateDynamicCamera(float elapsed_seconds, bool input_active) {
  if (g_settings.diorama_camera_mode != kDioramaCam_Dynamic) {
    bool changed = s_diorama_manual.framing_override != 0.0f;
    s_diorama_manual = (DioramaCameraManualState){0};
    return changed;
  }
  return DioramaCameraManual_Update(
      &s_diorama_manual, elapsed_seconds, input_active);
}

void Diorama_ResetCamera(void) {
  static const char *const kResetKeys[] = {
    "diorama_tilt_x_mrad",
    "diorama_tilt_y_mrad",
    "diorama_distance_x100",
    /* B4-baseline (followup doc): Reset Camera also returns Dynamic Cam's
     * dedicated baseline pose to its defaults, so it's a true "return
     * everything camera-related to defaults" action regardless of which
     * mode is active. */
    "diorama_dyncam_baseline_tilt_x_mrad",
    "diorama_dyncam_baseline_tilt_y_mrad",
    "diorama_dyncam_baseline_distance_x100",
    "diorama_reactive_strength",
    "diorama_depth_shade",
    "diorama_layer_backdrop",
    "diorama_layer_bg2",
    "diorama_layer_bg1",
    "diorama_layer_obj",
    "diorama_layer_bg3",
    "diorama_skybox",
    "diorama_shoebox",
  };
  for (size_t i = 0; i < sizeof(kResetKeys) / sizeof(kResetKeys[0]); i++) {
    const SettingDesc *row = Settings_Find(kResetKeys[i]);
    if (row) Settings_Reset(row);
  }
  Diorama_SeedCameraFromSettings();
  s_diorama_settings_dirty = true;
  s_diorama_settings_dirty_at = HostClock_Milliseconds();
}

void Diorama_FlushSettingsIfDirty(void) {
  if (s_diorama_settings_dirty && !s_diorama_dragging &&
      HostClock_Milliseconds() - s_diorama_settings_dirty_at > 500) {
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof settings_path, "settings.ini");
    if (Settings_SaveDeferred(settings_path))
      s_diorama_settings_dirty = false;
    else {
      s_diorama_settings_dirty_at = HostClock_Milliseconds();
      fprintf(stderr, "[diorama] failed to persist camera settings\n");
    }
  }
}

void Diorama_ApplySetting(const SettingDesc *desc) {
  if (desc->field == &g_settings.diorama_tilt_x_mrad ||
      desc->field == &g_settings.diorama_tilt_y_mrad ||
      desc->field == &g_settings.diorama_distance_x100)
    Diorama_SeedCameraFromSettings();
}

void Diorama_SetAutoCameraDistance(float distance) {
  if (distance > 0.0f) s_diorama_auto_distance = distance;
}

bool Diorama_InitRomBackdrops(const uint8_t *rom_data, size_t rom_size) {
  return DioramaRomSkyboxResource_Init(rom_data, rom_size);
}

static ArRenderTexture ResolveSkybox(
    void *userdata, ArRenderDevice *device, int source,
    bool fill, uint32_t color, bool *state_restore_failed) {
  (void)userdata;
  return DioramaRomSkyboxResource_Resolve(device, source, fill, color, state_restore_failed);
}

static bool EnvironmentEnabled(const char *name) {
  const char *value = getenv(name);
  return !value || value[0] != '0';
}

void Diorama_CaptureRenderOptions(DioramaRenderOptions *options) {
  if (!options) return;
  static bool initialized, stack_grouping, sparse_coverage, skybox_prefilter, priority_surface;
  static bool waterfall_diagnostics;
  if (!initialized) {
    stack_grouping = EnvironmentEnabled("AR_DIORAMA_STACK_GROUP");
    sparse_coverage = EnvironmentEnabled("AR_DIORAMA_SPARSE_COVERAGE");
    skybox_prefilter = EnvironmentEnabled("AR_DIORAMA_SKYBOX_PREFILTER");
    priority_surface = EnvironmentEnabled("AR_DIORAMA_PRIORITY_SURFACE");
    const char *log = getenv("AR_AITOS_WATERFALL_LOG");
    waterfall_diagnostics = log && log[0] && log[0] != '0';
    initialized = true;
  }
  uint32_t visible = 0;
  for (int plane = 0; plane < kDioramaPlane_Count; ++plane) {
    bool enabled = false;
    if (plane == kDioramaPlane_Backdrop) enabled = g_settings.diorama_layer_backdrop;
    else if (DioramaPlaneIsObjectPriority(plane)) enabled = g_settings.diorama_layer_obj;
    else if (plane == SR_PPU_OVERLAY_BG1 || plane == kDioramaPlane_Bg1Hi ||
             plane == kDioramaPlane_Bg1Far) enabled = g_settings.diorama_layer_bg1;
    else if (plane == SR_PPU_OVERLAY_BG2 || plane == kDioramaPlane_Bg2Hi ||
             plane == kDioramaPlane_Bg2Far) enabled = g_settings.diorama_layer_bg2;
    else if (plane == SR_PPU_OVERLAY_BG3) enabled = g_settings.diorama_layer_bg3;
    if (enabled) visible |= 1u << plane;
  }
  *options = (DioramaRenderOptions){
    .visible_planes = visible,
    .skybox = g_settings.diorama_skybox,
    .depth_shade = (float)g_settings.diorama_depth_shade / kPercentScale,
    .hud_flat = g_settings.diorama_hud_flat,
    .shoebox = g_settings.diorama_shoebox,
    .margin_fix = g_settings.diorama_margin_fix,
    .shadow_blur = g_settings.gpu_fx_shadow,
    .rim_light = g_settings.gpu_fx_rim,
    .depth_of_field = g_settings.gpu_fx_dof,
    .edge_aa = g_settings.gpu_fx_edgeaa,
    .stack_grouping = stack_grouping,
    .sparse_coverage = sparse_coverage,
    .skybox_prefilter = skybox_prefilter,
    .priority_surface = priority_surface,
    .waterfall_diagnostics = waterfall_diagnostics,
    .layers = DioramaLayerManifest_Table(),
    .resolve_skybox = ResolveSkybox,
  };
}

void Diorama_ResetRendererResources(ArRenderDevice *device) {
  DioramaRomSkyboxResource_Reset(device);
  Diorama_ResetCompositorResources(device);
  DioramaUpload_Reset();
}

void Diorama_Shutdown(ArRenderDevice *device) {
  Diorama_ResetRendererResources(device);
}
