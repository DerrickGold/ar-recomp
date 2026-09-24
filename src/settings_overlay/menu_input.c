#include "settings_overlay/menu_input.h"

#include <stdio.h>
#include <string.h>
#include "settings.h"

static const struct {
  InputAction action;
  MenuNav nav;
} kCommands[] = {
  {kInputAction_Up, kMenuNav_Up},
  {kInputAction_Down, kMenuNav_Down},
  {kInputAction_Left, kMenuNav_Left},
  {kInputAction_Right, kMenuNav_Right},
  {kInputAction_B, kMenuNav_Confirm},
  {kInputAction_A, kMenuNav_Back},
  {kInputAction_Y, kMenuNav_Reset},
  {kInputAction_X, kMenuNav_Details},
  {kInputAction_L, kMenuNav_TabPrev},
  {kInputAction_R, kMenuNav_TabNext},
  {kInputAction_Menu, kMenuNav_Close},
  {kInputAction_Start, kMenuNav_Close},
};

bool OverlayMenuInput_ActionNav(InputAction action, MenuNav *out) {
  if (!out) return false;
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i)
    if (kCommands[i].action == action) {
      *out = kCommands[i].nav;
      return true;
    }
  return false;
}

bool OverlayMenuInput_KeyNav(SDL_Keycode key, MenuNav *out) {
  if (!out) return false;
  const SDL_Scancode sc = SDL_GetScancodeFromKey(key, NULL);
  /* Fixed recovery controls remain usable even with broken/unbound game
   * controls. F2 belongs to the host, not to menu navigation. */
  switch (sc) {
    case SDL_SCANCODE_F2: return false;
    case SDL_SCANCODE_F3: *out = kMenuNav_Details; return true;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_F1: *out = kMenuNav_Close; return true;
    case SDL_SCANCODE_UP: *out = kMenuNav_Up; return true;
    case SDL_SCANCODE_DOWN: *out = kMenuNav_Down; return true;
    case SDL_SCANCODE_LEFT: *out = kMenuNav_Left; return true;
    case SDL_SCANCODE_RIGHT: *out = kMenuNav_Right; return true;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: *out = kMenuNav_Confirm; return true;
    case SDL_SCANCODE_LEFTBRACKET: *out = kMenuNav_TabPrev; return true;
    case SDL_SCANCODE_RIGHTBRACKET:
    case SDL_SCANCODE_TAB: *out = kMenuNav_TabNext; return true;
    default: break;
  }
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i) {
    /* Start/Select/Menu intentionally do not close on the keyboard: Start's
     * default Return binding confirms instead, and Esc/F1 always close. */
    if (kCommands[i].nav == kMenuNav_Close) continue;
    const uint32 binding = g_settings.input_bind[kInputClass_Keyboard][kCommands[i].action];
    if (INPUT_BIND_KIND(binding) == kInputBind_Key && INPUT_BIND_CODE(binding) == (int)sc) {
      *out = kCommands[i].nav;
      return true;
    }
  }
  return false;
}

int OverlayMenuInput_Hint(char *out, int capacity, MenuNav nav, InputClass device) {
  if (!out || capacity <= 0) return 0;
  *out = 0;
  if (device != kInputClass_Keyboard && device != kInputClass_Gamepad) return 0;
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i) {
    if (kCommands[i].nav != nav) continue;
    const uint32 binding = g_settings.input_bind[device][kCommands[i].action];
    if (!binding) continue;
    if (device == kInputClass_Keyboard) {
      MenuNav actual;
      if (INPUT_BIND_KIND(binding) != kInputBind_Key ||
          !OverlayMenuInput_KeyNav(SDL_GetKeyFromScancode(INPUT_BIND_CODE(binding), 0, false), &actual) ||
          actual != nav) continue;
    }
    return InputMap_ActionHintForDevice(out, capacity, kCommands[i].action, device);
  }
  if (device == kInputClass_Gamepad) {
    /* The left stick remains a direction source when its D-pad emulation is
     * enabled, even if a digital direction has been unbound. */
    if (g_settings.input_stick_as_dpad && nav >= kMenuNav_Up && nav <= kMenuNav_Right)
      return InputMap_FormatBindingHint(out, capacity,
          INPUT_BIND_MAKE(kInputBind_PadAxis,
              nav <= kMenuNav_Down ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_LEFTX,
              nav == kMenuNav_Up || nav == kMenuNav_Left), SDL_GAMEPAD_TYPE_STANDARD);
    return 0;
  }
  SDL_Scancode fallback;
  switch (nav) {
    case kMenuNav_Up: fallback = SDL_SCANCODE_UP; break;
    case kMenuNav_Down: fallback = SDL_SCANCODE_DOWN; break;
    case kMenuNav_Left: fallback = SDL_SCANCODE_LEFT; break;
    case kMenuNav_Right: fallback = SDL_SCANCODE_RIGHT; break;
    case kMenuNav_Confirm: fallback = SDL_SCANCODE_RETURN; break;
    case kMenuNav_TabPrev: fallback = SDL_SCANCODE_LEFTBRACKET; break;
    case kMenuNav_TabNext: fallback = SDL_SCANCODE_RIGHTBRACKET; break;
    case kMenuNav_Close: fallback = SDL_SCANCODE_ESCAPE; break;
    case kMenuNav_Details: fallback = SDL_SCANCODE_F3; break;
    default: return 0;
  }
  return InputMap_FormatBindingHint(out, capacity,
      INPUT_BIND_MAKE(kInputBind_Key, fallback, false), SDL_GAMEPAD_TYPE_STANDARD);
}

int OverlayMenuInput_PairHint(char *out, int capacity, MenuNav first,
                              MenuNav second, InputClass device) {
  if (!out || capacity <= 0) return 0;
  char a[64], b[64];
  OverlayMenuInput_Hint(a, sizeof(a), first, device);
  OverlayMenuInput_Hint(b, sizeof(b), second, device);
  /* Avoid repeating device prefixes, but retain arbitrary remapped names. */
  const char *suffix = b;
  const char *prefixes[] = {"D-Pad ", "LS ", "RS "};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
    size_t n = strlen(prefixes[i]);
    if (!strncmp(a, prefixes[i], n) && !strncmp(b, prefixes[i], n)) suffix += n;
  }
  return snprintf(out, capacity, "%s%s%s", a, *a && *b ? "/" : "", suffix);
}
