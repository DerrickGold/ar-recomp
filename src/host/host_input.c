#include "host_input.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actraiser_rtl.h"
#include "common_cpu_infra.h"
#include "diorama/diorama.h"
#include "forced_input.h"
#include "input_map.h"
#include "input_replay.h"
#include "present.h"
#include "runtime_settings.h"
#include "dev/scene_inspector.h"
#include "settings.h"
#include "settings_overlay.h"
#include "sim/sim3d.h"

/* FrameSlot_Capture records turbo in the immutable presentation snapshot. */
uint8 g_turbo;

static bool s_paused;
static bool s_inspector_owns_pause;
static bool s_paused_redraw_pending;
static uint32_t s_input_state;

extern bool g_sim3d_textures_ready;
extern InspectorPresentationSelection g_scene_inspector_presentation;

void HostInput_HandleKeyboard(int scancode, bool pressed) {
  InputMap_HandleKey(scancode, pressed);
  s_input_state = InputMap_State();
}

void HostInput_ClearHeld(void) {
  InputMap_Clear();
  s_input_state = 0;
}

uint32_t HostInput_ComputeGameInputs(bool *keep_running) {
  /* Re-read rather than trusting the last event: a gamepad's held bits are
   * owned by input_map.c and change without a keyboard event ever firing. */
  s_input_state = InputMap_State();
  const uint32_t inputs =
      ForcedInput_Apply(s_input_state, snes_frame_counter);
  const InputReplayFrameResult replay_result = InputReplay_Resolve(inputs);
  if (replay_result.stop_requested && keep_running) *keep_running = false;
  return replay_result.inputs;
}

bool HostInput_MenuGamepadIsActive(void) {
  return g_settings.input_device != kInputDevice_Keyboard &&
         InputMap_GamepadCount() > 0;
}

bool HostInput_MenuKeyboardIsActive(void) {
  const bool gamepad_connected = InputMap_GamepadCount() > 0;
  return InputMap_ShouldAcceptKeyboard(
      (InputDeviceMode)g_settings.input_device, gamepad_connected,
      gamepad_connected && InputMap_GamepadIsActive());
}

bool HostInput_KeyboardIsSuppressed(void) {
  return g_settings.input_device == kInputDevice_Auto &&
         InputMap_GamepadCount() > 0 && InputMap_GamepadIsActive();
}

bool HostInput_IsPaused(void) {
  return s_paused;
}

bool HostInput_IsTurbo(void) {
  return g_turbo != 0;
}

void HostInput_TogglePause(void) {
  s_paused = !s_paused;
  fprintf(stderr, "[pause] %s\n", s_paused ? "on" : "off");
}

void HostInput_ToggleTurbo(void) {
  g_turbo = !g_turbo;
  if (g_turbo)
    fprintf(stderr, "[turbo] ON (%dx)\n", g_settings.turbo_multiplier);
  else
    fprintf(stderr, "[turbo] off\n");
}

void HostInput_RequestPausedRedraw(void) {
  s_paused_redraw_pending = true;
}

bool HostInput_IsPausedRedrawPending(void) {
  return s_paused_redraw_pending;
}

bool HostInput_RedrawPausedFrameIfNeeded(void) {
  if ((!s_paused && !SettingsOverlay_IsOpen()) ||
      !s_paused_redraw_pending) {
    return false;
  }
  g_rtl_game_info->draw_ppu_frame();
  s_paused_redraw_pending = false;
  return true;
}

void HostInput_MarkFrameDrawn(void) {
  s_paused_redraw_pending = false;
}

bool HostInput_InspectorOwnsPause(void) {
  return s_inspector_owns_pause;
}

void HostInput_OnInspectorSelection(bool had_selection) {
  if (!SceneInspector_HasSelection()) return;
  if (!had_selection) s_inspector_owns_pause = !s_paused;
  HostInput_ClearHeld();
  s_paused = true;
}

void HostInput_CloseInspectorSelection(void) {
  SceneInspector_Clear();
  SettingsOverlay_HideDebugPanel();
  memset(&g_scene_inspector_presentation, 0,
         sizeof(g_scene_inspector_presentation));
  if (s_inspector_owns_pause) s_paused = false;
  s_inspector_owns_pause = false;
  HostInput_ClearHeld();
}

void HostInput_AdjustSim3DCamera(float yaw_delta, float pitch_delta,
                                 float zoom_delta) {
  Sim3DCamera_Adjust(yaw_delta, pitch_delta, zoom_delta);
  HostInput_RequestPausedRedraw();
}

void HostInput_ResetSim3DCamera(void) {
  Sim3DCamera_Reset();
  HostInput_RequestPausedRedraw();
}

void HostInput_ApplyAnalogCamera(void) {
  static const uint64_t kMaximumElapsedNs = 100000000ull;
  static const float kNanosecondsPerSecond = 1000000000.0f;
  static const float kPercentScale = 100.0f;
  static const float kYawRadiansPerSecond = 1.2f;
  static const float kPitchRadiansPerSecond = 1.2f;
  static const float kZoomUnitsPerSecond = 6.0f;
  static uint64_t last_ns;

  const uint64_t now_ns = SDL_GetTicksNS();
  uint64_t elapsed_ns = last_ns ? now_ns - last_ns : 0;
  last_ns = now_ns;
  /* A long stall (load, alt-tab) must not teleport the camera. */
  if (elapsed_ns > kMaximumElapsedNs) elapsed_ns = kMaximumElapsedNs;
  if (!elapsed_ns) return;

  const bool diorama =
      !SettingsOverlay_IsOpen() && Diorama_IsActiveThisFrame();
  const bool sim3d = !SettingsOverlay_IsOpen() && !diorama &&
      Sim3DCamera_ControlsAvailable(g_sim3d_textures_ready);

  const float elapsed_seconds =
      (float)elapsed_ns / kNanosecondsPerSecond;
  const float gain =
      (float)g_settings.input_cam_sensitivity / kPercentScale;

  float yaw = InputMap_AnalogAction(kInputAction_CamYawRight) -
              InputMap_AnalogAction(kInputAction_CamYawLeft);
  float pitch = InputMap_AnalogAction(kInputAction_CamPitchDown) -
                InputMap_AnalogAction(kInputAction_CamPitchUp);
  const float zoom = InputMap_AnalogAction(kInputAction_CamZoomOut) -
                     InputMap_AnalogAction(kInputAction_CamZoomIn);
  if (g_settings.input_cam_invert_y) pitch = -pitch;
  const bool orbit_input = yaw != 0.0f || pitch != 0.0f;
  const bool camera_input = orbit_input || zoom != 0.0f;

  const float yaw_delta =
      yaw * kYawRadiansPerSecond * gain * elapsed_seconds;
  const float pitch_delta =
      pitch * kPitchRadiansPerSecond * gain * elapsed_seconds;
  const float zoom_delta =
      zoom * kZoomUnitsPerSecond * gain * elapsed_seconds;
  if (diorama && camera_input)
    Diorama_AdjustCamera(yaw_delta, pitch_delta, zoom_delta);
  else if (sim3d && camera_input)
    HostInput_AdjustSim3DCamera(yaw_delta, pitch_delta, zoom_delta);

  const bool diorama_orbit_held =
      diorama && g_settings.diorama_camera_mode == kDioramaCam_Dynamic &&
      (orbit_input || Diorama_IsDragging());
  const bool sim_orbit_held =
      sim3d && g_settings.sim3d_camera_mode == kSimCam_Dynamic &&
      (orbit_input || Sim3DCamera_IsDragging());
  const bool diorama_changed = Diorama_UpdateDynamicCamera(
      elapsed_seconds, diorama_orbit_held);
  const bool sim_changed = Sim3DCamera_UpdateDynamic(
      elapsed_seconds, sim_orbit_held);
  if (((diorama || sim3d) && camera_input) ||
      (diorama && diorama_changed) ||
      (sim3d && sim_changed))
    HostInput_RequestPausedRedraw();
}

static void OnGamepadHostAction(InputAction action) {
  switch (action) {
    case kInputAction_Menu:
      if (SettingsOverlay_IsOpen()) {
        SettingsOverlay_Close();
      } else {
        HostInput_ClearHeld();
        SettingsOverlay_Open();
      }
      break;
    case kInputAction_Pause:
      HostInput_TogglePause();
      break;
    case kInputAction_CamReset:
      if (Diorama_IsActiveThisFrame()) {
        Diorama_ResetCamera();
      } else if (Sim3DCamera_ControlsAvailable(
                     g_sim3d_textures_ready)) {
        HostInput_ResetSim3DCamera();
      }
      break;
    case kInputAction_Turbo:
      HostInput_ToggleTurbo();
      break;
    case kInputAction_SaveState:
      (void)RuntimeSettings_HandleAction(Settings_Find("save_state"));
      break;
    case kInputAction_LoadState:
      (void)RuntimeSettings_HandleAction(Settings_Find("load_state"));
      break;
    case kInputAction_MagicCycle:
      /* Only records the request. The selection write and the OBJ tile
       * reload belong to the game thread's frame boundary, where nothing
       * else is touching WRAM or VRAM. */
      ActRaiser_RequestMagicCycle();
      break;
    default:
      break;
  }
}

void HostInput_InstallActionHandler(void) {
  InputMap_SetActionHandler(OnGamepadHostAction);
}
