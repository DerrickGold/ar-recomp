#ifndef AR_TOUCH_CONTROLS_H
#define AR_TOUCH_CONTROLS_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "render/render_types.h"

/* On-screen SNES pad for touchscreens. Fingers on the drawn D-pad, face
 * buttons, shoulders, Select/Start and Menu become the player's own keyboard
 * bindings, delivered as ordinary SDL key events. Gameplay, the settings menu
 * and rebinding therefore see exactly what a keyboard player produces, and no
 * consumer needs to know touch exists.
 *
 * Shown on Android always, and on any other platform after the first direct
 * touch. Hidden while a gamepad is connected. */

/* Consumes finger events (returns true for them). Safe on any thread that
 * pumps SDL events; pushes the translated key events back onto the queue. */
bool TouchControls_HandleEvent(const SDL_Event *event);

/* Draws in physical output pixels; call from the host UI pass. */
void TouchControls_Draw(ArRenderExtentI output_size);

#endif /* AR_TOUCH_CONTROLS_H */
