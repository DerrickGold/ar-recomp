#include "input_map.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(kInputClass_Count == kSettingsInputClasses,
               "settings.h input class count out of sync");
_Static_assert(kInputAction_Count == kSettingsInputActions,
               "settings.h input action count out of sync");

/* --- Device registry ------------------------------------------------------ */

enum { kMaxGamepads = 8 };

typedef struct {
  SDL_JoystickID id;
  SDL_Gamepad *pad;
  char name[64];
} GamepadSlot;

static GamepadSlot s_pads[kMaxGamepads];
static int s_pad_count;

/* Held state, tracked per source so the device-mode row can gate them
 * independently without either source clobbering the other's bits. */
static uint32 s_key_bits;
static uint32 s_pad_bits;
static uint32 s_stick_bits;   /* left stick emulating the D-pad */

/* Raw held state, needed by the POLLED analog camera actions: those are read
 * once per host iteration rather than dispatched from an event, so the last
 * known position of every control has to be retained. */
static bool s_key_down[SDL_SCANCODE_COUNT];
static bool s_pad_button_down[SDL_GAMEPAD_BUTTON_COUNT];
static int s_pad_axis_value[SDL_GAMEPAD_AXIS_COUNT];

static InputMapActionFn s_action_handler;

int InputMap_GamepadCount(void) { return s_pad_count; }

const char *InputMap_GamepadName(int slot) {
  if (slot < 0 || slot >= s_pad_count) return "";
  return s_pads[slot].name;
}

static int SlotForJoystick(SDL_JoystickID id) {
  for (int i = 0; i < s_pad_count; i++)
    if (s_pads[i].id == id) return i;
  return -1;
}

/* g_settings.input_gamepad_slot is 1-based with 0 meaning "first connected",
 * so an unplug/replug does not silently strand a player who never opened the
 * menu. Returns true when `id` is the pad we currently listen to. */
static bool JoystickIsSelected(SDL_JoystickID id) {
  int slot = SlotForJoystick(id);
  if (slot < 0) return false;
  int selected = g_settings.input_gamepad_slot;
  if (selected <= 0 || selected > s_pad_count) return slot == 0;
  return slot == selected - 1;
}

static void AddGamepad(SDL_JoystickID id) {
  if (SlotForJoystick(id) >= 0) return;
  if (s_pad_count >= kMaxGamepads) return;
  SDL_Gamepad *pad = SDL_OpenGamepad(id);
  if (!pad) {
    fprintf(stderr, "[input] could not open gamepad %u: %s\n",
            (unsigned)id, SDL_GetError());
    return;
  }
  GamepadSlot *slot = &s_pads[s_pad_count++];
  slot->id = id;
  slot->pad = pad;
  const char *name = SDL_GetGamepadName(pad);
  snprintf(slot->name, sizeof(slot->name), "%s", name ? name : "Gamepad");
  fprintf(stderr, "[input] gamepad %d connected: %s\n",
          s_pad_count, slot->name);
}

static void RemoveGamepad(SDL_JoystickID id) {
  int slot = SlotForJoystick(id);
  if (slot < 0) return;
  fprintf(stderr, "[input] gamepad %d disconnected: %s\n",
          slot + 1, s_pads[slot].name);
  SDL_CloseGamepad(s_pads[slot].pad);
  for (int i = slot; i + 1 < s_pad_count; i++) s_pads[i] = s_pads[i + 1];
  s_pad_count--;
  memset(&s_pads[s_pad_count], 0, sizeof(s_pads[s_pad_count]));
  /* A pad going away must not leave its bits — or a deflected stick — stuck. */
  s_pad_bits = 0;
  s_stick_bits = 0;
  memset(s_pad_button_down, 0, sizeof(s_pad_button_down));
  memset(s_pad_axis_value, 0, sizeof(s_pad_axis_value));
}

void InputMap_SetActionHandler(InputMapActionFn handler) {
  s_action_handler = handler;
}

void InputMap_Init(void) {
  /* SDL3 drives the Steam Deck's built-in controls through its HIDAPI Steam
   * driver when the game is launched OUTSIDE Steam; inside Steam, Steam Input
   * presents a virtual Xbox pad and this hint is a no-op. Enabling it is what
   * makes a desktop-mode (non-Steam) launch on the Deck see the built-in
   * sticks, D-pad, and face buttons at all. */
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");

  /* Optional community mapping database for pads SDL does not know about.
   * Absent by default; a missing file is not an error. */
  static const char *const kMappingFiles[] = {
    "gamecontrollerdb.txt", "assets/gamecontrollerdb.txt",
  };
  for (int i = 0; i < (int)(sizeof(kMappingFiles) / sizeof(kMappingFiles[0]));
       i++) {
    int added = SDL_AddGamepadMappingsFromFile(kMappingFiles[i]);
    if (added > 0) {
      fprintf(stderr, "[input] %d gamepad mappings from %s\n",
              added, kMappingFiles[i]);
      break;
    }
  }
  SDL_ClearError();

  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  if (ids) {
    for (int i = 0; i < count; i++) AddGamepad(ids[i]);
    SDL_free(ids);
  }
  if (!s_pad_count)
    fprintf(stderr, "[input] no gamepad connected; keyboard bindings active\n");
}

void InputMap_Shutdown(void) {
  for (int i = 0; i < s_pad_count; i++) SDL_CloseGamepad(s_pads[i].pad);
  s_pad_count = 0;
  memset(s_pads, 0, sizeof(s_pads));
  InputMap_Clear();
}

/* --- Defaults ------------------------------------------------------------- */

static const struct {
  const char *label;
  int key;        /* SDL_Scancode */
  int pad_kind;   /* kInputBind_* */
  int pad_code;
} kDefaults[kInputAction_Count] = {
  /* The keyboard column reproduces the pre-rebinding hard-coded layout in
   * main.c exactly, so an existing player's muscle memory survives the
   * upgrade with no settings.ini present. */
  [kInputAction_B]      = { "B jump/confirm", SDL_SCANCODE_Z,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_SOUTH },
  [kInputAction_Y]      = { "Y attack", SDL_SCANCODE_A,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_WEST },
  [kInputAction_Select] = { "Select", SDL_SCANCODE_RSHIFT,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_BACK },
  [kInputAction_Start]  = { "Start", SDL_SCANCODE_RETURN,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_START },
  [kInputAction_Up]     = { "Up", SDL_SCANCODE_UP,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_DPAD_UP },
  [kInputAction_Down]   = { "Down", SDL_SCANCODE_DOWN,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
  [kInputAction_Left]   = { "Left", SDL_SCANCODE_LEFT,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_DPAD_LEFT },
  [kInputAction_Right]  = { "Right", SDL_SCANCODE_RIGHT,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
  [kInputAction_A]      = { "A cancel", SDL_SCANCODE_X,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_EAST },
  [kInputAction_X]      = { "X magic", SDL_SCANCODE_S,
                            kInputBind_PadButton, SDL_GAMEPAD_BUTTON_NORTH },
  [kInputAction_L]      = { "L", SDL_SCANCODE_Q, kInputBind_PadButton,
                            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
  [kInputAction_R]      = { "R", SDL_SCANCODE_W, kInputBind_PadButton,
                            SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },

  /* Host actions: gamepad only. L3 is the default menu key because it is the
   * one control every Deck/Xbox/DualSense pad has that ActRaiser itself never
   * uses, and the Deck's Steam button is owned by Steam. */
  [kInputAction_Menu]      = { "Open settings menu", 0, kInputBind_PadButton,
                               SDL_GAMEPAD_BUTTON_LEFT_STICK },
  /* R3 recentres the camera, the near-universal convention for a right-stick
   * click. Pause has no pad default on purpose: every remaining button is
   * spoken for, the keyboard still has P, and the menu carries a Pause row. */
  [kInputAction_CamReset]  = { "Reset camera", 0, kInputBind_PadButton,
                               SDL_GAMEPAD_BUTTON_RIGHT_STICK },
  [kInputAction_Pause]     = { "Pause emulation", 0, kInputBind_None, 0 },
  [kInputAction_Turbo]     = { "Fast forward", 0, kInputBind_None, 0 },
  [kInputAction_SaveState] = { "Save state", 0, kInputBind_None, 0 },
  [kInputAction_LoadState] = { "Load state", 0, kInputBind_None, 0 },

  /* Right stick orbits, triggers zoom — the layout any 3D game trains for.
   * The keyboard column is left unbound: the desktop path is the mouse
   * (right-drag orbits, wheel zooms), and claiming letter keys here would
   * collide with the diorama's existing hotkeys. */
  [kInputAction_CamYawLeft]   = { "Camera yaw left", 0, kInputBind_PadAxis,
                                  SDL_GAMEPAD_AXIS_RIGHTX },
  [kInputAction_CamYawRight]  = { "Camera yaw right", 0, kInputBind_PadAxis,
                                  SDL_GAMEPAD_AXIS_RIGHTX },
  [kInputAction_CamPitchUp]   = { "Camera pitch up", 0, kInputBind_PadAxis,
                                  SDL_GAMEPAD_AXIS_RIGHTY },
  [kInputAction_CamPitchDown] = { "Camera pitch down", 0, kInputBind_PadAxis,
                                  SDL_GAMEPAD_AXIS_RIGHTY },
  [kInputAction_CamZoomIn]    = { "Camera zoom in", 0, kInputBind_PadAxis,
                                  SDL_GAMEPAD_AXIS_RIGHT_TRIGGER },
  [kInputAction_CamZoomOut]   = { "Camera zoom out", 0, kInputBind_PadAxis,
                                  SDL_GAMEPAD_AXIS_LEFT_TRIGGER },
};

/* The four stick-orbit defaults share an axis and differ only in sign, which
 * the kDefaults table has no room for. */
static bool DefaultBindingIsNegative(InputAction action) {
  return action == kInputAction_CamYawLeft ||
         action == kInputAction_CamPitchUp;
}

const char *InputMap_ActionLabel(InputAction action) {
  if (action < 0 || action >= kInputAction_Count) return "";
  return kDefaults[action].label;
}

uint32 InputMap_DefaultBinding(InputAction action, InputClass klass) {
  if (action < 0 || action >= kInputAction_Count) return 0;
  if (klass == kInputClass_Keyboard) {
    int key = kDefaults[action].key;
    return key ? INPUT_BIND_MAKE(kInputBind_Key, key, false) : 0;
  }
  int kind = kDefaults[action].pad_kind;
  return kind ? INPUT_BIND_MAKE(kind, kDefaults[action].pad_code,
                                DefaultBindingIsNegative(action)) : 0;
}

/* --- Naming --------------------------------------------------------------- */

/* SDL's own button strings are lowercase wire names ("a", "dpup"); these are
 * what the menu shows AND what lands in settings.ini, so they are spelled the
 * way a player would describe the button. Parsing accepts either.
 *
 * No digits: the ROM font renders '1' as a slanted stroke, so "L1" reads as
 * "L/" on the menu row. They are also kept short — the row label gives up
 * characters to the value column, and a long value truncates the label. */
static const char *const kPadButtonNames[SDL_GAMEPAD_BUTTON_COUNT] = {
  [SDL_GAMEPAD_BUTTON_SOUTH] = "A / south",
  [SDL_GAMEPAD_BUTTON_EAST] = "B / east",
  [SDL_GAMEPAD_BUTTON_WEST] = "X / west",
  [SDL_GAMEPAD_BUTTON_NORTH] = "Y / north",
  [SDL_GAMEPAD_BUTTON_BACK] = "View",
  [SDL_GAMEPAD_BUTTON_GUIDE] = "Guide",
  [SDL_GAMEPAD_BUTTON_START] = "Menu",
  [SDL_GAMEPAD_BUTTON_LEFT_STICK] = "L Stick In",
  [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = "R Stick In",
  [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = "L Bumper",
  [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "R Bumper",
  [SDL_GAMEPAD_BUTTON_DPAD_UP] = "D-Pad Up",
  [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = "D-Pad Down",
  [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = "D-Pad Left",
  [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = "D-Pad Right",
  [SDL_GAMEPAD_BUTTON_MISC1] = "Misc1",
  [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] = "R Paddle",
  [SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] = "L Paddle",
  [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2] = "R Paddle B",
  [SDL_GAMEPAD_BUTTON_LEFT_PADDLE2] = "L Paddle B",
  [SDL_GAMEPAD_BUTTON_TOUCHPAD] = "Touchpad",
};

/* Stick axes are named by DIRECTION rather than "axis + sign": the ROM font
 * has no '+' glyph (it renders as a double quote), and "R-Stick Up" is what a
 * player would say anyway. Triggers travel one way only, so they carry no
 * direction word. This is both the menu text and the settings.ini text. */
static const struct {
  const char *name;
  int axis;
  bool negative;
} kPadAxisDirections[] = {
  { "L-Stick Left",  SDL_GAMEPAD_AXIS_LEFTX,  true  },
  { "L-Stick Right", SDL_GAMEPAD_AXIS_LEFTX,  false },
  { "L-Stick Up",    SDL_GAMEPAD_AXIS_LEFTY,  true  },
  { "L-Stick Down",  SDL_GAMEPAD_AXIS_LEFTY,  false },
  { "R-Stick Left",  SDL_GAMEPAD_AXIS_RIGHTX, true  },
  { "R-Stick Right", SDL_GAMEPAD_AXIS_RIGHTX, false },
  { "R-Stick Up",    SDL_GAMEPAD_AXIS_RIGHTY, true  },
  { "R-Stick Down",  SDL_GAMEPAD_AXIS_RIGHTY, false },
  { "L Trigger",     SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  false },
  { "R Trigger",     SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, false },
};

static bool AxisIsTrigger(int axis) {
  return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
         axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

static const char *PadAxisDirectionName(int axis, bool negative) {
  for (int i = 0; i < (int)(sizeof(kPadAxisDirections) /
                            sizeof(kPadAxisDirections[0])); i++) {
    if (kPadAxisDirections[i].axis != axis) continue;
    if (AxisIsTrigger(axis) || kPadAxisDirections[i].negative == negative)
      return kPadAxisDirections[i].name;
  }
  return NULL;
}


int InputMap_FormatBinding(char *buffer, int buffer_size, uint32 binding) {
  int kind = INPUT_BIND_KIND(binding);
  int code = INPUT_BIND_CODE(binding);
  switch (kind) {
    case kInputBind_Key: {
      const char *name = SDL_GetScancodeName((SDL_Scancode)code);
      if (!name || !name[0]) break;
      return snprintf(buffer, buffer_size, "Key %s", name);
    }
    case kInputBind_PadButton: {
      const char *name = code >= 0 && code < SDL_GAMEPAD_BUTTON_COUNT
          ? kPadButtonNames[code] : NULL;
      if (name)
        return snprintf(buffer, buffer_size, "Pad %s", name);
      return snprintf(buffer, buffer_size, "Pad Button %d", code);
    }
    case kInputBind_PadAxis: {
      const char *name = PadAxisDirectionName(code, INPUT_BIND_NEG(binding));
      if (name) return snprintf(buffer, buffer_size, "Pad %s", name);
      return snprintf(buffer, buffer_size, "Pad Axis %d %s", code,
                      INPUT_BIND_NEG(binding) ? "-" : "+");
    }
    default:
      break;
  }
  return snprintf(buffer, buffer_size, "Unbound");
}

static bool EqualsIgnoreCase(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (ca != cb) return false;
  }
  return !*a && !*b;
}

bool InputMap_ParseBinding(const char *text, uint32 *binding) {
  if (!text || !binding) return false;
  while (*text == ' ') text++;
  if (!*text || EqualsIgnoreCase(text, "Unbound") ||
      EqualsIgnoreCase(text, "None")) {
    *binding = 0;
    return true;
  }
  if (!strncmp(text, "Key ", 4)) {
    SDL_Scancode code = SDL_GetScancodeFromName(text + 4);
    if (code == SDL_SCANCODE_UNKNOWN) return false;
    *binding = INPUT_BIND_MAKE(kInputBind_Key, code, false);
    return true;
  }
  if (strncmp(text, "Pad ", 4)) return false;
  const char *rest = text + 4;

  /* Trailing sign, if any, belongs to the axis form. */
  size_t length = strlen(rest);
  bool negative = false;
  char stem[64];
  if (length >= 2 && (rest[length - 1] == '+' || rest[length - 1] == '-') &&
      rest[length - 2] == ' ') {
    negative = rest[length - 1] == '-';
    length -= 2;
  }
  if (length >= sizeof(stem)) return false;
  memcpy(stem, rest, length);
  stem[length] = 0;

  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
    if (kPadButtonNames[i] && EqualsIgnoreCase(stem, kPadButtonNames[i])) {
      *binding = INPUT_BIND_MAKE(kInputBind_PadButton, i, false);
      return true;
    }
  }
  for (int i = 0; i < (int)(sizeof(kPadAxisDirections) /
                           sizeof(kPadAxisDirections[0])); i++) {
    if (!EqualsIgnoreCase(stem, kPadAxisDirections[i].name)) continue;
    *binding = INPUT_BIND_MAKE(kInputBind_PadAxis, kPadAxisDirections[i].axis,
                               kPadAxisDirections[i].negative);
    return true;
  }
  /* Numeric fallbacks keep a hand-edited ini from being silently dropped. */
  int code = 0;
  if (sscanf(stem, "Button %d", &code) == 1) {
    *binding = INPUT_BIND_MAKE(kInputBind_PadButton, code, false);
    return true;
  }
  if (sscanf(stem, "Axis %d", &code) == 1) {
    *binding = INPUT_BIND_MAKE(kInputBind_PadAxis, code, negative);
    return true;
  }
  /* SDL's own wire names, for anyone hand-editing from an SDL mapping. */
  SDL_GamepadButton button = SDL_GetGamepadButtonFromString(stem);
  if (button != SDL_GAMEPAD_BUTTON_INVALID) {
    *binding = INPUT_BIND_MAKE(kInputBind_PadButton, button, false);
    return true;
  }
  SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(stem);
  if (axis != SDL_GAMEPAD_AXIS_INVALID) {
    *binding = INPUT_BIND_MAKE(kInputBind_PadAxis, axis, negative);
    return true;
  }
  return false;
}

/* --- Descriptor plumbing -------------------------------------------------- */

bool InputMap_DescribeRow(const SettingDesc *desc, InputAction *action,
                          InputClass *klass) {
  if (!desc || desc->type != kSettingType_Binding || !desc->field)
    return false;
  const uint32 *field = (const uint32 *)desc->field;
  const uint32 *base = &g_settings.input_bind[0][0];
  ptrdiff_t index = field - base;
  if (index < 0 || index >= kSettingsInputClasses * kSettingsInputActions)
    return false;
  if (klass) *klass = (InputClass)(index / kSettingsInputActions);
  if (action) *action = (InputAction)(index % kSettingsInputActions);
  return true;
}

/* Shared by every binding descriptor: "" restores that row's default, which
 * is why the row identity is recovered from the field pointer. */
bool InputMap_ParseBindingField(const char *text, void *field) {
  uint32 *slot = (uint32 *)field;
  ptrdiff_t index = slot - &g_settings.input_bind[0][0];
  if (index < 0 || index >= kSettingsInputClasses * kSettingsInputActions)
    return false;
  if (!text || !text[0]) {
    *slot = InputMap_DefaultBinding(
        (InputAction)(index % kSettingsInputActions),
        (InputClass)(index / kSettingsInputActions));
    return true;
  }
  uint32 parsed = 0;
  if (!InputMap_ParseBinding(text, &parsed)) return false;
  *slot = parsed;
  return true;
}

int InputMap_FormatBindingField(char *buffer, int buffer_size,
                                const void *field) {
  return InputMap_FormatBinding(buffer, buffer_size, *(const uint32 *)field);
}

SettingChangeResult InputMap_ApplyBinding(const SettingDesc *desc,
                                          uint32 binding) {
  InputAction action;
  InputClass klass;
  if (!InputMap_DescribeRow(desc, &action, &klass))
    return kSettingChange_Rejected;

  /* One physical control drives one action. Steal it from whichever row in
   * the same class already had it rather than leaving a silent double-bind
   * the player has to hunt for. */
  if (binding) {
    for (int i = 0; i < kSettingsInputActions; i++) {
      if (i == (int)action) continue;
      if (g_settings.input_bind[klass][i] != binding) continue;
      for (int d = 0; d < g_setting_desc_count; d++) {
        const SettingDesc *other = &g_setting_descs[d];
        if (other->field == &g_settings.input_bind[klass][i])
          Settings_SetText(other, "Unbound");
      }
    }
  }

  char text[64];
  InputMap_FormatBinding(text, sizeof(text), binding);
  InputMap_Clear();
  return Settings_SetText(desc, text);
}

/* --- Runtime state -------------------------------------------------------- */

void InputMap_Clear(void) {
  s_key_bits = s_pad_bits = s_stick_bits = 0;
  memset(s_key_down, 0, sizeof(s_key_down));
  memset(s_pad_button_down, 0, sizeof(s_pad_button_down));
  memset(s_pad_axis_value, 0, sizeof(s_pad_axis_value));
}

uint32 InputMap_State(void) {
  uint32 state = 0;
  /* Safety valve: "Gamepad" with nothing plugged in would otherwise leave the
   * player with no working input and no way to reach the menu to undo it. */
  if (g_settings.input_device != kInputDevice_Gamepad || !s_pad_count)
    state |= s_key_bits;
  if (g_settings.input_device != kInputDevice_Keyboard)
    state |= s_pad_bits | s_stick_bits;
  return state & 0xFFF;
}

static void SetActionBit(uint32 *bits, InputAction action, bool pressed) {
  if (action >= kInputAction_PadCount) return;
  if (pressed) *bits |= 1u << action;
  else *bits &= ~(1u << action);
}

void InputMap_HandleKey(int scancode, bool pressed) {
  if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
    s_key_down[scancode] = pressed;
  uint32 want = INPUT_BIND_MAKE(kInputBind_Key, scancode, false);
  for (int a = 0; a < kInputAction_PadCount; a++)
    if (g_settings.input_bind[kInputClass_Keyboard][a] == want)
      SetActionBit(&s_key_bits, (InputAction)a, pressed);
}

/* Axis magnitude at which a stick/trigger counts as "pressed" for a binding.
 * Deliberately higher than the stick-as-D-pad threshold: a bound trigger
 * should not fire on a resting finger. */
enum { kAxisPressThreshold = 20000, kAxisReleaseThreshold = 12000 };

static bool AxisBindingHeld(uint32 binding, int value, bool was_held) {
  bool negative = INPUT_BIND_NEG(binding);
  int magnitude = negative ? -value : value;
  return magnitude > (was_held ? kAxisReleaseThreshold : kAxisPressThreshold);
}

static int StickDeadzone(void) {
  int percent = g_settings.input_stick_deadzone;
  if (percent < 5) percent = 5;
  if (percent > 90) percent = 90;
  return 32767 * percent / 100;
}

static void HandlePadButton(SDL_GamepadButton button, bool pressed) {
  if (button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT)
    s_pad_button_down[button] = pressed;
  uint32 want = INPUT_BIND_MAKE(kInputBind_PadButton, button, false);
  for (int a = 0; a < kInputAction_Count; a++) {
    if (g_settings.input_bind[kInputClass_Gamepad][a] != want) continue;
    if (a < kInputAction_PadCount) {
      SetActionBit(&s_pad_bits, (InputAction)a, pressed);
    } else if (INPUT_ACTION_IS_ANALOG(a)) {
      /* Polled, not dispatched — see InputMap_AnalogAction. */
    } else if (pressed && s_action_handler) {
      s_action_handler((InputAction)a);
    }
  }
}

/* Host actions bound to an axis are edge-triggered; this remembers the last
 * evaluated state per action so holding a trigger does not repeat. */
static uint32 s_host_axis_held;

static void HandlePadAxis(SDL_GamepadAxis axis, int value) {
  if (axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT)
    s_pad_axis_value[axis] = value;
  for (int a = 0; a < kInputAction_Count; a++) {
    uint32 binding = g_settings.input_bind[kInputClass_Gamepad][a];
    if (INPUT_BIND_KIND(binding) != kInputBind_PadAxis) continue;
    if (INPUT_BIND_CODE(binding) != (int)axis) continue;
    if (a < kInputAction_PadCount) {
      bool was_held = (s_pad_bits & (1u << a)) != 0;
      SetActionBit(&s_pad_bits, (InputAction)a,
                   AxisBindingHeld(binding, value, was_held));
    } else if (INPUT_ACTION_IS_ANALOG(a)) {
      /* Polled, not dispatched. */
    } else if (s_action_handler) {
      bool was_held = (s_host_axis_held & (1u << a)) != 0;
      bool held = AxisBindingHeld(binding, value, was_held);
      if (held) s_host_axis_held |= 1u << a;
      else s_host_axis_held &= ~(1u << a);
      if (held && !was_held) s_action_handler((InputAction)a);
    }
  }

  if (!g_settings.input_stick_as_dpad) return;
  int deadzone = StickDeadzone();
  if (axis == SDL_GAMEPAD_AXIS_LEFTX) {
    SetActionBit(&s_stick_bits, kInputAction_Left, value < -deadzone);
    SetActionBit(&s_stick_bits, kInputAction_Right, value > deadzone);
  } else if (axis == SDL_GAMEPAD_AXIS_LEFTY) {
    SetActionBit(&s_stick_bits, kInputAction_Up, value < -deadzone);
    SetActionBit(&s_stick_bits, kInputAction_Down, value > deadzone);
  }
}

/* 0..1 deflection of one binding right now. Sticks scale past the camera
 * deadzone so a light touch nudges rather than slews; triggers use their full
 * 0..32767 travel from a small dead spot. */
static float BindingMagnitude(uint32 binding) {
  int kind = INPUT_BIND_KIND(binding);
  int code = INPUT_BIND_CODE(binding);
  switch (kind) {
    case kInputBind_Key:
      return code >= 0 && code < SDL_SCANCODE_COUNT && s_key_down[code]
          ? 1.0f : 0.0f;
    case kInputBind_PadButton:
      return code >= 0 && code < SDL_GAMEPAD_BUTTON_COUNT &&
             s_pad_button_down[code] ? 1.0f : 0.0f;
    case kInputBind_PadAxis: {
      if (code < 0 || code >= SDL_GAMEPAD_AXIS_COUNT) return 0.0f;
      int value = s_pad_axis_value[code];
      /* A trigger rests at 0 and only travels positive; its binding's sign is
       * meaningless, so accept it whichever way the row was captured. */
      bool trigger = code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                     code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
      int magnitude = trigger ? value
                              : (INPUT_BIND_NEG(binding) ? -value : value);
      if (magnitude <= 0) return 0.0f;
      int percent = g_settings.input_cam_deadzone;
      if (percent < 0) percent = 0;
      if (percent > 90) percent = 90;
      int deadzone = 32767 * percent / 100;
      if (magnitude <= deadzone) return 0.0f;
      float scaled = (float)(magnitude - deadzone) / (float)(32767 - deadzone);
      return scaled > 1.0f ? 1.0f : scaled;
    }
    default:
      return 0.0f;
  }
}

float InputMap_AnalogAction(InputAction action) {
  if (action < 0 || action >= kInputAction_Count) return 0.0f;
  float best = 0.0f;
  if (g_settings.input_device != kInputDevice_Gamepad || !s_pad_count) {
    float m = BindingMagnitude(
        g_settings.input_bind[kInputClass_Keyboard][action]);
    if (m > best) best = m;
  }
  if (g_settings.input_device != kInputDevice_Keyboard) {
    float m = BindingMagnitude(
        g_settings.input_bind[kInputClass_Gamepad][action]);
    if (m > best) best = m;
  }
  return best;
}

void InputMap_HandleEvent(const SDL_Event *event) {
  if (!event) return;
  switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      AddGamepad(event->gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      RemoveGamepad(event->gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      if (!JoystickIsSelected(event->gbutton.which)) break;
      HandlePadButton((SDL_GamepadButton)event->gbutton.button,
                      event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      if (!JoystickIsSelected(event->gaxis.which)) break;
      HandlePadAxis((SDL_GamepadAxis)event->gaxis.axis, event->gaxis.value);
      break;
    default:
      break;
  }
}

/* Menu-nav edge tracking, kept apart from the gameplay bits so opening the
 * overlay (which clears those) cannot desynchronize it. */
static uint32 s_menu_axis_held;

bool InputMap_ActionForEvent(const SDL_Event *event, InputAction *action,
                             bool *pressed) {
  if (!event) return false;

  if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
      event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
    if (!JoystickIsSelected(event->gbutton.which)) return false;
    uint32 want = INPUT_BIND_MAKE(kInputBind_PadButton, event->gbutton.button,
                                  false);
    for (int a = 0; a < kInputAction_Count; a++) {
      if (g_settings.input_bind[kInputClass_Gamepad][a] != want) continue;
      if (action) *action = (InputAction)a;
      if (pressed) *pressed = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      return true;
    }
    return false;
  }

  if (event->type != SDL_EVENT_GAMEPAD_AXIS_MOTION) return false;
  if (!JoystickIsSelected(event->gaxis.which)) return false;
  int axis = event->gaxis.axis;
  int value = event->gaxis.value;

  for (int a = 0; a < kInputAction_Count; a++) {
    bool held;
    uint32 binding = g_settings.input_bind[kInputClass_Gamepad][a];
    bool was_held = (s_menu_axis_held & (1u << a)) != 0;
    if (INPUT_BIND_KIND(binding) == kInputBind_PadAxis &&
        INPUT_BIND_CODE(binding) == axis) {
      held = AxisBindingHeld(binding, value, was_held);
    } else if (g_settings.input_stick_as_dpad &&
               ((axis == SDL_GAMEPAD_AXIS_LEFTX &&
                 (a == kInputAction_Left || a == kInputAction_Right)) ||
                (axis == SDL_GAMEPAD_AXIS_LEFTY &&
                 (a == kInputAction_Up || a == kInputAction_Down)))) {
      int deadzone = StickDeadzone();
      bool negative = a == kInputAction_Left || a == kInputAction_Up;
      held = negative ? value < -deadzone : value > deadzone;
    } else {
      continue;
    }
    if (held == was_held) continue;
    if (held) s_menu_axis_held |= 1u << a;
    else s_menu_axis_held &= ~(1u << a);
    if (action) *action = (InputAction)a;
    if (pressed) *pressed = held;
    return true;
  }
  return false;
}

bool InputMap_DecodeEvent(const SDL_Event *event, InputClass klass,
                          uint32 *binding) {
  if (!event || !binding) return false;
  if (klass == kInputClass_Keyboard) {
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat) return false;
    *binding = INPUT_BIND_MAKE(kInputBind_Key, event->key.scancode, false);
    return true;
  }
  /* Capture deliberately accepts ANY connected pad, unlike the gameplay path
   * which honours input_gamepad_slot: whichever control the player physically
   * pressed is the one they meant to bind, and refusing it would just leave
   * the row stuck waiting with no explanation. */
  if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    *binding = INPUT_BIND_MAKE(kInputBind_PadButton, event->gbutton.button,
                               false);
    return true;
  }
  if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    int value = event->gaxis.value;
    if (value > kAxisPressThreshold) {
      *binding = INPUT_BIND_MAKE(kInputBind_PadAxis, event->gaxis.axis, false);
      return true;
    }
    if (value < -kAxisPressThreshold) {
      *binding = INPUT_BIND_MAKE(kInputBind_PadAxis, event->gaxis.axis, true);
      return true;
    }
  }
  return false;
}
