#ifndef AR_TOUCH_CONTROLS_H
#define AR_TOUCH_CONTROLS_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "render/render_types.h"

/* On-screen SNES pad for touchscreens. The D-pad, face buttons, shoulders and
 * Select/Start report fixed buttons on an SDL virtual gamepad. Gameplay,
 * settings navigation and rebinding all use the ordinary gamepad bindings.
 * Menu remains a reserved host control so rebinding cannot strand the player.
 *
 * Shown on Android always, and on any other platform after the first direct
 * touch. Hidden while a gamepad is connected. */

/* Main-thread session lifetime, around InputMap's lifetime. */
void TouchControls_Init(void);
void TouchControls_Shutdown(void);

/* Consumes finger events (returns true for them), publishing gamepad events
 * back onto the SDL queue. Also observes hotplug and focus loss. */
bool TouchControls_HandleEvent(const SDL_Event *event);
/* Reserved Menu button events bypass device gating and binding capture. */
bool TouchControls_IsMenuEvent(const SDL_Event *event);

/* Draws in physical output pixels; call from the host UI pass. */
void TouchControls_Draw(ArRenderExtentI output_size);

#endif /* AR_TOUCH_CONTROLS_H */
