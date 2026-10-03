#include "host/host_ppu_output.h"
#include "host/host_input.h"
#include "host/host_clock.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actraiser/actraiser_rtl.h"
#include "actraiser/actraiser_sim_menu.h"
#include "snesrecomp/game/bootstrap.h"
#include "diorama/diorama.h"
#include "app/forced_input.h"
#include "app/input_map.h"
#include "present/present.h"
#include "app/runtime_settings.h"
#include "app/runtime_diagnostics.h"
#include "present/display_geometry.h"
#include "present/render_comparison.h"
#include "app/session_fatal.h"
#include "dev/host_dev_tools.h"
#include "dev/scene_inspector.h"
#include "app/settings.h"
#include "settings_overlay/settings_overlay.h"
#include "sim/sim3d/sim3d.h"
#include "constants.h"
#include "host/host_display.h"
#include "host/host_video.h"
#include "app/user_data_dir.h"
#include "manual/manual_reader.h"
#include "actraiser_game.h"
#include "snesrecomp/game/runtime.h"

#include "sim/sim3d/sim3d_textures.h"

static bool s_turbo;

static bool s_paused;
static bool s_inspector_owns_pause;
static bool s_paused_redraw_pending;
static uint32_t s_input_state;
static bool s_logged_key_down;
static bool s_logged_mouse_down;
static bool s_logged_suppressed_key;

void HostInput_LogStatus(const char *reason) {
  static const char *const kDeviceNames[] = {"Auto", "Keyboard", "Gamepad"};
  const int mode = g_settings.input_device;
  SDL_Window *keyboard = SDL_GetKeyboardFocus();
  SDL_Window *mouse = SDL_GetMouseFocus();
  fprintf(stderr,
      "[input] %s: ms=%llu window=%u keyboard-focus=%u mouse-focus=%u flags=$%llx "
      "mode=%s gamepads=%d pad-active=%d key-suppressed=%d "
      "overlay=%d capture=%d paused=%d inspector=%d inspector-selection=%d "
      "inspector-owns-pause=%d key-seen=%d mouse-click-seen=%d\n",
      reason, (unsigned long long)SDL_GetTicks(),
      g_window ? (unsigned)SDL_GetWindowID(g_window) : 0,
      keyboard ? (unsigned)SDL_GetWindowID(keyboard) : 0,
      mouse ? (unsigned)SDL_GetWindowID(mouse) : 0,
      (unsigned long long)(g_window ? SDL_GetWindowFlags(g_window) : 0),
      mode >= 0 && mode < kInputDevice_Count ? kDeviceNames[mode] : "Unknown",
      InputMap_GamepadCount(), InputMap_GamepadIsActive(),
      HostInput_KeyboardIsSuppressed(), SettingsOverlay_IsOpen(),
      SettingsOverlay_IsCapturing(), s_paused, g_settings.scene_inspector,
      SceneInspector_HasSelection(), s_inspector_owns_pause,
      s_logged_key_down, s_logged_mouse_down);
}

void HostInput_HandleKeyboard(int scancode, bool pressed, bool repeated) {
  InputMap_HandleKey(scancode, pressed, repeated);
  s_input_state = InputMap_State();
}

void HostInput_ClearHeld(void) {
  InputMap_Clear();
  s_input_state = 0;
}

uint32_t HostInput_SampleLiveInputs(void) {
  /* Re-read rather than trusting the last event: a gamepad's held bits are
   * owned by input_map.c and change without a keyboard event ever firing. */
  s_input_state = InputMap_State();
  if (ActRaiserSimMenu_OwnsInput()) {
    s_input_state &= ~(1u << kInputAction_X);
    if (InputMap_GameActionHeld(kInputAction_SimDescribe))
      s_input_state |= 1u << kInputAction_X;
  }
  return ForcedInput_Apply(s_input_state, snes_frame_counter);
}

uint32_t HostInput_ResolveActionInputs(uint32_t input) {
  /* Action-only producer owns the tick counter; no main-thread input state. */
  return ForcedInput_Apply(input, snes_frame_counter);
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
  return s_turbo;
}

void HostInput_TogglePause(void) {
  s_paused = !s_paused;
  fprintf(stderr, "[pause] %s\n", s_paused ? "on" : "off");
}

void HostInput_ToggleTurbo(void) {
  s_turbo = !s_turbo;
  if (s_turbo)
    fprintf(stderr, "[turbo] ON (%dx)\n", g_settings.turbo_multiplier);
  else
    fprintf(stderr, "[turbo] off\n");
}

void HostInput_RequestPausedRedraw(void) {
  s_paused_redraw_pending = true;
}

/* Camera pose is presentation-owned and can refresh a retained live frame
 * without redrawing the emulated scene. Only frozen emulation needs the legacy
 * redraw request to produce a new retained frame immediately. Marking every
 * live mouse event as a content redraw suppresses retained re-presentation
 * until the next ~60 Hz tick, which makes the FPS counter collapse while the
 * camera is moving. */
static void RequestCameraRedrawIfFrozen(void) {
  if (s_paused || SettingsOverlay_IsOpen())
    HostInput_RequestPausedRedraw();
}

bool HostInput_IsPausedRedrawPending(void) {
  return s_paused_redraw_pending;
}

bool HostInput_RedrawPausedFrameIfNeeded(void) {
  if ((!s_paused && !SettingsOverlay_IsOpen() &&
       !RenderComparison_FreezesGameplay()) ||
      !s_paused_redraw_pending) {
    return false;
  }
  (void)RtlGameDrawPpuFrame();
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
  if (!had_selection) HostInput_LogStatus("inspector-selected");
}

void HostInput_CloseInspectorSelection(void) {
  const bool had_selection = SceneInspector_HasSelection();
  SceneInspector_Clear();
  SettingsOverlay_HideDebugPanel();
  HostDevTools_ClearInspectorPresentation();
  if (s_inspector_owns_pause) s_paused = false;
  s_inspector_owns_pause = false;
  HostInput_ClearHeld();
  if (had_selection) HostInput_LogStatus("inspector-cleared");
}

void HostInput_AdjustSim3DCamera(float yaw_delta, float pitch_delta,
                                 float zoom_delta) {
  Sim3DCamera_Adjust(yaw_delta, pitch_delta, zoom_delta);
  RequestCameraRedrawIfFrozen();
}

void HostInput_ResetSim3DCamera(void) {
  Sim3DCamera_Reset();
  RequestCameraRedrawIfFrozen();
}

/* Both host loops share a clock, including handoffs between them. Streamed
 * presentation may only touch host camera state, never live runner/SIM state. */
static void ApplyAnalogCamera(bool diorama, bool sim3d, bool update_sim) {
  enum { kMaximumAnalogCameraElapsedMs = 100 };
  static const uint64_t kMaximumElapsedNs =
      (uint64_t)kMaximumAnalogCameraElapsedMs *
      kNanosecondsPerMillisecond;
  static const float kYawRadiansPerSecond = 1.2f;
  static const float kPitchRadiansPerSecond = 1.2f;
  static const float kZoomUnitsPerSecond = 6.0f;
  static uint64_t last_ns;

  const uint64_t now_ns = HostClock_Nanoseconds();
  uint64_t elapsed_ns = last_ns ? now_ns - last_ns : 0;
  last_ns = now_ns;
  /* A long stall (load, alt-tab) must not teleport the camera. */
  if (elapsed_ns > kMaximumElapsedNs) elapsed_ns = kMaximumElapsedNs;
  if (!elapsed_ns) return;

  const float elapsed_seconds =
      (float)elapsed_ns / (float)kNanosecondsPerSecond;
  const float gain =
      (float)g_settings.input_cam_sensitivity / (float)kPercentScale;

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

  const bool diorama_input_active =
      diorama && g_settings.diorama_camera_mode == kDioramaCam_Dynamic &&
      camera_input;
  const bool sim_orbit_held =
      sim3d && (orbit_input || Sim3DCamera_IsDragging());
  const bool diorama_changed = Diorama_UpdateDynamicCamera(
      elapsed_seconds, diorama_input_active);
  const bool sim_changed = update_sim && Sim3DCamera_UpdateDynamic(
      elapsed_seconds, sim_orbit_held);
  if ((diorama && camera_input) || (diorama && diorama_changed))
    RequestCameraRedrawIfFrozen();
  if (sim3d && sim_changed)
    RequestCameraRedrawIfFrozen();
}

void HostInput_ApplyAnalogCamera(void) {
  const bool controls_enabled = !SettingsOverlay_IsOpen() &&
      !RenderComparison_FreezesGameplay();
  const bool diorama = controls_enabled && Diorama_IsActiveThisFrame();
  const bool sim3d = controls_enabled && !diorama &&
      Sim3DCamera_ControlsAvailable(Sim3DTextures_Ready());
  ApplyAnalogCamera(diorama, sim3d, true);
}

void HostInput_ApplyDioramaPresentationCamera(void) {
  const bool controls_enabled = !SettingsOverlay_IsOpen() &&
      !RenderComparison_FreezesGameplay();
  ApplyAnalogCamera(controls_enabled, false, false);
}

static bool AuthenticFrameReady(void) {
  const uint64_t captured = HostPpuOutput_AuthenticFrameSerial();
  return captured != 0 &&
      PresentAuthenticUploadedFrameSerial() == captured;
}

void HostInput_UpdateRenderComparison(void) {
  if (RenderComparison_RequiresAuthenticFrame() &&
      !HostPpuOutput_AuthenticEnabled()) {
    SessionFatal_Request(
        "Authentic comparison could not create a native rendering surface. "
        "Restart the game. If this happens again, update your graphics "
        "driver or select a different SDL renderer.");
    return;
  }
  RenderComparison_Tick(
      SDL_GetTicks(), InputMap_ActionHeld(kInputAction_RenderCompare),
      AuthenticFrameReady());
  if (RenderComparison_IsAwaitingAuthenticFrame())
    HostInput_RequestPausedRedraw();
  if (RenderComparison_AuthenticWaitExpired()) {
    SessionFatal_Request(
        "Authentic comparison could not produce a current native-camera "
        "frame within %d ms. Restart the game; if the problem repeats, "
        "disable the comparison binding and report the current room and "
        "graphics settings.",
        kRenderComparisonAuthenticWaitMilliseconds);
  }
}

bool HostInput_RenderComparisonOwnsPause(void) {
  return RenderComparison_FreezesGameplay();
}

bool HostInput_RenderComparisonCaptureRequired(void) {
  const bool configured =
      g_settings.input_bind[kInputClass_Keyboard]
                           [kInputAction_RenderCompare] != 0 ||
      g_settings.input_bind[kInputClass_Gamepad]
                           [kInputAction_RenderCompare] != 0;
  return configured || RenderComparison_RequiresAuthenticFrame();
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
                     Sim3DTextures_Ready())) {
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
    case kInputAction_RenderCompare:
      if (!SettingsOverlay_IsOpen()) {
        RenderComparison_OnPress(SDL_GetTicks());
        fprintf(stderr, "[compare] pressed; waiting for click or hold\n");
      }
      break;
    default:
      break;
  }
}

void HostInput_BeginSession(void) {
  InputMap_Init();
  s_paused = s_turbo = s_inspector_owns_pause = s_paused_redraw_pending = false;
  HostInput_ClearHeld();
  InputMap_SetActionHandler(OnGamepadHostAction);
  s_logged_key_down = s_logged_mouse_down = s_logged_suppressed_key = false;
  HostInput_LogStatus("session-start");
}

void HostInput_EndSession(void) {
  InputMap_SetActionHandler(NULL);
  HostInput_ClearHeld();
  InputMap_Shutdown();
}

/* Capture wins over hotkeys; then the active menu device gets first use.
 * Suppression applies only to key-down. Key-up remains in the event pump so
 * previously accepted keys can always be released. */
static bool IsHostHotkey(SDL_Keycode key) {
  switch (key) {
    case SDLK_ESCAPE: case SDLK_F1: case SDLK_P: case SDLK_T: case SDLK_F3:
    case SDLK_MINUS: case SDLK_KP_MINUS: case SDLK_EQUALS: case SDLK_PLUS:
    case SDLK_KP_PLUS: case SDLK_F5: case SDLK_F7: case SDLK_F9: case SDLK_F6:
    case SDLK_F2: case SDLK_C: case SDLK_D:
      return true;
    default: return false;
  }
}

static void HandleKeyDown(const SDL_Event *event) {
  if (SettingsOverlay_HandleCaptureEvent(event))
    return;
  if (HostInput_KeyboardIsSuppressed()) {
    if (!s_logged_suppressed_key) {
      s_logged_suppressed_key = true;
      HostInput_LogStatus("key-down suppressed by gamepad activity");
    }
    return;
  }
  if (SettingsOverlay_IsOpen()) {
    if (HostInput_MenuKeyboardIsActive()) {
      bool was_open = true;
      bool consumed = SettingsOverlay_HandleKey(event->key.key, true,
                                                event->key.repeat != 0);
      if (was_open && !SettingsOverlay_IsOpen())
        HostInput_ClearHeld();
      if (consumed)
        return;
    } else {
      return;
    }
  }
  /* Shared with producer-time routing: a new hard-wired host command must
   * be classified above, so both paths retain its ownership barrier. */
  if (!IsHostHotkey(event->key.key)) {
    HostInput_HandleKeyboard((int)event->key.scancode, true,
                             event->key.repeat != 0);
    return;
  }
  if (!event->key.repeat &&
      (event->key.key == SDLK_ESCAPE || event->key.key == SDLK_F1)) {
    HostInput_ClearHeld();
    SettingsOverlay_Open();
  } else if (event->key.key == SDLK_P) {
    if (SceneInspector_HasSelection()) {
      const bool inspector_owned_pause = HostInput_InspectorOwnsPause();
      HostInput_CloseInspectorSelection();
      if (!inspector_owned_pause)
        HostInput_TogglePause();
    } else {
      HostInput_TogglePause();
    }
  } else if (event->key.key == SDLK_T) {
    HostInput_ToggleTurbo();
  } else if (event->key.key == SDLK_F3) {
    if (!event->key.repeat) {
      const SettingDesc *inspector = Settings_Find("scene_inspector");
      SettingChangeResult result =
          Settings_SetLong(inspector, !g_settings.scene_inspector);
      char settings_path[kHostPathCapacity];
      UserDataFile(settings_path, sizeof settings_path, "settings.ini");
      if (result > kSettingChange_Unchanged && !Settings_Save(settings_path))
        fprintf(stderr, "[scene-inspector] could not save settings.ini\n");
      fprintf(stderr, "[scene-inspector] %s (%s)\n",
              g_settings.scene_inspector ? "enabled — click the game to inspect"
                                         : "disabled",
              Settings_ChangeResultName(result));
    }
  } else if (event->key.key == SDLK_MINUS || event->key.key == SDLK_KP_MINUS) {
    if (!event->key.repeat)
      HostDevTools_AdjustHudOutputScale(-25);
  } else if (event->key.key == SDLK_EQUALS || event->key.key == SDLK_PLUS ||
             event->key.key == SDLK_KP_PLUS) {
    if (!event->key.repeat)
      HostDevTools_AdjustHudOutputScale(25);
  } else if (event->key.key == SDLK_F5) {
    (void)RuntimeSettings_HandleAction(Settings_Find("save_state"));
  } else if (event->key.key == SDLK_F7) {
    (void)RuntimeSettings_HandleAction(Settings_Find("load_state"));
  } else if (event->key.key == SDLK_F9) {
    if (event->key.repeat) {
    } else if (event->key.mod & SDL_KMOD_SHIFT) {
      DumpDiagState("hotkey");
    } else if (!g_ws_active) {
      fprintf(stderr, "[display] F9 needs ExtendedAspectRatio "
                      "(e.g. 16:9) in config.ini; staying 4:3\n");
    } else {
      int m = Settings_CycleDisplayMode();
      fprintf(stderr, "[display] mode %d/%d -> %s\n", m + 1,
              kDisplayMode_PresetCount, Settings_DisplayModeName(m));
    }
  } else if (event->key.key == SDLK_F6) {
    (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
  } else if (event->key.key == SDLK_F2 || event->key.key == SDLK_C) {
    HostDevTools_TakeFullSnapshot();
  } else if (event->key.key == SDLK_D && !event->key.repeat) {
    if (event->key.mod & SDL_KMOD_SHIFT) {
      if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
        fprintf(stderr,
                "[diorama] layer dump requires an action stage "
                "($18=%02x)\n",
                g_ram[kActRaiserWram_MapGroup]);
      } else {
        HostDevTools_ArmDioramaDump();
        fprintf(stderr, "[diorama] layer capture armed for next frame\n");
      }
    } else {
      const SettingDesc *mode = Settings_Find("diorama_mode");
      if (mode && !Settings_IsAvailable(mode)) {
        fprintf(stderr, "[diorama] requires the new renderer\n");
      } else if (mode) {
        Settings_SetLong(mode, !g_settings.diorama_mode);
        fprintf(stderr, "[diorama] %s\n",
                g_settings.diorama_mode ? "ON" : "OFF");
      }
    }
  } else {
    HostInput_HandleKeyboard((int)event->key.scancode, true,
                             event->key.repeat != 0);
  }
}

/* Manual-reader mouse input is modal. Otherwise camera controls precede
 * flat scene inspection; mouse-up releases drags independently of eligibility.
 */
static void HandleMouse(const SDL_Event *event) {
  switch (event->type) {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (!SettingsOverlay_IsOpen() && !RenderComparison_FreezesGameplay() &&
        Diorama_IsActiveThisFrame()) {
      if (event->button.button == SDL_BUTTON_RIGHT)
        Diorama_SetDragging(true);
      else if (event->button.button == SDL_BUTTON_MIDDLE)
        Diorama_ResetCamera();
    } else if (!SettingsOverlay_IsOpen() &&
               !RenderComparison_FreezesGameplay() &&
               Sim3DCamera_ControlsAvailable(Sim3DTextures_Ready())) {
      if (event->button.button == SDL_BUTTON_RIGHT)
        Sim3DCamera_SetDragging(true);
      else if (event->button.button == SDL_BUTTON_MIDDLE)
        HostInput_ResetSim3DCamera();
    } else if (!SettingsOverlay_IsOpen() && g_settings.scene_inspector) {
      if (event->button.button == SDL_BUTTON_RIGHT) {
        HostInput_CloseInspectorSelection();
      } else if (event->button.button == SDL_BUTTON_LEFT) {
        int event_x = (int)event->button.x;
        int event_y = (int)event->button.y;
        int output_x = 0, output_y = 0;
        if (!HostDisplay_WindowPointToOutput(event_x, event_y, &output_x,
                                             &output_y) ||
            !SettingsOverlay_BeginDebugPanelDrag(output_x, output_y))
          (void)HostDevTools_InspectWindowPoint(event_x, event_y);
      }
    }
    break;
  case SDL_EVENT_MOUSE_MOTION:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (!RenderComparison_FreezesGameplay() && Diorama_IsDragging() &&
        Diorama_IsActiveThisFrame()) {
      Diorama_AdjustCamera(event->motion.xrel * Diorama_DragRadPerPx(),
                           event->motion.yrel * Diorama_DragRadPerPx(), 0.0f);
    } else if (!RenderComparison_FreezesGameplay() &&
               Sim3DCamera_IsDragging() &&
               Sim3DCamera_ControlsAvailable(Sim3DTextures_Ready())) {
      HostInput_AdjustSim3DCamera(event->motion.xrel * Diorama_DragRadPerPx(),
                                  event->motion.yrel * Diorama_DragRadPerPx(),
                                  0.0f);
    } else if (SettingsOverlay_IsDebugPanelDragging()) {
      int output_x = 0, output_y = 0;
      if (HostDisplay_WindowPointToOutput(
              (int)event->motion.x, (int)event->motion.y, &output_x, &output_y))
        SettingsOverlay_DragDebugPanel(output_x, output_y);
    }
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (!SettingsOverlay_IsOpen() && !RenderComparison_FreezesGameplay() &&
        Diorama_IsActiveThisFrame())
      Diorama_AdjustCamera(0.0f, 0.0f, -event->wheel.y * Diorama_ZoomStep());
    else if (!SettingsOverlay_IsOpen() && !RenderComparison_FreezesGameplay() &&
             Sim3DCamera_ControlsAvailable(Sim3DTextures_Ready()))
      HostInput_AdjustSim3DCamera(0.0f, 0.0f,
                                  -event->wheel.y * Diorama_ZoomStep());
    break;
  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (event->button.button == SDL_BUTTON_RIGHT) {
      Diorama_SetDragging(false);
      Sim3DCamera_SetDragging(false);
    }
    if (event->button.button == SDL_BUTTON_LEFT)
      SettingsOverlay_EndDebugPanelDrag();
    break;
  }
}

bool HostInput_HandleEvent(const SDL_Event *event) {
  switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
      if (!s_logged_key_down) {
        s_logged_key_down = true;
        HostInput_LogStatus("first-key-down");
      }
      HandleKeyDown(event);
      break;
    case SDL_EVENT_TEXT_INPUT:
      if (SettingsOverlay_IsOpen())
        (void)SettingsOverlay_HandleText(event->text.text);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !s_logged_mouse_down) {
        s_logged_mouse_down = true;
        HostInput_LogStatus("first-mouse-button-down");
      }
      HandleMouse(event);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
      InputMap_HandleEvent(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:

      if (SettingsOverlay_IsOpen() &&
          SettingsOverlay_HandleCaptureEvent(event))
        break;

      if (SettingsOverlay_IsOpen()) {
        if (HostInput_MenuGamepadIsActive())
          (void)SettingsOverlay_HandleGamepadEvent(event);
        break;
      }
      InputMap_HandleEvent(event);
      break;
    case SDL_EVENT_KEY_UP:
      if (SettingsOverlay_IsOpen()) {
        if (HostInput_MenuKeyboardIsActive())
          (void)SettingsOverlay_HandleKey(event->key.key, false, false);
      } else {
        HostInput_HandleKeyboard((int)event->key.scancode, false, false);
      }
      break;
    default:
      return false;
  }
  return true;
}

bool HostInput_TryHandleGameOnlyEvent(const SDL_Event *event) {
  if (!event || s_paused || SettingsOverlay_IsOpen() ||
      SettingsOverlay_IsCapturing() || g_settings.scene_inspector ||
      RenderComparison_FreezesGameplay()) return false;
  if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
    if (IsHostHotkey(event->key.key) ||
        InputMap_KeyHasHostBinding(event->key.scancode)) return false;
    /* Preserve normal device arbitration, suppression, repeat and key-up
     * behavior. Classification above guarantees no host callback can run. */
    return HostInput_HandleEvent(event);
  }
  return InputMap_TryHandleGameOnlyEvent(event);
}

void HostInput_ApplySetting(const SettingDesc *desc) {
  if (desc->field == &g_settings.scene_inspector &&
      !g_settings.scene_inspector)
    HostInput_CloseInspectorSelection();
}
