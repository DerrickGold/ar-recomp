#include "action_effect_projection.h"
#include "action_light_kinds.h"
#include "action_storm.h"

#include <math.h>
#include <string.h>

#include "diorama/diorama.h"

static bool EffectUsesSkybox(const DioramaProjection *projection,
                             const ActionEffectInstance *effect) {
  return projection && projection->bg2_skybox.count &&
      (effect->projection_plane == kActionEffectProjectionPlane_Bg2 ||
       effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds);
}

static bool EffectUsesStaticAnchor(const ActionEffectInstance *effect) {
  return (effect->flags & kActionEffectFlag_StaticAnchor) != 0;
}

static bool SkyboxEffectBounds(const DioramaProjection *projection,
    const ActionEffectInstance *effect, float anchor_y,
    float *x0, float *y0, float *x1, float *y1) {
  if (effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds &&
      projection->bg2_skybox.world_plane.valid) {
    const DioramaPlaneProjection plane = Diorama_BetweenBackgroundsPlane(projection);
    return Diorama_PlaneCaptureBounds(projection, &plane, x0, y0, x1, y1);
  }
  const ArRenderPointF offset =
      effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds
      ? Diorama_BetweenBackgroundsSkyboxOffset(projection) : (ArRenderPointF){0};
  const bool valid = EffectUsesStaticAnchor(effect)
      ? Diorama_SkyboxAnchorBounds(projection, anchor_y + offset.y, x0, y0, x1, y1)
      : Diorama_SkyboxCaptureBounds(projection, x0, y0, x1, y1);
  if (valid) { *x0 -= offset.x; *x1 -= offset.x; *y0 -= offset.y; *y1 -= offset.y; }
  return valid;
}

static const DioramaPlaneProjection *ProjectionPlaneForEffect(
    const DioramaProjection *projection,
    const ActionEffectInstance *effect, DioramaPlaneProjection *atmosphere) {
  if (!projection || !effect) return NULL;
  /* The atmosphere has independent parallax and is inserted between layers,
   * but shares BG2's finite projected footprint. Moving its geometric plane
   * toward the viewer would expose light outside the backdrop's side edges. */
  if (effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds) {
    *atmosphere = Diorama_BetweenBackgroundsPlane(projection);
    return atmosphere;
  }
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
  DioramaPlaneProjection atmosphere;
  const DioramaPlaneProjection *plane =
      ProjectionPlaneForEffect(projection, effect, &atmosphere);
  float x0, y0, x1, y1;
  return Diorama_PlaneCaptureBounds(projection, plane, &x0, &y0, &x1, &y1) &&
      capture_x >= x0 && capture_x <= x1 && texture_y >= y0 &&
      (texture_y <= y1 || plane->overflow_valid);
}

static void AddRequiredObjPriorities(
    uint8_t *mask, const ActionEffectInstance *effects, uint8_t count,
    uint8_t capacity, bool overflow) {
  if (!mask || !effects || overflow || count > capacity) return;
  for (uint8_t i = 0; i < count; i++) {
    const ActionEffectInstance *effect = &effects[i];
    if (!(effect->flags & kActionEffectFlag_Visible) ||
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
    if(effect->tuning.light_receivers_set && (effect->tuning.light_receivers&kActionReceiver_Scenery) &&
        ActionLightKind_Supported(effect->kind) && effect->render_layer==kActionEffectRenderLayer_WorldOverlay)
      *mask |= 1u << SR_PPU_OVERLAY_BG1;
    if (effect->kind == kActionEffect_BloodpoolMoonlight)
      *mask |= 1u << SR_PPU_OVERLAY_BG1;
    /* Source projection and insertion point are independent for authored
     * effects. Request the attachment callback as well as its camera plane. */
    if (effect->render_layer == kActionEffectRenderLayer_Bg2Alpha ||
        effect->render_layer == kActionEffectRenderLayer_Bg2Plane)
      *mask |= 1u << SR_PPU_OVERLAY_BG2;
    if (effect->render_layer == kActionEffectRenderLayer_Bg1Mist ||
        effect->render_layer == kActionEffectRenderLayer_Bg1Plane ||
        effect->render_layer == kActionEffectRenderLayer_Bg1Light)
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
    /* Attached authored lights and alpha particles also need an object plane
     * even when no native sprite or recognized accent occupies that depth. */
    AddRequiredObjPriorities(
        &mask, scene_frame->authored, scene_frame->authored_count,
        kActionAuthoredMaxInstances, false);
    AddRequiredObjPriorities(
        &mask, scene_frame->effects, scene_frame->effect_count,
        kActionSceneEffectMaxInstances, scene_frame->overflow != 0);
    /* Detached smoke keeps its source depth after the last actor retires. */
    const ActionFireballSmoke *smoke = &scene_frame->fireball_smoke;
    if (smoke->count <= kActionFireballSmokeMaxPuffs) {
      for (unsigned i = 0; i < smoke->count; ++i) {
        const ActionFireballSmokePuff *puff = &smoke->puffs[i];
        if (puff->age < kActionFireballSmokeLifetime &&
            puff->priority < kActionEffectObjPriorityCount)
          mask |= (uint8_t)(1u << puff->priority);
      }
    }
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
    AddRequiredBgPlanes(&mask, scene_frame->authored, scene_frame->authored_count,
        kActionAuthoredMaxInstances, false);
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

ArRenderPointF ActionEffectProjection_CaptureOrigin(const ActionEffectProjectionContext *context,
    const ActionEffectInstance *effect) {
  const int cx = EffectCameraCoordinate(effect, context->bg1_camera_x, context->bg2_camera_x);
  const int cy = EffectCameraCoordinate(effect, context->bg1_camera_y, context->bg2_camera_y);
  ArRenderPointF origin = {
      context->ws_extra + (int16_t)(uint16_t)(effect->world_x - cx),
      context->ws_extra_top + (int16_t)(uint16_t)(effect->world_y - cy)};
  if (effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds) {
    origin.x -= 0.5f * ((int)context->bg1_camera_x + context->bg2_camera_x) - cx;
    origin.y -= 0.5f * ((int)context->bg1_camera_y + context->bg2_camera_y) - cy;
  }
  if (ActionStorm_CustomParallax(effect)) {
    const float rate = effect->field_style.storm.parallax_x;
    origin.x += cx - rate * context->bg1_camera_x;
    if (context->diorama_projection) {
      const DioramaProjection *p = context->diorama_projection;
      const float bg2_motion = p->bg2_plane.valid ? p->bg2_plane.capture_offset.x :
          p->bg2_skybox.world_plane.valid ? p->bg2_skybox.world_plane.capture_offset.x :
          p->bg2_skybox.capture_offset.x;
      /* Projectors already add BG2 capture motion; replace only that X term
       * with the bolt's depth, including fractional generated camera frames. */
      origin.x += rate * p->bg1_plane.capture_offset.x - bg2_motion;
    }
  }
  return origin;
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

bool ActionEffectProjection_ViewBounds(const ActionEffectProjectionContext *context,
    const ActionEffectInstance *effect, ActionEffectLocalRect *bounds) {
  if (!context || !effect || !bounds) return false;
  const ArRenderPointF origin = ActionEffectProjection_CaptureOrigin(context, effect);
  const float screen_x = origin.x - context->ws_extra;
  const float screen_y = origin.y - context->ws_extra_top;
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
      DioramaPlaneProjection atmosphere;
      const DioramaPlaneProjection *plane = ProjectionPlaneForEffect(projection, effect, &atmosphere);
      if (!Diorama_PlaneCaptureBounds(projection, plane,
              &visible.x0, &visible.y0, &visible.x1, &visible.y1))
        return false;
      visible.x0 -= context->ws_extra + screen_x;
      visible.x1 -= context->ws_extra + screen_x;
      visible.y0 -= context->ws_extra_top + screen_y;
      visible.y1 -= context->ws_extra_top + screen_y;
    }
  } else {
    if (context->visible_width <= 0 || context->snes_height <= 0 ||
        context->viewport.w <= 0 || context->viewport.h <= 0)
      return false;
    visible = (ActionEffectLocalRect){
      context->visible_x0 - context->ws_extra - screen_x,
      -context->visible_top - screen_y,
      context->visible_x0 + context->visible_width - context->ws_extra - screen_x,
      context->snes_height - context->visible_top - screen_y,
    };
    if (context->capture_height > 0) {
      const ActionEffectLocalRect capture = {
        visible.x0, -context->ws_extra_top - screen_y,
        visible.x1, context->capture_height - context->ws_extra_top - screen_y,
      };
      IntersectClipRect(&visible, &capture);
    }
  }
  if (!ClipRectIsValid(&visible)) return false;
  *bounds = visible;
  return true;
}

bool ActionEffectProjection_ClipBounds(
    void *userdata, const ActionEffectInstance *effect, ActionEffectLocalRect *bounds) {
  if (!effect || !bounds || effect->geometry.kind != kActionEffectGeometry_Rect)
    return false;
  *bounds = effect->geometry.data.rect;
  if (!ClipRectIsValid(bounds)) return false;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    if (!ClipRectIsValid(&effect->clip_rect)) return false;
    IntersectClipRect(bounds, &effect->clip_rect);
  }
  ActionEffectLocalRect visible;
  if (!ActionEffectProjection_ViewBounds(userdata, effect, &visible)) return false;
  IntersectClipRect(bounds, &visible);
  return ClipRectIsValid(bounds);
}

bool ActionEffectProjection_BillboardBounds(const ActionEffectProjectionContext *context,
    const ActionEffectInstance *effect, ActionEffectLocalRect *bounds) {
  if (!context || !effect || !bounds) return false;
  *bounds = (ActionEffectLocalRect){-1e9f,-1e9f,1e9f,1e9f};
  if (effect->render_layer != kActionEffectRenderLayer_Atmosphere &&
      !(effect->flags & kActionEffectFlag_ClippedMesh)) {
    if (!ActionEffectProjection_ViewBounds(context,effect,bounds)) return false;
    if (context->diorama_projection && !EffectUsesSkybox(context->diorama_projection,effect)) {
      DioramaPlaneProjection atmosphere;
      const DioramaPlaneProjection *plane = ProjectionPlaneForEffect(context->diorama_projection,effect,&atmosphere);
      if (plane && plane->overflow_valid) bounds->y1 = 1e9f;
    }
  }
  if ((effect->flags & kActionEffectFlag_ClipToRect) &&
      !(effect->flags & kActionEffectFlag_ClippedMesh)) IntersectClipRect(bounds,&effect->clip_rect);
  return ClipRectIsValid(bounds);
}

static bool ProjectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, ArRenderPointF *point, bool billboard) {
  const ActionEffectProjectionContext *context = userdata;
  if (!context || !effect || !point) return false;

  if (!billboard && (effect->flags & kActionEffectFlag_ClipToRect)) {
    const ActionEffectLocalRect *clip = &effect->clip_rect;
    if (!ClipRectIsValid(clip)) return false;
    if (!(effect->flags & kActionEffectFlag_ClippedMesh) &&
        (local_x < clip->x0 || local_x > clip->x1 ||
         local_y < clip->y0 || local_y > clip->y1)) {
      return false;
    }
  }

  const ArRenderPointF origin = ActionEffectProjection_CaptureOrigin(context, effect);
  const float capture_x = origin.x + local_x;
  const float capture_y = origin.y - context->ws_extra_top + local_y;

  if (context->diorama_projection) {
    /* Texture row zero represents screen y=-ws_extra_top. Flat mode keeps
     * authentic screen Y and therefore intentionally ignores this margin. */
    const float texture_y = capture_y + (float)context->ws_extra_top;
    /* Clipped triangles already intersect the source plane. Re-testing their
     * interpolated boundary points can introduce roundoff holes; never move
     * those points or change their interpolated brightness. */
    if (!billboard && !(effect->flags & kActionEffectFlag_ClippedMesh) &&
        !PointIsOnPublishedDioramaPlane(
            context->diorama_projection, effect, capture_x, texture_y,
            origin.y))
      return false;
    ArRenderPointF projected;
    bool valid;
    if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
      valid = Diorama_ProjectCapturedBg1Point(
          context->diorama_projection, capture_x, texture_y,
          &projected, NULL, NULL);
    else if (effect->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds)
      valid = Diorama_ProjectCapturedBetweenBackgroundsPoint(
          context->diorama_projection, capture_x, texture_y, origin.y,
          EffectUsesStaticAnchor(effect), &projected);
    else if (effect->projection_plane == kActionEffectProjectionPlane_Bg2) {
      if (EffectUsesSkybox(context->diorama_projection, effect) &&
          EffectUsesStaticAnchor(effect))
        valid = Diorama_ProjectSkyboxAnchorPoint(
            context->diorama_projection, origin.y,
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
    if (valid && effect->projection_plane==kActionEffectProjectionPlane_Bg2 &&
        context->diorama_projection->bg2_skybox.to_texture) {
      const DioramaProjection *p=context->diorama_projection;
      const DioramaSkyboxProjection *sky=&p->bg2_skybox;
      if (sky->active_band<0 || sky->active_band>=(int)sky->count) return false;
      const DioramaSkyboxBandProjection *band=&sky->bands[sky->active_band];
      const float u=(projected.x-p->output_x)/p->output_width;
      const float v=((projected.y-p->output_y)/p->output_height-band->output_y0)/
          (band->output_y1-band->output_y0);
      projected=(ArRenderPointF){band->x0+u*(band->x1-band->x0)+sky->texture_offset.x,
          band->y0+v*(band->y1-band->y0)+sky->texture_offset.y};
    }
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
      (capture_y + context->visible_top) * context->viewport.h /
          (float)context->snes_height;
  return true;
}

bool ActionEffectProjection_ProjectPoint(void *context, const ActionEffectInstance *effect,
    float x, float y, ArRenderPointF *point) {
  return ProjectPoint(context,effect,x,y,point,false);
}

bool ActionEffectProjection_ProjectBillboardPoint(void *context, const ActionEffectInstance *effect,
    float x, float y, ArRenderPointF *point) {
  return ProjectPoint(context,effect,x,y,point,true);
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
  const ArRenderPointF origin = ActionEffectProjection_CaptureOrigin(context, effect);
  const float x0 = origin.x + rect->x0;
  const float x1 = origin.x + rect->x1;
  const float y0 = origin.y - context->ws_extra_top + rect->y0;
  const float y1 = origin.y - context->ws_extra_top + rect->y1;
  const float visible_x0 = (float)context->visible_x0;
  const float visible_x1 = visible_x0 + (float)context->visible_width;
  return x1 > visible_x0 && x0 < visible_x1 &&
      y1 > -(float)context->visible_top &&
      y0 < (float)(context->snes_height - context->visible_top);
}

/* Comparing the full value is conservative: padding or unused fields may
 * cause a miss, but cannot reuse a stale transform. Stored pointers are null. */
bool ActionEffectProjection_Matches(const ActionEffectProjectionSnapshot *s,
                                    const ActionEffectProjectionContext *c) {
  if (!s || !s->valid || !c || s->has_diorama != (c->diorama_projection != NULL)) return false;
  ActionEffectProjectionContext value = *c;
  value.diorama_projection = NULL;
  return !memcmp(&s->context, &value, sizeof(value)) &&
      (!s->has_diorama || !memcmp(&s->diorama, c->diorama_projection, sizeof(s->diorama)));
}
void ActionEffectProjection_Remember(ActionEffectProjectionSnapshot *s,
                                     const ActionEffectProjectionContext *c) {
  s->valid = c != NULL;
  if (!c) return;
  s->context = *c;
  s->context.diorama_projection = NULL;
  s->has_diorama = c->diorama_projection != NULL;
  if (s->has_diorama) s->diorama = *c->diorama_projection;
}
