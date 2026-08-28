#ifndef ACTION_EFFECT_PROJECTION_H
#define ACTION_EFFECT_PROJECTION_H

#include <stdint.h>

#include "action_effects.h"
#include "render/render_types.h"

typedef struct DioramaProjection DioramaProjection;

/* Immutable presentation inputs needed to map an action-world effect point.
 * Keeping this smaller than FrameSlot makes the camera/widescreen/Diorama
 * seam pure and directly testable while present.c remains the owner of the
 * frame-slot-to-context copy. */
typedef struct ActionEffectProjectionContext {
  int16_t bg1_camera_x, bg1_camera_y;
  int16_t bg2_camera_x, bg2_camera_y;
  int ws_extra;
  int ws_extra_top;
  int visible_x0;
  int visible_width;
  int snes_height;
  ArRenderRectI viewport;
  const DioramaProjection *diorama_projection;
} ActionEffectProjectionContext;

/* Returns the authentic OBJ priority bands needed by current world-overlay
 * effects. Diorama uses this current-frame publication to retain an actor
 * projection when the isolated source band has no winning pixels; BG-local
 * effects never enter this mask. */
uint8_t ActionEffectProjection_RequiredObjPriorityMask(
    const ActionEffectFrame *spell_frame,
    const ActionSceneEffectFrame *scene_frame);

/* Returns the exact BG priority planes needed by current BG-attached effects. Unlike a
 * texture-content mask, this remains set when the isolated hardware plane is
 * empty at the current scroll position: the attached host effect is itself
 * current content at that authored transform. */
uint32_t ActionEffectProjection_RequiredBgPlaneMask(
    const ActionEffectFrame *spell_frame,
    const ActionSceneEffectFrame *scene_frame);

/* ActionEffectProjectPointFn-compatible projection callback. Flat mode maps
 * through the resolved viewport. Diorama mode uses the compositor-published
 * BG1/BG2/OBJ source plane, including display margins and the hidden apron
 * owned by DioramaProjection. */
bool ActionEffectProjection_ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, ArRenderPointF *point);

#endif  /* ACTION_EFFECT_PROJECTION_H */
