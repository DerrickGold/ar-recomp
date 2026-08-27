#ifndef INPUT_MAP_H
#define INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#include "settings.h"
#include "snesrecomp/game/types.h"

/* Host input mapping: the single owner of "what the player physically did" ->
 * "which SNES joypad bits are held". Everything above this layer (the runner's
 * ComputeGameInputs, the oracle record/replay path) still sees one 12-bit
 * word in the runner's own bit order, so rebinding never reaches the emulated
 * side.
 *
 * The first twelve actions ARE that bit order (bit index == enum value), which
 * is the layout SwapInputBits translates into the SNES auto-joypad word. The
 * host actions after them are gamepad-only (except MagicCycle and the render
 * comparison control, see below):
 * keyboard hotkeys (Esc/F1, P, T, F5, F7) stay hard-wired in main.c so a
 * rebind cannot strand a desktop player without a way back into the menu. On a
 * Steam Deck there is no keyboard at all, so the pad needs its own way to
 * reach the overlay. */
typedef enum {
  kInputAction_B = 0,
  kInputAction_Y,
  kInputAction_Select,
  kInputAction_Start,
  kInputAction_Up,
  kInputAction_Down,
  kInputAction_Left,
  kInputAction_Right,
  kInputAction_A,
  kInputAction_X,
  kInputAction_L,
  kInputAction_R,
  kInputAction_PadCount,

  /* Edge-triggered host actions: fire once on the press. */
  kInputAction_Menu = kInputAction_PadCount,
  kInputAction_Pause,
  kInputAction_Turbo,
  kInputAction_SaveState,
  kInputAction_LoadState,
  kInputAction_CamReset,
  /* Debug aid, not gameplay: cycles the action-stage spell selection. Inert
   * unless the "Cycle magic spell" cheat is armed, so binding it costs a
   * normal player nothing. Unlike the other host actions this one HAS a
   * keyboard row — it is a testing control a desktop developer reaches for,
   * and it cannot strand anyone the way a rebound menu key could. */
  kInputAction_MagicCycle,
  /* Session-only authentic/enhanced comparison. It has both keyboard and pad
   * rows because press duration selects its picture-in-picture hold mode. */
  kInputAction_RenderCompare,
  kInputAction_EdgeEnd,

  /* Analog camera actions. Unlike everything above these are POLLED, not
   * edge-driven: main.c asks for each one's 0..1 magnitude once per host
   * iteration and integrates it over real elapsed time, so a stick held
   * half-way orbits at half speed. Dynamic Cam accepts the same controls but
   * treats orbit as a returning transient offset. */
  kInputAction_CamYawLeft = kInputAction_EdgeEnd,
  kInputAction_CamYawRight,
  kInputAction_CamPitchUp,
  kInputAction_CamPitchDown,
  kInputAction_CamZoomIn,
  kInputAction_CamZoomOut,
  kInputAction_Count,
} InputAction;

/* Analog actions are excluded from the edge-dispatch path; a bound trigger
 * must not also fire a one-shot every time it crosses the threshold. */
#define INPUT_ACTION_IS_ANALOG(a) ((a) >= kInputAction_EdgeEnd)

/* Which physical device a binding row belongs to. Bindings are stored per
 * class, so a keyboard and a pad can be bound simultaneously and the
 * "Input device" row only decides which of them feeds the game. */
typedef enum {
  kInputClass_Keyboard = 0,
  kInputClass_Gamepad,
  kInputClass_Count,
} InputClass;

typedef enum {
  /* Both remain available, but meaningful gamepad activity wins over
   * simultaneous keyboard input instead of ORing the two sources together. */
  kInputDevice_Auto = 0,
  kInputDevice_Keyboard,
  kInputDevice_Gamepad,
  kInputDevice_Count,
} InputDeviceMode;

/* User-facing analog ranges shared by the settings descriptor and the input
 * evaluator. Keeping validation and runtime defense on these same symbols
 * prevents an accepted setting from acquiring a different meaning in the
 * event path. */
enum {
  kInputStickDeadzoneDefaultPercent = 35,
  kInputStickDeadzoneMinimumPercent = 5,
  kInputStickDeadzoneMaximumPercent = 90,
  kInputCameraDeadzoneDefaultPercent = 12,
  kInputCameraDeadzoneMinimumPercent = 0,
  kInputCameraDeadzoneMaximumPercent = 60,
  kInputAxisMaximumMagnitude = 32767,
};

/* Packed binding word, stored in g_settings and serialized through the
 * descriptor's parse/format pair:
 *   bits 24-27  kind (InputBindKind)
 *   bit  16     axis direction is negative
 *   bits 0-15   SDL_Scancode / SDL_GamepadButton / SDL_GamepadAxis */
enum {
  kInputBind_None = 0,
  kInputBind_Key = 1,
  kInputBind_PadButton = 2,
  kInputBind_PadAxis = 3,
};

#define INPUT_BIND_KIND(word)  (((word) >> 24) & 0xF)
#define INPUT_BIND_CODE(word)  ((int)((word) & 0xFFFF))
#define INPUT_BIND_NEG(word)   (((word) & 0x10000u) != 0)
#define INPUT_BIND_MAKE(kind, code, negative) \
  (((uint32)(kind) << 24) | ((negative) ? 0x10000u : 0u) | \
   ((uint32)(code) & 0xFFFFu))

/* Boot-time: opens every already-connected gamepad and applies any
 * gamecontrollerdb.txt sitting next to the ROM. Safe to call headless. */
void InputMap_Init(void);
void InputMap_Shutdown(void);

/* Feeds one SDL event. Returns true when the event was a device/binding event
 * this layer owns (the caller may still want to look at it for other reasons;
 * the return value is informational, not a consume flag). */
void InputMap_HandleEvent(const SDL_Event *event);
/* Keyboard path, kept separate because main.c's hotkey chain also wants the
 * event and ordering there matters. `scancode` is SDL_Scancode; `repeated`
 * is SDL's key-down auto-repeat flag and is false for key-up. */
void InputMap_HandleKey(int scancode, bool pressed, bool repeated);
/* Selected-gamepad button path. Exposed beside the keyboard path so its
 * press-edge contract can be tested without attaching a physical pad. */
void InputMap_HandlePadButton(SDL_GamepadButton button, bool pressed);

/* Resolves the two physical sources under the device policy. Kept public as a
 * pure function so the simultaneous-input contract is testable without
 * attaching a real controller. */
uint32 InputMap_ArbitrateState(InputDeviceMode mode, bool gamepad_connected,
                               bool gamepad_active, uint32 keyboard_state,
                               uint32 gamepad_state);
bool InputMap_ShouldAcceptKeyboard(InputDeviceMode mode,
                                   bool gamepad_connected,
                                   bool gamepad_active);

/* True while the selected pad has any meaningful button/axis activity. This
 * combines retained event state with SDL's live device state, allowing a
 * keyboard event queued just before its gamepad twin to still be suppressed. */
bool InputMap_GamepadIsActive(void);

/* Current joypad word in runner bit order, already arbitrated under the
 * configured device mode and selected pad slot. */
uint32 InputMap_State(void);
/* Drops every held bit — used wherever main.c freezes the game (menu open,
 * inspector selection) so a held direction cannot leak across the freeze. */
void InputMap_Clear(void);

/* Current 0..1 magnitude of a polled analog action, already gated on the
 * device mode. A bound stick axis scales with deflection past the camera
 * deadzone; a bound button or key is all-or-nothing. */
float InputMap_AnalogAction(InputAction action);

/* Physical held state across both host-binding classes. Host comparison uses
 * this beside its press edge to distinguish click from hold. */
bool InputMap_ActionHeld(InputAction action);

/* Stateful threshold used by both event edges and held-state polling. Kept
 * pure/public so the press/release hysteresis contract can be regression
 * tested without attaching a physical controller. */
bool InputMap_AxisBindingHeld(uint32 binding, int value, bool was_held);

/* Host-action dispatch (gamepad only; see the header comment). Fires on the
 * press edge. */
typedef void (*InputMapActionFn)(InputAction action);
void InputMap_SetActionHandler(InputMapActionFn handler);

/* --- Binding rows -------------------------------------------------------- */

/* The descriptor's field pointer identifies the row; these resolve it back. */
bool InputMap_DescribeRow(const SettingDesc *desc, InputAction *action,
                          InputClass *klass);
const char *InputMap_ActionLabel(InputAction action);
uint32 InputMap_DefaultBinding(InputAction action, InputClass klass);

/* The parse/format pair every binding descriptor in settings.c points at. */
bool InputMap_ParseBindingField(const char *text, void *field);
int InputMap_FormatBindingField(char *buffer, int buffer_size,
                                const void *field);

/* Reverse lookup for the settings overlay: which bound action (if any) does
 * this gamepad event represent, and is it a press or a release edge? Lets the
 * menu be driven with the player's OWN layout — after a rebind the menu
 * follows the pad instead of staying on the factory buttons. Only edges are
 * reported; there is no auto-repeat. */
bool InputMap_ActionForEvent(const SDL_Event *event, InputAction *action,
                             bool *pressed);

/* Turns one SDL event into a binding word for `klass`, for the overlay's
 * capture mode. Returns false for events that are not a usable binding
 * (key-up, axis below threshold, a pad that is not the selected one). */
bool InputMap_DecodeEvent(const SDL_Event *event, InputClass klass,
                          uint32 *binding);

/* Writes `binding` into `desc` through Settings_SetText (so the ini
 * round-trip and the change observer stay on one path), first clearing any
 * other row in the same class that already held it. */
SettingChangeResult InputMap_ApplyBinding(const SettingDesc *desc,
                                          uint32 binding);

/* The exact text written to settings.ini. Keys persist as "Key #<scancode>"
 * (plus a "(Name)" hint the reader ignores) because SDL scancode NAMES are
 * documented as platform-unstable and would silently reset a binding when a
 * config moves between platforms. */
int InputMap_FormatBinding(char *buffer, int buffer_size, uint32 binding);
/* Pretty form for the settings menu — a key reads as "Key Return", without
 * the numeric storage spelling. */
int InputMap_DescribeBinding(char *buffer, int buffer_size, uint32 binding);
bool InputMap_ParseBinding(const char *text, uint32 *binding);

/* Connected-pad enumeration for the "Gamepad" row. Slot 0 is "first
 * connected"; slots 1..N name a specific pad. */
int InputMap_GamepadCount(void);
const char *InputMap_GamepadName(int slot);

#endif  /* INPUT_MAP_H */
