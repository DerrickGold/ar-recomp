#ifndef ACTION_EFFECT_PROJECTION_H
#define ACTION_EFFECT_PROJECTION_H

#include <stdint.h>
#include <SDL3/SDL.h>

#include "action_effects.h"
#include "diorama/diorama.h"

/* Immutable presentation inputs needed to map an action-world effect point.
 * Keeping this smaller than FrameSlot makes the camera/widescreen/Diorama
 * seam pure and directly testable while present.c remains the owner of the
 * frame-slot-to-context copy. */
typedef struct ActionEffectProjectionContext {
  int16_t bg1_camera_x, bg1_camera_y;
  int ws_extra;
  int ws_extra_top;
  int visible_x0;
  int visible_width;
  int snes_height;
  SDL_Rect viewport;
  const DioramaProjection *diorama_projection;
} ActionEffectProjectionContext;

/* ActionEffectProjectPointFn-compatible projection callback. Flat mode maps
 * through the resolved viewport. Diorama mode uses the compositor-published
 * BG1/OBJ source plane, including display margins and the hidden apron owned
 * by DioramaProjection. */
bool ActionEffectProjection_ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, SDL_FPoint *point);

#endif  /* ACTION_EFFECT_PROJECTION_H */
