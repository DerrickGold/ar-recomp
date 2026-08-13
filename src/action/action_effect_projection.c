#include "action_effect_projection.h"

static void AddRequiredObjPriorities(
    uint8_t *mask, const ActionEffectInstance *effects, uint8_t count,
    uint8_t capacity, bool overflow) {
  if (!mask || !effects || overflow || count > capacity) return;
  for (uint8_t i = 0; i < count; i++) {
    const ActionEffectInstance *effect = &effects[i];
    if (!(effect->flags & kActionEffectFlag_Visible) ||
        effect->render_layer != kActionEffectRenderLayer_WorldOverlay ||
        effect->projection_plane != kActionEffectProjectionPlane_Obj ||
        effect->obj_priority >= kActionEffectObjPriorityCount)
      continue;
    *mask |= (uint8_t)(1u << effect->obj_priority);
  }
}

uint8_t ActionEffectProjection_RequiredObjPriorityMask(
    const ActionEffectFrame *spell_frame,
    const ActionSceneEffectFrame *scene_frame) {
  uint8_t mask = 0;
  if (spell_frame)
    AddRequiredObjPriorities(
        &mask, spell_frame->effects, spell_frame->effect_count,
        kActionEffectMaxInstances, false);
  if (scene_frame) {
    AddRequiredObjPriorities(
        &mask, scene_frame->effects, scene_frame->effect_count,
        kActionSceneEffectMaxInstances, scene_frame->overflow != 0);
  }
  return mask;
}

bool ActionEffectProjection_ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, SDL_FPoint *point) {
  const ActionEffectProjectionContext *context = userdata;
  if (!context || !effect || !point) return false;

  const int16_t camera_x = effect->projection_plane ==
          kActionEffectProjectionPlane_Bg2
      ? context->bg2_camera_x : context->bg1_camera_x;
  const int16_t camera_y = effect->projection_plane ==
          kActionEffectProjectionPlane_Bg2
      ? context->bg2_camera_y : context->bg1_camera_y;
  const int screen_x = (int16_t)(uint16_t)(
      (uint16_t)effect->world_x - (uint16_t)camera_x);
  const int screen_y = (int16_t)(uint16_t)(
      (uint16_t)effect->world_y - (uint16_t)camera_y);
  const float capture_x = (float)context->ws_extra + screen_x + local_x;
  const float capture_y = (float)screen_y + local_y;

  if (context->diorama_projection) {
    /* Texture row zero represents screen y=-ws_extra_top. Flat mode keeps
     * authentic screen Y and therefore intentionally ignores this margin. */
    const float texture_y = capture_y + (float)context->ws_extra_top;
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
      return Diorama_ProjectCapturedBg1Point(
          context->diorama_projection, capture_x, texture_y,
          point, NULL, NULL);
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg2)
      return Diorama_ProjectCapturedBg2Point(
          context->diorama_projection, capture_x, texture_y,
          point, NULL, NULL);
    return Diorama_ProjectCapturedPoint(
        context->diorama_projection, capture_x, texture_y,
        effect->obj_priority, point, NULL, NULL);
  }

  if (context->visible_width <= 0 || context->snes_height <= 0 ||
      context->viewport.w <= 0 || context->viewport.h <= 0)
    return false;
  point->x = context->viewport.x +
      (capture_x - (float)context->visible_x0) * context->viewport.w /
          (float)context->visible_width;
  point->y = context->viewport.y +
      capture_y * context->viewport.h / (float)context->snes_height;
  return true;
}
