#include "action_effect_projection.h"

#include <math.h>

#include "diorama/diorama.h"

static bool EffectUsesSkybox(const DioramaProjection *projection,
                             const ActionEffectInstance *effect) {
  return projection && projection->bg2_skybox.count &&
      (effect->projection_plane == kActionEffectProjectionPlane_Bg2 ||
       effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds);
}

static bool EffectUsesMoonAnchor(const ActionEffectInstance *effect) {
  /* The cloud shades this same moon and its rays. Native raster bands may
   * scroll the lake independently; they must not split the airborne veil. */
  return effect->kind == kActionEffect_BloodpoolMoonlight ||
      effect->kind == kActionEffect_BloodpoolCloud;
}

static bool SkyboxEffectBounds(const DioramaProjection *projection,
    const ActionEffectInstance *effect, float anchor_y,
    float *x0, float *y0, float *x1, float *y1) {
  if (EffectUsesMoonAnchor(effect))
    return Diorama_SkyboxAnchorBounds(projection, anchor_y, x0, y0, x1, y1);
  return Diorama_SkyboxCaptureBounds(projection, x0, y0, x1, y1);
}

static const DioramaPlaneProjection *ProjectionPlaneForEffect(
    const DioramaProjection *projection,
    const ActionEffectInstance *effect) {
  if (!projection || !effect) return NULL;
  /* The atmosphere has independent parallax and is inserted between layers,
   * but shares BG2's finite projected footprint. Moving its geometric plane
   * toward the viewer would expose light outside the backdrop's side edges. */
  if (effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds)
    return &projection->bg2_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
    return &projection->bg1_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg2)
    return &projection->bg2_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg1High)
    return &projection->bg1_high_plane;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg2High)
    return &projection->bg2_high_plane;
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
    float capture_x, float texture_y, float anchor_y) {
  if (!projection || !effect) return false;
  if (effect->render_layer == kActionEffectRenderLayer_Atmosphere)
    return true;
  if (EffectUsesSkybox(projection, effect)) {
    float x0, y0, x1, y1;
    return SkyboxEffectBounds(projection, effect, anchor_y, &x0, &y0, &x1, &y1) &&
        capture_x >= x0 && capture_x <= x1 && texture_y >= y0 && texture_y <= y1;
  }
  const DioramaPlaneProjection *plane =
      ProjectionPlaneForEffect(projection, effect);
  if (!plane || !plane->valid || projection->texture_width <= 0 ||
      projection->texture_height <= 0)
    return false;

  const float u =
      (capture_x + plane->capture_offset.x + (float)projection->texture_x_origin) /
      (float)projection->texture_width;
  const float v = (texture_y+plane->capture_offset.y) / (float)projection->texture_height;
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
    if (effect->kind == kActionEffect_BloodpoolMoonlight)
      *mask |= 1u << SR_PPU_OVERLAY_BG1;
    if (effect->render_layer == kActionEffectRenderLayer_Bg2HighAlpha)
      *mask |= 1u << kDioramaPlane_Bg2Hi;
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
      *mask |= 1u << SR_PPU_OVERLAY_BG1;
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2 ||
             effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds)
      *mask |= 1u << SR_PPU_OVERLAY_BG2;
    else if (effect->projection_plane ==
             kActionEffectProjectionPlane_Bg1High)
      *mask |= 1u << kDioramaPlane_Bg1Hi;
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2High)
      *mask |= 1u << kDioramaPlane_Bg2Hi;
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

static int16_t EffectCameraCoordinate(
    const ActionEffectInstance *effect, int16_t bg1, int16_t bg2) {
  if (effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds)
    return (int16_t)(((int)bg1 + bg2) / 2);
  return effect->projection_plane == kActionEffectProjectionPlane_Bg2 ||
      effect->projection_plane == kActionEffectProjectionPlane_Bg2High ? bg2 : bg1;
}

static bool ClipRectIsValid(const ActionEffectLocalRect *rect) {
  return isfinite(rect->x0) && isfinite(rect->y0) &&
      isfinite(rect->x1) && isfinite(rect->y1) &&
      rect->x0 < rect->x1 && rect->y0 < rect->y1;
}

static void IntersectClipRect(ActionEffectLocalRect *a, const ActionEffectLocalRect *b) {
  a->x0 = fmaxf(a->x0, b->x0);
  a->y0 = fmaxf(a->y0, b->y0);
  a->x1 = fminf(a->x1, b->x1);
  a->y1 = fminf(a->y1, b->y1);
}

bool ActionEffectProjection_ClipBounds(
    void *userdata, const ActionEffectInstance *effect, ActionEffectLocalRect *bounds) {
  const ActionEffectProjectionContext *context = userdata;
  if (!context || !effect || !bounds || effect->geometry.kind != kActionEffectGeometry_Rect)
    return false;
  *bounds = effect->geometry.data.rect;
  if (!ClipRectIsValid(bounds)) return false;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    if (!ClipRectIsValid(&effect->clip_rect)) return false;
    IntersectClipRect(bounds, &effect->clip_rect);
  }
  const int camera_x = EffectCameraCoordinate(
      effect, context->bg1_camera_x, context->bg2_camera_x);
  const int camera_y = EffectCameraCoordinate(
      effect, context->bg1_camera_y, context->bg2_camera_y);
  const int screen_x = (int16_t)(uint16_t)(effect->world_x - camera_x);
  const int screen_y = (int16_t)(uint16_t)(effect->world_y - camera_y);
  ActionEffectLocalRect visible;
  if (context->diorama_projection) {
    const DioramaProjection *projection = context->diorama_projection;
    if (EffectUsesSkybox(projection, effect)) {
      if (!SkyboxEffectBounds(projection, effect, context->ws_extra_top + screen_y,
              &visible.x0, &visible.y0, &visible.x1, &visible.y1)) return false;
      visible.x0 -= context->ws_extra + screen_x;
      visible.x1 -= context->ws_extra + screen_x;
      visible.y0 -= context->ws_extra_top + screen_y;
      visible.y1 -= context->ws_extra_top + screen_y;
    } else {
      const DioramaPlaneProjection *plane = ProjectionPlaneForEffect(projection, effect);
      if (!projection->valid || !plane || !plane->valid ||
          projection->texture_width <= 0 || projection->texture_height <= 0)
        return false;
      visible = (ActionEffectLocalRect){
        plane->u0 * projection->texture_width - projection->texture_x_origin -
            context->ws_extra - screen_x - plane->capture_offset.x,
        plane->v0 * projection->texture_height - context->ws_extra_top -
            screen_y - plane->capture_offset.y,
        plane->u1 * projection->texture_width - projection->texture_x_origin -
            context->ws_extra - screen_x - plane->capture_offset.x,
        plane->v1 * projection->texture_height - context->ws_extra_top -
            screen_y - plane->capture_offset.y,
      };
    }
  } else {
    if (context->visible_width <= 0 || context->snes_height <= 0 ||
        context->viewport.w <= 0 || context->viewport.h <= 0)
      return false;
    visible = (ActionEffectLocalRect){
      context->visible_x0 - context->ws_extra - screen_x, -screen_y,
      context->visible_x0 + context->visible_width - context->ws_extra - screen_x,
      context->snes_height - screen_y,
    };
  }
  if (!ClipRectIsValid(&visible)) return false;
  IntersectClipRect(bounds, &visible);
  return ClipRectIsValid(bounds);
}

bool ActionEffectProjection_ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, ArRenderPointF *point) {
  const ActionEffectProjectionContext *context = userdata;
  if (!context || !effect || !point) return false;

  if (effect->flags & kActionEffectFlag_ClipToRect) {
    const ActionEffectLocalRect *clip = &effect->clip_rect;
    if (!ClipRectIsValid(clip)) return false;
    if (!(effect->flags & kActionEffectFlag_ClippedMesh) &&
        (local_x < clip->x0 || local_x > clip->x1 ||
         local_y < clip->y0 || local_y > clip->y1)) {
      return false;
    }
  }

  const int16_t camera_x = EffectCameraCoordinate(
      effect, context->bg1_camera_x, context->bg2_camera_x);
  const int16_t camera_y = EffectCameraCoordinate(
      effect, context->bg1_camera_y, context->bg2_camera_y);
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
    /* Clipped triangles already intersect the source plane. Re-testing their
     * interpolated boundary points in normalized UVs introduces roundoff
     * holes; never move those points or change their interpolated brightness. */
    if (!(effect->flags & kActionEffectFlag_ClippedMesh) &&
        !PointIsOnPublishedDioramaPlane(
            context->diorama_projection, effect, capture_x, texture_y,
            screen_y + context->ws_extra_top))
      return false;
    ArRenderPointF projected;
    bool valid;
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
      valid = Diorama_ProjectCapturedBg1Point(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2 ||
             effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds) {
      if (EffectUsesSkybox(context->diorama_projection, effect) &&
          EffectUsesMoonAnchor(effect))
        valid = Diorama_ProjectSkyboxAnchorPoint(
            context->diorama_projection, screen_y + context->ws_extra_top,
            capture_x, texture_y, &projected);
      else valid = Diorama_ProjectCapturedBg2Point(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    }
    else if (effect->projection_plane ==
             kActionEffectProjectionPlane_Bg1High)
      valid = Diorama_ProjectCapturedBg1HighPoint(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2High)
      valid = Diorama_ProjectCapturedBg2HighPoint(
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
  const int16_t camera_x = EffectCameraCoordinate(
      effect, context->bg1_camera_x, context->bg2_camera_x);
  const int16_t camera_y = EffectCameraCoordinate(
      effect, context->bg1_camera_y, context->bg2_camera_y);
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
