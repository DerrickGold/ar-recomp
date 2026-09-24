#ifndef SETTINGS_OVERLAY_MENU_INPUT_H
#define SETTINGS_OVERLAY_MENU_INPUT_H

#include "input_map.h"

/* Private menu commands. Dispatch and displayed bindings share this mapping;
 * menu events never need to update gameplay's held-button state. */
typedef enum {
  kMenuNav_Up,
  kMenuNav_Down,
  kMenuNav_Left,
  kMenuNav_Right,
  kMenuNav_Confirm,
  kMenuNav_Back,     /* Return to navigation, or close from that column. */
  kMenuNav_Reset,    /* Restore the selected row's default. */
  kMenuNav_TabPrev,
  kMenuNav_TabNext,
  kMenuNav_Close,    /* Close the overlay; dismiss a confirmation/Details. */
  kMenuNav_Details,  /* Read-only expanded explanation of a regional option. */
} MenuNav;

bool OverlayMenuInput_KeyNav(SDL_Keycode key, MenuNav *out);
bool OverlayMenuInput_ActionNav(InputAction action, MenuNav *out);
/* Empty for an unavailable command. Keyboard hints honor universal shortcuts
 * taking precedence over user bindings; gamepad labels use the selected pad. */
int OverlayMenuInput_Hint(char *out, int capacity, MenuNav nav, InputClass device);
int OverlayMenuInput_PairHint(char *out, int capacity, MenuNav first,
                              MenuNav second, InputClass device);

#endif
