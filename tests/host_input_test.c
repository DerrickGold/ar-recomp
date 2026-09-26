/* Characterize modal input routing without a window, renderer or game. */
#include "host/host_input.h"
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
#include "present/presentation_textures.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool s_overlay, s_capture, s_pad_active, s_manual, s_diorama;
static bool s_close_menu, s_diorama_drag, s_sim_drag;
static int s_pads, s_keys, s_pad_events, s_menu_keys, s_menu_pads;
static int s_clears, s_mouse, s_text, s_actions;
static const char *s_last_setting;
static InputMapActionFn s_action_handler;
static SettingDesc s_setting;

static const ActRaiserDisplayGeometry s_geometry;
const ActRaiserDisplayGeometry *const g_actraiser_display_geometry = &s_geometry;
uint8 g_ram[0x20000];
Settings g_settings;
bool g_sim3d_textures_ready;
int snes_frame_counter;

bool ActRaiserSimMenu_OwnsInput(void) {
  return 0;
}
bool ActRaiser_AuthenticCaptureEnabled(void) {
  return 0;
}
uint64_t ActRaiser_AuthenticFrameSerial(void) {
  return 0;
}
void ActRaiser_RequestMagicCycle(void) {
}
void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
}
float Diorama_DragRadPerPx(void) {
  return 0;
}
bool Diorama_IsActiveThisFrame(void) {
  return s_diorama;
}
bool Diorama_IsDragging(void) {
  return 0;
}
void Diorama_ResetCamera(void) {
}
void Diorama_SetDragging(bool dragging) {
  s_diorama_drag = dragging;
}
bool Diorama_UpdateDynamicCamera(float elapsed_seconds, bool orbit_held) {
  return 0;
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
  return 0;
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
  return 0;
}
void InputMap_Clear(void) {
  ++s_clears;
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
}
bool SceneInspector_HasSelection(void) {
  return 0;
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

  /* Keyboard and pad save-state commands reach the same application action. */
  Key(SDL_EVENT_KEY_DOWN, SDLK_F5);
  assert(s_actions == 1 && !strcmp(s_last_setting, "save_state"));
  HostInput_InstallActionHandler();
  assert(s_action_handler);
  s_action_handler(kInputAction_SaveState);
  assert(s_actions == 2 && !strcmp(s_last_setting, "save_state"));
  puts("host input: modal routing, device arbitration and releases passed");
  return 0;
}
