#ifndef AR_ACTION_EFFECT_PROJECTION_H
#define AR_ACTION_EFFECT_PROJECTION_H
/* ActionEffectProjection: maps action-world effect points to the screen
 * (camera, widescreen and diorama projection) from a small immutable context,
 * and reports which OBJ priority bands and BG planes current effects need.
 * Phase: pure.
 * Tests: tests/action_effect_render_test.c */

#include <stdint.h>

#include "action_effects.h"
#include "render/render_types.h"
#include "diorama/diorama.h"

/* Immutable presentation inputs needed to map an action-world effect point.
 * Keeping this smaller than FrameSlot makes the camera/widescreen/Diorama
 * seam pure and directly testable while present_action_effects.c owns the
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

/* Owned value snapshot for optional retained CPU work. Never key a cache by a
 * FrameSlot/projection pointer: those objects are reused as the camera moves.
 * Zero-initialize storage. Custom projection callbacks must bypass this cache. */
typedef struct ActionEffectProjectionSnapshot {
  ActionEffectProjectionContext context;
  DioramaProjection diorama;
  bool valid, has_diorama;
} ActionEffectProjectionSnapshot;
bool ActionEffectProjection_Matches(const ActionEffectProjectionSnapshot *,
                                   const ActionEffectProjectionContext *);
void ActionEffectProjection_Remember(ActionEffectProjectionSnapshot *,
                                    const ActionEffectProjectionContext *);


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
 * BG1/BG2/OBJ source plane, including display margins and the capture apron
 * owned by DioramaProjection. */
bool ActionEffectProjection_ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, ArRenderPointF *point);

/* Finite room, capture-plane and viewport intersection in effect-local space.
 * ActionEffectClipBoundsFn-compatible; does not alter geometry or its colors. */
bool ActionEffectProjection_ClipBounds(
    void *userdata, const ActionEffectInstance *effect, ActionEffectLocalRect *bounds);

/* Reports whether a visible rect-backed effect overlaps the source rectangle
 * shown by flat action presentation. This is useful for optional full-scene
 * passes whose cost and appearance should be tied to semantic source content,
 * rather than merely to the current room id. */
bool ActionEffectProjection_IntersectsFlatViewport(
    const ActionEffectProjectionContext *context,
    const ActionEffectInstance *effect);

#endif  /* AR_ACTION_EFFECT_PROJECTION_H */
