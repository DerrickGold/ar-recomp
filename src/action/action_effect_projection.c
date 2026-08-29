#include "action_effect_projection.h"

#include "diorama/diorama.h"

static const DioramaPlaneProjection *ProjectionPlaneForEffect(
    const DioramaProjection *projection,
    const ActionEffectInstance *effect) {
  if (!projection || !effect) return NULL;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
    return &projection->bg1_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg2)
    return &projection->bg2_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg1High)
    return &projection->bg1_high_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Obj &&
      effect->obj_priority < kDioramaObjectPriorityCount)
    return &projection->object_planes[effect->obj_priority];
  return NULL;
}

/* A projected plane is finite even though its perspective transform is
 * mathematically happy to extrapolate forever. Rejecting samples beyond the
 * plane's published source window prevents attached glows and particles from
 * floating in the surrounding Diorama void. Atmosphere is explicitly
 * unbounded, and BG2 may publish a folded continuation below its main plane. */
static bool PointIsOnPublishedDioramaPlane(
    const DioramaProjection *projection,
    const ActionEffectInstance *effect,
    float capture_x, float texture_y) {
  if (!projection || !effect) return false;
  if (effect->render_layer == kActionEffectRenderLayer_Atmosphere)
    return true;
  const DioramaPlaneProjection *plane =
      ProjectionPlaneForEffect(projection, effect);
  if (!plane || !plane->valid || projection->texture_width <= 0 ||
      projection->texture_height <= 0)
    return false;

  const float u =
      (capture_x + (float)projection->texture_x_origin) /
      (float)projection->texture_width;
  const float v = texture_y / (float)projection->texture_height;
  const float u_min = plane->u0 < plane->u1 ? plane->u0 : plane->u1;
  const float u_max = plane->u0 > plane->u1 ? plane->u0 : plane->u1;
  const float v_min = plane->v0 < plane->v1 ? plane->v0 : plane->v1;
  const float v_max = plane->v0 > plane->v1 ? plane->v0 : plane->v1;
  return u >= u_min && u <= u_max && v >= v_min &&
      (v <= v_max || plane->overflow_valid);
}

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

static void AddRequiredBgPlanes(
    uint32_t *mask, const ActionEffectInstance *effects, uint8_t count,
    uint8_t capacity, bool overflow) {
  if (!mask || !effects || overflow || count > capacity) return;
  for (uint8_t i = 0; i < count; i++) {
    const ActionEffectInstance *effect = &effects[i];
    if (!(effect->flags & kActionEffectFlag_Visible)) continue;
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
      *mask |= 1u << SR_PPU_OVERLAY_BG1;
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2)
      *mask |= 1u << SR_PPU_OVERLAY_BG2;
    else if (effect->projection_plane ==
             kActionEffectProjectionPlane_Bg1High)
      *mask |= 1u << kDioramaPlane_Bg1Hi;
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

uint32_t ActionEffectProjection_RequiredBgPlaneMask(
    const ActionEffectFrame *spell_frame,
    const ActionSceneEffectFrame *scene_frame) {
  uint32_t mask = 0;
  if (spell_frame)
    AddRequiredBgPlanes(
        &mask, spell_frame->effects, spell_frame->effect_count,
        kActionEffectMaxInstances, false);
  if (scene_frame) {
    AddRequiredBgPlanes(
        &mask, scene_frame->effects, scene_frame->effect_count,
        kActionSceneEffectMaxInstances, scene_frame->overflow != 0);
    AddRequiredBgPlanes(
        &mask, scene_frame->decorations, scene_frame->decoration_count,
        kActionSceneDecorationMaxInstances,
        scene_frame->decoration_overflow != 0);
  }
  return mask;
}

bool ActionEffectProjection_ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, ArRenderPointF *point) {
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
    if (!PointIsOnPublishedDioramaPlane(
            context->diorama_projection, effect, capture_x, texture_y))
      return false;
    ArRenderPointF projected;
    bool valid;
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
      valid = Diorama_ProjectCapturedBg1Point(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2)
      valid = Diorama_ProjectCapturedBg2Point(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    else if (effect->projection_plane ==
             kActionEffectProjectionPlane_Bg1High)
      valid = Diorama_ProjectCapturedBg1HighPoint(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    else
      valid = Diorama_ProjectCapturedPoint(
          context->diorama_projection, capture_x, texture_y,
          effect->obj_priority, &projected, NULL, NULL);
    if (valid) *point = projected;
    return valid;
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

bool ActionEffectProjection_IntersectsFlatViewport(
    const ActionEffectProjectionContext *context,
    const ActionEffectInstance *effect) {
  if (!context || !effect ||
      !(effect->flags & kActionEffectFlag_Visible) ||
      effect->geometry.kind != kActionEffectGeometry_Rect ||
      context->visible_width <= 0 || context->snes_height <= 0)
    return false;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  if (rect->x0 > rect->x1 || rect->y0 > rect->y1)
    return false;
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
  const float x0 = (float)context->ws_extra + screen_x + rect->x0;
  const float x1 = (float)context->ws_extra + screen_x + rect->x1;
  const float y0 = (float)screen_y + rect->y0;
  const float y1 = (float)screen_y + rect->y1;
  const float visible_x0 = (float)context->visible_x0;
  const float visible_x1 = visible_x0 + (float)context->visible_width;
  return x1 > visible_x0 && x0 < visible_x1 &&
      y1 > 0.0f && y0 < (float)context->snes_height;
}
