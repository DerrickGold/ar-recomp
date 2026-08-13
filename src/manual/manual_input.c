#include "manual_input.h"

#include "constants.h"

ManualIntent ManualInput_KeyIntent(SDL_Keycode key, bool zoomed) {
  switch (key) {
    /* These page at any zoom, so there is always a way to turn without zooming
     * back out first. */
    case SDLK_PAGEDOWN:  return kManualIntent_PageForward;
    case SDLK_PAGEUP:    return kManualIntent_PageBack;
    case SDLK_SPACE:     return kManualIntent_PageForward;
    case SDLK_BACKSPACE: return kManualIntent_PageBack;

    /* The arrows change meaning with the zoom -- see the header. */
    case SDLK_RIGHT:
      return zoomed ? kManualIntent_PanRight : kManualIntent_PageForward;
    case SDLK_LEFT:
      return zoomed ? kManualIntent_PanLeft : kManualIntent_PageBack;
    /* Up and down do NOTHING at fit, deliberately. There is no vertical
     * overhang to pan, and making them page would put two different gestures on
     * the same axis as the horizontal page turn. */
    case SDLK_UP:   return zoomed ? kManualIntent_PanUp : kManualIntent_None;
    case SDLK_DOWN: return zoomed ? kManualIntent_PanDown : kManualIntent_None;

    case SDLK_HOME: return kManualIntent_First;
    case SDLK_END:  return kManualIntent_Last;

    case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
      return kManualIntent_ZoomIn;
    case SDLK_MINUS: case SDLK_KP_MINUS:
      return kManualIntent_ZoomOut;
    case SDLK_0: case SDLK_KP_0:
      return kManualIntent_ZoomReset;

    default: return kManualIntent_None;
  }
}

ManualIntent ManualInput_PadIntent(SDL_GamepadButton button, bool zoomed) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kManualIntent_PageForward;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return kManualIntent_PageBack;

    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
      return zoomed ? kManualIntent_PanRight : kManualIntent_PageForward;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
      return zoomed ? kManualIntent_PanLeft : kManualIntent_PageBack;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
      return zoomed ? kManualIntent_PanUp : kManualIntent_None;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
      return zoomed ? kManualIntent_PanDown : kManualIntent_None;

    case SDL_GAMEPAD_BUTTON_SOUTH: return kManualIntent_ZoomIn;
    case SDL_GAMEPAD_BUTTON_WEST:  return kManualIntent_ZoomOut;
    case SDL_GAMEPAD_BUTTON_NORTH: return kManualIntent_ZoomReset;
    /* East is "back" everywhere else in this menu; the manual does not get to
     * mean something different by it. */
    case SDL_GAMEPAD_BUTTON_EAST:  return kManualIntent_Close;
    case SDL_GAMEPAD_BUTTON_BACK:  return kManualIntent_Close;

    default: return kManualIntent_None;
  }
}

float ManualInput_StickAxis(int raw, int deadzone_percent) {
  /* Same clamp input_map.c applies, so a deadzone the player set once means the
   * same thing in the manual as it does in the game. */
  if (deadzone_percent < 5) deadzone_percent = 5;
  if (deadzone_percent > 90) deadzone_percent = 90;
  const float deadzone =
      32767.0f * (float)deadzone_percent / (float)kPercentScale;

  float value = (float)raw;
  if (value > 32767.0f) value = 32767.0f;      /* -32768 is one past the top */
  if (value < -32767.0f) value = -32767.0f;
  const float magnitude = value < 0.0f ? -value : value;
  if (magnitude <= deadzone) return 0.0f;

  /* Rescale what is left of the travel to the full range, so the stick starts
   * from a standstill at the edge of the deadzone instead of snapping. */
  const float scaled = (magnitude - deadzone) / (32767.0f - deadzone);
  return value < 0.0f ? -scaled : scaled;
}

const char *ManualInput_HintText(ManualHintDevice device, bool zoomed) {
  /* Upper case throughout: the ROM's dialog font has no lower-case glyphs, and
   * the overlay substitutes '?' for anything it cannot draw. */
  if (device == kManualHintDevice_Gamepad) {
    /* Named for what ManualInput_PadIntent actually maps. B closes because East
     * is "back" everywhere else in this menu. */
    return zoomed ? "L/R PAGE   STICK OR D-PAD PANS   A/X ZOOM   B BACK"
                  : "L/R OR D-PAD PAGE   A/X ZOOM   B BACK";
  }
  return zoomed ? "ARROWS PAN   PGUP/PGDN PAGE   +/- ZOOM   DRAG PANS   ESC BACK"
                : "ARROWS PAGE   +/- ZOOM   CLICK A PAGE TO TURN   ESC BACK";
}

int ManualInput_NextDecode(const int *wanted, int wanted_count,
                           bool (*cached)(int page, void *user), void *user) {
  if (!wanted || !cached) return -1;
  for (int i = 0; i < wanted_count; i++) {
    const int page = wanted[i];
    if (page < 0) continue;          /* an empty side of the spread */
    if (cached(page, user)) continue;
    /* PRIORITY ORDER IS THE POINT: the caller lists what it wants most first,
     * so on the frame a turn starts, the leaf -- the thing actually moving, and
     * the one whose absence is most visible -- is decoded before the page being
     * revealed behind it. */
    return page;
  }
  return -1;
}
