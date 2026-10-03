/* Characterize modal input routing without a window, renderer or game. */
#include "host/host_ppu_output.h"
#include "host/host_input.h"
#include "host/host_clock.h"
#include "actraiser/actraiser_rtl.h"
#include "actraiser/actraiser_sim_menu.h"
#include "snesrecomp/game/bootstrap.h"
#include "diorama/diorama.h"
#include "app/forced_input.h"
#include "replacements/hd_replacement_host.h"
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
#include "app/user_data_dir.h"
#include "manual/manual_reader.h"
#include "actraiser_game.h"
#include "snesrecomp/game/runtime.h"
#include "sim/sim3d/sim3d_textures.h"

#include <SDL3/SDL.h>
#include "support/test_assert.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static bool s_overlay, s_capture, s_pad_active, s_manual, s_diorama;
static bool s_close_menu, s_diorama_drag, s_sim_drag;
static bool s_inspector_selection;
static int s_pads, s_keys, s_pad_events, s_menu_keys, s_menu_pads;
static int s_clears, s_mouse, s_text, s_actions;
static const char *s_last_setting;
static InputMapActionFn s_action_handler;
static bool s_input_open;
static SettingDesc s_setting;
static InputAction s_analog_action;
static float s_analog_value, s_camera_zoom;
static bool s_dynamic_input_active;
static bool s_key_host_binding;
static uint64_t s_now_ns = 1000000000;
static int s_runner_camera_reads;
static DioramaCameraManualState s_manual_camera;

/* Advance host time deterministically without sleeping or running game ticks. */
uint64_t HostClock_Nanoseconds(void) { return s_now_ns; }

static const ActRaiserDisplayGeometry s_geometry;
const ActRaiserDisplayGeometry *const g_actraiser_display_geometry = &s_geometry;
uint8 g_ram[0x20000];
Settings g_settings;
SDL_Window *g_window;
bool Sim3DTextures_Ready(void) { return false; }
int snes_frame_counter;

bool ActRaiserSimMenu_OwnsInput(void) {
  return 0;
}
bool HostPpuOutput_AuthenticEnabled(void) {
  return 0;
}
uint64_t HostPpuOutput_AuthenticFrameSerial(void) {
  return 0;
}
void ActRaiser_RequestMagicCycle(void) {
}
void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
  s_camera_zoom = d_zoom;
  if (g_settings.diorama_camera_mode == kDioramaCam_Dynamic) {
    s_manual_camera.offset.tilt_y += d_yaw;
    s_manual_camera.offset.tilt_x += d_pitch;
    s_manual_camera.offset.distance += d_zoom;
    DioramaCameraManual_Input(&s_manual_camera);
  }
}
float Diorama_DragRadPerPx(void) {
  return 0;
}
bool Diorama_IsActiveThisFrame(void) {
  ++s_runner_camera_reads;
  return s_diorama;
}
bool Diorama_IsDragging(void) {
  return s_diorama_drag;
}
void Diorama_ResetCamera(void) {
}
void Diorama_SetDragging(bool dragging) {
  s_diorama_drag = dragging;
}
bool Diorama_UpdateDynamicCamera(float elapsed_seconds, bool input_active) {
  s_dynamic_input_active = input_active;
  if (g_settings.diorama_camera_mode != kDioramaCam_Dynamic) {
    s_manual_camera = (DioramaCameraManualState){0};
    return false;
  }
  return DioramaCameraManual_Update(&s_manual_camera, elapsed_seconds, input_active);
}
float Diorama_ZoomStep(void) {
  return 0;
}
void DumpDiagState(const char * tag) {
}
uint32_t ForcedInput_Apply(uint32_t live_inputs, int host_frame) {
  return 0;
}
void HostDevTools_AdjustHudOutputScale(int delta_percent) {
}
void HostDevTools_ArmDioramaDump(void) {
}
void HostDevTools_ClearInspectorPresentation(void) {
}
bool HostDevTools_InspectWindowPoint(int window_x, int window_y) {
  const bool had_selection = s_inspector_selection;
  s_inspector_selection = true;
  HostInput_OnInspectorSelection(had_selection);
  return true;
}
void HostDevTools_TakeFullSnapshot(void) {
}
bool HostDisplay_WindowPointToOutput(int window_x, int window_y, int * output_x, int * output_y) {
  return 0;
}
bool InputMap_ActionHeld(InputAction action) {
  return 0;
}
float InputMap_AnalogAction(InputAction action) {
  return action == s_analog_action ? s_analog_value : 0.0f;
}
void InputMap_Clear(void) {
  ++s_clears;
}
void InputMap_Init(void) {
  assert(!s_input_open);
  s_input_open = true;
}
void InputMap_Shutdown(void) {
  assert(s_input_open && !s_action_handler);
  s_input_open = false;
}
bool InputMap_GameActionHeld(InputAction action) {
  return 0;
}
int InputMap_GamepadCount(void) {
  return s_pads;
}
bool InputMap_GamepadIsActive(void) {
  return s_pad_active;
}
void InputMap_HandleEvent(const SDL_Event * event) {
  ++s_pad_events;
}
bool InputMap_TryHandleGameOnlyEvent(const SDL_Event *event) { return false; }
bool InputMap_KeyHasHostBinding(int scancode) { return s_key_host_binding; }
void InputMap_HandleKey(int scancode, bool pressed, bool repeated) {
  ++s_keys;
}
void InputMap_SetActionHandler(InputMapActionFn handler) {
  s_action_handler = handler;
}
bool InputMap_ShouldAcceptKeyboard(InputDeviceMode mode, bool gamepad_connected,
                                   bool gamepad_active) {
  return mode == kInputDevice_Keyboard || (mode == kInputDevice_Auto && !gamepad_active);
}
uint32 InputMap_State(void) {
  return 0;
}
bool ManualReader_HandleMouse(const SDL_Event * event) {
  ++s_mouse;
  return true;
}
bool ManualReader_IsOpen(void) {
  return s_manual;
}
bool SettingsOverlay_IsCapturing(void) {
  return s_overlay && s_capture;
}
uint64_t PresentAuthenticUploadedFrameSerial(void) {
  return 0;
}
bool RenderComparison_AuthenticWaitExpired(void) {
  return 0;
}
bool RenderComparison_FreezesGameplay(void) {
  return 0;
}
bool RenderComparison_IsAwaitingAuthenticFrame(void) {
  return 0;
}
void RenderComparison_OnPress(uint64_t now_ms) {
}
bool RenderComparison_RequiresAuthenticFrame(void) {
  return 0;
}
void RenderComparison_Tick(uint64_t now_ms, bool control_held, bool authentic_frame_ready) {
}
bool RtlGameDrawPpuFrame(void) {
  return 0;
}
bool RuntimeSettings_HandleAction(const SettingDesc * desc) {
  ++s_actions;
  return true;
}
void SceneInspector_Clear(void) {
  s_inspector_selection = false;
}
bool SceneInspector_HasSelection(void) {
  return s_inspector_selection;
}
void SessionFatal_Request(const char * format, ...) {
}
bool SettingsOverlay_BeginDebugPanelDrag(int output_x, int output_y) {
  return 0;
}
void SettingsOverlay_Close(void) {
  s_overlay = false;
}
void SettingsOverlay_DragDebugPanel(int output_x, int output_y) {
}
void SettingsOverlay_EndDebugPanelDrag(void) {
}
bool SettingsOverlay_HandleCaptureEvent(const SDL_Event * event) {
  return s_capture;
}
bool SettingsOverlay_HandleGamepadEvent(const SDL_Event * event) {
  ++s_menu_pads;
  return true;
}
bool SettingsOverlay_HandleKey(SDL_Keycode key, bool pressed, bool repeat) {
  ++s_menu_keys;
  if (s_close_menu) s_overlay = false;
  return true;
}
bool SettingsOverlay_HandleText(const char * text) {
  ++s_text;
  return true;
}
void SettingsOverlay_HideDebugPanel(void) {
}
bool SettingsOverlay_IsDebugPanelDragging(void) {
  return 0;
}
bool SettingsOverlay_IsOpen(void) {
  return s_overlay;
}
void SettingsOverlay_Open(void) {
  s_overlay = true;
}
const char *Settings_ChangeResultName(SettingChangeResult result) {
  return 0;
}
int Settings_CycleDisplayMode(void) {
  return 0;
}
const char *Settings_DisplayModeName(int mode) {
  return 0;
}
const SettingDesc *Settings_Find(const char * key) {
  s_last_setting = key;
  return &s_setting;
}
bool Settings_GetLong(const SettingDesc * desc, long * value) {
  return 0;
}
bool Settings_IsAvailable(const SettingDesc * desc) {
  return 0;
}
bool Settings_Save(const char * path) {
  return 0;
}
SettingChangeResult Settings_SetLong(const SettingDesc * desc, long value) {
  return 0;
}
void Sim3DCamera_Adjust(float yaw_delta, float pitch_delta, float zoom_delta) {
}
bool Sim3DCamera_ControlsAvailable(bool textures_ready) {
  ++s_runner_camera_reads;
  return 0;
}
bool Sim3DCamera_IsDragging(void) {
  return 0;
}
void Sim3DCamera_Reset(void) {
}
void Sim3DCamera_SetDragging(bool dragging) {
  s_sim_drag = dragging;
}
bool Sim3DCamera_UpdateDynamic(float elapsed_seconds, bool orbit_held) {
  ++s_runner_camera_reads;
  return 0;
}
char *UserDataFile(char * buf, size_t size, const char * leaf) {
  return 0;
}

static void Key(Uint32 type, SDL_Keycode key) {
  SDL_Event event = {.type = type};
  event.key.key = key;
  event.key.scancode = SDL_SCANCODE_A;
  assert(HostInput_HandleEvent(&event));
}

static void CheckStreamedCamera(void) {
  const int rates[] = {60, 90, 120};
  g_settings.diorama_camera_mode = kDioramaCam_Dynamic;
  /* The retained action frame, not the live map group, authorizes this path. */
  s_diorama = false;
  const int runner_reads = s_runner_camera_reads;
  for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
    const int rate = rates[r];
    const uint64_t interval_ns = 1000000000 / rate;
    s_manual_camera = (DioramaCameraManualState){0};
    s_analog_action = kInputAction_CamZoomOut;
    s_analog_value = 1.0f;
    /* No new input events and no emulation ticks/ownership handoffs. */
    for (int i = 0; i < rate; ++i) {
      s_now_ns += interval_ns;
      HostInput_ApplyDioramaPresentationCamera();
    }
    assert(fabsf(s_manual_camera.offset.distance - 6.0f) < .0001f);
    assert(s_manual_camera.framing_override == 1.0f);
    s_analog_value = 0.0f;
    for (int i = 0; i < rate / 3; ++i) {
      s_now_ns += interval_ns;
      HostInput_ApplyDioramaPresentationCamera();
    }
    assert(s_manual_camera.framing_override == 1.0f); /* Idle grace. */
    s_now_ns += 100000000;
    HostInput_ApplyDioramaPresentationCamera();
    assert(s_manual_camera.framing_override < 1.0f);
    for (int i = 0; i < rate * 4; ++i) {
      s_now_ns += interval_ns;
      HostInput_ApplyDioramaPresentationCamera();
    }
    assert(s_manual_camera.offset.distance == 0.0f);
    assert(s_manual_camera.framing_override == 0.0f);
  }
  assert(s_runner_camera_reads == runner_reads);

  /* Switching host paths at the same timestamp cannot integrate twice. */
  s_diorama = true;
  s_analog_value = 1.0f;
  s_now_ns += 10000000;
  HostInput_ApplyAnalogCamera();
  const float zoom = s_manual_camera.offset.distance;
  HostInput_ApplyDioramaPresentationCamera();
  assert(s_manual_camera.offset.distance == zoom);
  /* Modal input suppression still applies during retained presentation. */
  s_overlay = true;
  s_now_ns += 10000000;
  HostInput_ApplyDioramaPresentationCamera();
  assert(s_manual_camera.offset.distance == zoom && !s_dynamic_input_active);
  s_overlay = false;
  s_analog_value = 0.0f;
}

int main(void) {
  SDL_Event event = {.type = SDL_EVENT_QUIT};
  assert(!HostInput_HandleEvent(&event));
  g_settings.input_device = kInputDevice_Auto;

  /* Binding capture wins; auto-mode pad activity suppresses presses only. */
  s_capture = true;
  Key(SDL_EVENT_KEY_DOWN, SDLK_A);
  assert(s_keys == 0);
  s_capture = false;
  s_pads = 1;
  s_pad_active = true;
  Key(SDL_EVENT_KEY_DOWN, SDLK_A);
  assert(s_keys == 0);
  Key(SDL_EVENT_KEY_UP, SDLK_A);
  assert(s_keys == 1);
  s_pad_active = false;
  Key(SDL_EVENT_KEY_DOWN, SDLK_A);
  assert(s_keys == 2);

  /* Menus receive keys first; closing a menu releases held gameplay input. */
  s_overlay = true;
  s_close_menu = true;
  Key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE);
  assert(s_menu_keys == 1 && s_clears == 1 && !s_overlay && s_keys == 2);
  s_close_menu = false;
  Key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE);
  assert(s_overlay && s_clears == 2);
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.text = "Name";
  assert(HostInput_HandleEvent(&event) && s_text == 1);

  /* Pad hotplug bypasses capture; pad presses obey capture and modal routing. */
  s_capture = true;
  event.type = SDL_EVENT_GAMEPAD_ADDED;
  assert(HostInput_HandleEvent(&event) && s_pad_events == 1);
  event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  assert(HostInput_HandleEvent(&event) && s_pad_events == 1 && s_menu_pads == 0);
  s_capture = false;
  assert(HostInput_HandleEvent(&event) && s_menu_pads == 1);
  s_overlay = false;
  assert(HostInput_HandleEvent(&event) && s_pad_events == 2);

  /* The manual is mouse-modal; camera drags release even after closing a view. */
  s_manual = true;
  s_diorama = true;
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_RIGHT;
  assert(HostInput_HandleEvent(&event) && s_mouse == 1 && !s_diorama_drag);
  s_manual = false;
  assert(HostInput_HandleEvent(&event) && s_diorama_drag);
  s_overlay = true;
  s_sim_drag = true;
  event.type = SDL_EVENT_MOUSE_BUTTON_UP;
  assert(HostInput_HandleEvent(&event) && !s_diorama_drag && !s_sim_drag);
  s_overlay = false;

  /* Held zoom postpones Dynamic Cam's return, while a stationary mouse drag
   * lets it return once the camera's idle grace period expires. */
  g_settings.diorama_camera_mode = kDioramaCam_Dynamic;
  g_settings.input_cam_sensitivity = 100;
  s_analog_action = kInputAction_CamZoomOut;
  s_analog_value = 1.0f;
  HostInput_ApplyAnalogCamera();
  s_now_ns += 1000000;
  HostInput_ApplyAnalogCamera();
  assert(s_camera_zoom > 0.0f && s_dynamic_input_active);
  s_analog_value = 0.0f;
  s_diorama_drag = true;
  s_now_ns += 1000000;
  HostInput_ApplyAnalogCamera();
  assert(!s_dynamic_input_active);
  s_diorama_drag = false;
  g_settings.diorama_camera_mode = kDioramaCam_Free;
  s_analog_value = 1.0f;
  s_camera_zoom = 0.0f;
  s_now_ns += 1000000;
  HostInput_ApplyAnalogCamera();
  assert(s_camera_zoom > 0.0f && !s_dynamic_input_active);
  s_analog_value = 0.0f;

  CheckStreamedCamera();

  /* Keyboard and pad save-state commands reach the same application action. */
  Key(SDL_EVENT_KEY_DOWN, SDLK_F5);
  assert(s_actions == 1 && !strcmp(s_last_setting, "save_state"));
  HostInput_BeginSession();
  assert(s_action_handler);
  s_action_handler(kInputAction_SaveState);
  assert(s_actions == 2 && !strcmp(s_last_setting, "save_state"));
  if (!HostInput_IsPaused()) HostInput_TogglePause();
  if (!HostInput_IsTurbo()) HostInput_ToggleTurbo();
  HostInput_RequestPausedRedraw();
  HostInput_EndSession();
  assert(!s_input_open && !s_action_handler);
  HostInput_BeginSession();
  assert(!HostInput_IsPaused() && !HostInput_IsTurbo());
  assert(!HostInput_IsPausedRedrawPending() && !HostInput_InspectorOwnsPause());
  HostInput_EndSession();
  assert(!s_input_open && !s_action_handler);

  /* A saved inspector toggle does not capture keyboard input. Selecting a
   * point pauses gameplay, but settings remain reachable; clearing the point
   * releases only a pause introduced by the inspector. */
  s_pads = 0;
  s_diorama = false;
  g_settings.scene_inspector = true;
  HostInput_BeginSession();
  const int keys_before_inspector = s_keys;
  Key(SDL_EVENT_KEY_DOWN, SDLK_A);
  assert(s_keys == keys_before_inspector + 1 && !HostInput_IsPaused());
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  assert(HostInput_HandleEvent(&event));
  assert(s_inspector_selection && HostInput_IsPaused());
  assert(HostInput_InspectorOwnsPause());
  assert(HostInput_HandleEvent(&event));
  assert(HostInput_InspectorOwnsPause());
  Key(SDL_EVENT_KEY_DOWN, SDLK_A);
  assert(s_keys == keys_before_inspector + 2);
  Key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE);
  assert(s_overlay && HostInput_IsPaused());
  s_close_menu = true;
  Key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE);
  assert(!s_overlay);
  s_close_menu = false;
  event.button.button = SDL_BUTTON_RIGHT;
  assert(HostInput_HandleEvent(&event));
  assert(!s_inspector_selection && !HostInput_IsPaused());
  assert(!HostInput_InspectorOwnsPause());

  HostInput_TogglePause();
  event.button.button = SDL_BUTTON_LEFT;
  assert(HostInput_HandleEvent(&event));
  assert(s_inspector_selection && !HostInput_InspectorOwnsPause());
  event.button.button = SDL_BUTTON_RIGHT;
  assert(HostInput_HandleEvent(&event));
  assert(!s_inspector_selection && HostInput_IsPaused());
  HostInput_TogglePause();
  g_settings.scene_inspector = false;
  const int keys_before_stream = s_keys;
  event = (SDL_Event){.type = SDL_EVENT_KEY_DOWN};
  event.key.key = SDLK_A; event.key.scancode = SDL_SCANCODE_A;
  assert(HostInput_TryHandleGameOnlyEvent(&event));
  event.type = SDL_EVENT_KEY_UP;
  assert(HostInput_TryHandleGameOnlyEvent(&event));
  assert(s_keys == keys_before_stream + 2);
  s_key_host_binding = true;
  assert(!HostInput_TryHandleGameOnlyEvent(&event));
  s_key_host_binding = false;
  const SDL_Keycode host_keys[] = {SDLK_ESCAPE, SDLK_P, SDLK_T, SDLK_F5,
      SDLK_F7, SDLK_F9, SDLK_D, SDLK_C, SDLK_1, SDLK_PLUS};
  for (unsigned i = 0; i < sizeof(host_keys)/sizeof(host_keys[0]); ++i) {
    event.type = SDL_EVENT_KEY_DOWN; event.key.key = host_keys[i];
    assert(!HostInput_TryHandleGameOnlyEvent(&event));
  }
  assert(s_keys == keys_before_stream + 2 && !HostInput_IsPaused() && !s_overlay);
  event.key.key = SDLK_A;
  s_pads = 1; s_pad_active = true;
  assert(HostInput_TryHandleGameOnlyEvent(&event));
  assert(s_keys == keys_before_stream + 2); /* Suppressed presses stay suppressed. */
  event.type = SDL_EVENT_KEY_UP;
  assert(HostInput_TryHandleGameOnlyEvent(&event));
  assert(s_keys == keys_before_stream + 3); /* Releases still get through. */
  s_overlay = true;
  assert(!HostInput_TryHandleGameOnlyEvent(&event));
  s_overlay = false;
  HostInput_EndSession();
  puts("host input: modal routing, device arbitration and releases passed");
  return 0;
}
