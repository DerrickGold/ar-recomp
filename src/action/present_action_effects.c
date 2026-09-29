/* Action-stage enhancement passes. Geometry recipes live in
 * action_effect_render*.c; this owner uploads masks, submits flat/Diorama
 * effects, brackets heat refraction and releases all its cached resources.
 * Only captured FrameSlot values may affect the scene. */
#include "action/present_action_effects.h"

#include <stdio.h>
#include <string.h>

#include "action/action_effect_projection.h"
#include "action/action_effect_render.h"
#include "actraiser/actraiser_room_profiles.h"
#include "app/session_fatal.h"
#include "diorama/diorama.h"
#include "present/present.h"
#include "present/presentation_upload_mirror.h"
#include "render/effect_batch.h"
#include "render/render_output.h"

static ArRenderTexture s_action_bg1_mask_texture;
static ArRenderTexture s_action_bg2_mask_texture;
static ArRenderTexture s_action_plane_effect_target;
static int s_action_plane_effect_w, s_action_plane_effect_h;
static bool s_action_plane_blend_supported = true;
static bool s_action_surface_light_supported = true;
static ArRenderTexture s_action_heat_target;
static int s_action_heat_w, s_action_heat_h;
static bool s_action_heat_supported = true;
static bool s_action_heat_engaged;

typedef struct ActionHeatPassState {
  ArRenderTargetState target_state;
  bool valid;
} ActionHeatPassState;

typedef struct ActionHeatMeshCache {
  ActionHeatRenderMesh mesh;
  ArRenderRectI viewport;
  int target_width, target_height, source_width;
  uint16_t game_frame;
  bool valid;
} ActionHeatMeshCache;

static ActionHeatPassState s_action_heat_saved_state;
static ActionHeatMeshCache s_action_heat_mesh_cache;

/* Effect builders are synchronous and presentation runs on one render thread,
 * so one reusable workspace covers actor and every depth-ordered decoration
 * pass. Keep these large bounded arrays out of automatic storage: the scene
 * batch alone is roughly half a MiB and can exhaust default Windows/custom
 * thread stacks before a backend driver gets its own frame. */
typedef struct ActionEffectRenderScratch {
  ActionEffectRenderBatch spell;
  ActionSceneEffectRenderBatch scene;
} ActionEffectRenderScratch;

static ActionEffectRenderScratch s_action_effect_render_scratch;

static PresentationUploadMirror s_action_bg1_mask_mirror;
static PresentationUploadMirror s_action_bg2_mask_mirror;
static bool s_action_bg1_mask_has_alpha;
static bool s_action_bg1_mask_ready;
static bool s_action_bg2_mask_has_alpha;
static bool s_action_bg2_mask_ready;
static uint32_t s_action_alpha_mask[kFrameSlotLayerTextureWidth * kFrameSlotAuthenticHeight];

static float ActionEffectBrightness(const FrameSlot *slot) {
  return slot && !(slot->inidisp & 0x80)
      ? (float)(slot->inidisp & 0x0f) / 15.0f : 0.0f;
}

/* Tile captures already contain master brightness. Apply it once to the
 * host-built effect colours, retaining opacity for alpha and premultiplied
 * target passes as well as additive and surface-light draws. */
static void FadeEffectVertices(const FrameSlot *slot,
                               ArRenderVertex2D *vertices, int count) {
  const float brightness = ActionEffectBrightness(slot);
  if (brightness == 1.0f) return;
  for (int i = 0; i < count; i++) {
    vertices[i].color.r *= brightness;
    vertices[i].color.g *= brightness;
    vertices[i].color.b *= brightness;
  }
}

static bool FrameUsesBg2Alpha(const FrameSlot *slot) {
  if (!slot || !slot->action_environmental_effects ||
      slot->action_scene_effects.decoration_overflow ||
      slot->action_scene_effects.decoration_count > kActionSceneDecorationMaxInstances)
    return false;
  for (unsigned i = 0; i < slot->action_scene_effects.decoration_count; i++) {
    const unsigned layer = slot->action_scene_effects.decorations[i].render_layer;
    if (layer == kActionEffectRenderLayer_Bg2Alpha ||
        layer == kActionEffectRenderLayer_Bg2HighAlpha)
      return true;
  }
  return false;
}

static bool FrameUsesAlphaBg2Mask(const FrameSlot *slot) {
  if (FrameUsesBg2Alpha(slot)) return true;
  if (!slot || !slot->action_environmental_effects ||
      slot->action_scene_effects.decoration_overflow ||
      slot->action_scene_effects.decoration_count > kActionSceneDecorationMaxInstances)
    return false;
  for (unsigned i = 0; i < slot->action_scene_effects.decoration_count; i++)
    if (slot->action_scene_effects.decorations[i].kind == kActionEffect_CaveWater ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_CastleSky)
      return true;
  return false;
}

float PresentActionEffects_Bg1Dimming(const FrameSlot *slot) {
  if (!slot || !slot->action_environmental_effects ||
      slot->action_scene_effects.decoration_overflow ||
      slot->action_scene_effects.decoration_count > kActionSceneDecorationMaxInstances)
    return 0;
  for (unsigned i = 0; i < slot->action_scene_effects.decoration_count; i++) {
    const ActionEffectInstance *effect = &slot->action_scene_effects.decorations[i];
    if (slot->diorama_map_group == kActRaiserMapGroup_Fillmore &&
        slot->diorama_map_number == 3 &&
        effect->kind == kActionEffect_CaveAmbientLight && effect->environment_room == 3 &&
        effect->phase == kActionEffectPhase_CaveEnvironment &&
        effect->render_layer == kActionEffectRenderLayer_ForegroundLight &&
        effect->projection_plane == kActionEffectProjectionPlane_Bg1 &&
        (effect->flags & kActionEffectFlag_Visible)) return .45f;
    if (slot->diorama_map_group == kActRaiserMapGroup_Bloodpool &&
        effect->environment_room == slot->diorama_map_number &&
        (effect->environment_room == 3 || effect->environment_room == 4 || effect->environment_room == 5 ||
         effect->environment_room == 7 || effect->environment_room == 8) &&
        effect->kind == kActionEffect_CastleLight &&
        effect->phase == kActionEffectPhase_CastleEnvironment &&
        effect->render_layer == kActionEffectRenderLayer_Bg1Plane &&
        effect->projection_plane == kActionEffectProjectionPlane_Bg1 &&
        effect->source_mask && (effect->flags & kActionEffectFlag_Visible))
      return effect->environment_room == 5 ? .42f : effect->environment_room == 8 ? .30f : .36f;
  }
  return 0;
}

static bool FrameUsesAlphaBg1Mask(const FrameSlot *slot) {
  if (PresentActionEffects_Bg1Dimming(slot) > 0) return true;
  if (!slot || !slot->action_environmental_effects ||
      slot->action_scene_effects.decoration_overflow ||
      slot->action_scene_effects.decoration_count > kActionSceneDecorationMaxInstances)
    return false;
  for (unsigned i = 0; i < slot->action_scene_effects.decoration_count; i++)
    if (slot->action_scene_effects.decorations[i].kind == kActionEffect_CastleLight ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_CastleWater ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_CastleMist ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_CaveSheen ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_BloodpoolWater ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_BloodpoolTimber ||
        slot->action_scene_effects.decorations[i].kind == kActionEffect_TempleGroundMist)
      return true;
  return false;
}

uint64_t PresentActionEffects_UploadMask(
    ArRenderDevice *device, int plane, const FrameSlot *slot,
    const uint8_t *pixels, int pitch_bytes) {
  if (plane == SR_PPU_OVERLAY_BG1) s_action_bg1_mask_ready = false;
  if (plane == SR_PPU_OVERLAY_BG2) s_action_bg2_mask_ready = false;
  if (!pixels || !slot || pitch_bytes <= 0 ||
      (plane != SR_PPU_OVERLAY_BG1 && plane != SR_PPU_OVERLAY_BG2))
    return 0;
  ArRenderTexture *texture = plane == SR_PPU_OVERLAY_BG1
      ? &s_action_bg1_mask_texture : &s_action_bg2_mask_texture;
  PresentationUploadMirror *mirror = plane == SR_PPU_OVERLAY_BG1
      ? &s_action_bg1_mask_mirror : &s_action_bg2_mask_mirror;
  if (!ArRenderTexture_IsValid(*texture)) {
    const ArRenderTextureDesc desc = {
      .width = kFrameSlotLayerTextureWidth,
      .height = kFrameSlotAuthenticHeight,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Multiply,
    };
    (void)ArRenderDevice_CreateTexture(
        device, &desc, texture);
  }
  if (ArRenderTexture_IsValid(*texture)) {
    const ArRenderRectI mask = {
      0, 0, slot->snes_width, slot->snes_height,
    };
    const bool alpha_mask = plane == SR_PPU_OVERLAY_BG2 ?
        FrameUsesAlphaBg2Mask(slot) : FrameUsesAlphaBg1Mask(slot);
    if (alpha_mask) {
      if (mask.w <= 0 || mask.w > kFrameSlotLayerTextureWidth || mask.h <= 0 ||
          mask.h > kFrameSlotAuthenticHeight || pitch_bytes < mask.w * 4)
        return 0;
      /* Native winner masks use opaque black outside BG2. Direct masked
       * geometry needs zero alpha there, for light, water and leaf passes. */
      for (int y = 0; y < mask.h; y++) {
        for (int x = 0; x < mask.w; x++) {
          uint32_t pixel;
          memcpy(&pixel, pixels + (size_t)y * pitch_bytes + x * 4, sizeof(pixel));
          s_action_alpha_mask[y * mask.w + x] =
              (pixel & 0x00ffffffu) ? 0xffffffffu : 0;
        }
      }
      pixels = (const uint8_t *)s_action_alpha_mask;
      pitch_bytes = mask.w * 4;
    }
    PresentationUploadResult result;
    if (PresentationUploadMirror_UploadArgb8888(
            mirror, device, *texture, pixels, mask.w, mask.h,
            pitch_bytes, mask.x, mask.y, &result)) {
      if (plane == SR_PPU_OVERLAY_BG1) {
        s_action_bg1_mask_has_alpha = alpha_mask;
        s_action_bg1_mask_ready = true;
      }
      if (plane == SR_PPU_OVERLAY_BG2) {
        s_action_bg2_mask_has_alpha = alpha_mask;
        s_action_bg2_mask_ready = true;
      }
      return result.uploaded_bytes;
    }
  }
  return 0;
}

static void DisableActionPlaneEffect(ArRenderDevice *device, const char *operation) {
  if (!s_action_plane_blend_supported) return;
  s_action_plane_blend_supported = false;
  fprintf(stderr,
          "[action-fx] flat BG-local effect unavailable at %s (%s); "
          "disabled\n",
          operation ? operation : "unknown operation",
          ArRenderDevice_LastError(device));
}

/* An unavailable enhancement can be skipped. An unknown active target cannot:
 * stop the scene before later passes draw into the effect's private texture. */
static void FailActionPlaneTargetState(ArRenderDevice *device,
                                       const char *operation) {
  DisableActionPlaneEffect(device, operation);
  SessionFatal_Request(
      "The action plane-effect pass could not restore the active render "
      "target (%s). Restart the game; if this repeats, update your graphics "
      "driver or disable action effects.",
      ArRenderDevice_LastError(device));
}

static void DisableActionHeat(ArRenderDevice *device, const char *operation) {
  if (!s_action_heat_supported) return;
  s_action_heat_supported = false;
  fprintf(stderr,
          "[action-fx] lava heat refraction unavailable at %s (%s); "
          "disabled\n",
          operation ? operation : "unknown operation",
          ArRenderDevice_LastError(device));
}

static void FailActionHeatTargetState(ArRenderDevice *device, const char *operation) {
  DisableActionHeat(device, operation);
  SessionFatal_Request(
      "The action heat-refraction pass could not restore the active render "
      "target (%s). Restart the game; if this repeats, update your graphics "
      "driver or disable Environmental effects.",
      ArRenderDevice_LastError(device));
}

static bool FrameUsesActionHeat(const FrameSlot *slot) {
  if (ActionEffectBrightness(slot) == 0 ||
      !slot->action_environmental_effects || slot->diorama_active ||
      ActRaiserRoom_ProfileFor(
          slot->diorama_map_group, slot->diorama_map_number) !=
              kActRaiserRoomProfile_AitosAct2Lava ||
      slot->action_scene_effects.decoration_overflow ||
      slot->action_scene_effects.decoration_count >
          kActionSceneDecorationMaxInstances)
    return false;
  const ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
  };
  for (uint8_t i = 0;
       i < slot->action_scene_effects.decoration_count; i++) {
    const ActionEffectInstance *effect =
        &slot->action_scene_effects.decorations[i];
    if (effect->kind == kActionEffect_AitosLavaReservoir &&
        effect->phase == kActionEffectPhase_AitosLavaReservoir &&
        ActionEffectProjection_IntersectsFlatViewport(
            &projection, effect))
      return true;
  }
  return false;
}

static ArRenderTexture EnsureActionHeatTarget(ArRenderDevice *device, int width, int height) {
  if (!s_action_heat_supported || width <= 0 || height <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_action_heat_target) &&
      s_action_heat_w == width &&
      s_action_heat_h == height)
    return s_action_heat_target;
  ArRenderDevice_DestroyTexture(device, s_action_heat_target);
  s_action_heat_target = ArRenderTexture_Invalid();
  s_action_heat_w = width;
  s_action_heat_h = height;
  const ArRenderTextureDesc desc = {
    .width = width,
    .height = height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          device, &desc, &s_action_heat_target)) {
    s_action_heat_w = s_action_heat_h = 0;
    DisableActionHeat(device, "target creation");
  }
  return s_action_heat_target;
}

static void ClearActionHeatSavedState(void) {
  s_action_heat_saved_state = (ActionHeatPassState){0};
}

static bool ActionHeatMeshMatches(
    const ActionHeatMeshCache *cache, uint16_t game_frame,
    ArRenderRectI viewport, int target_width, int target_height,
    int source_width) {
  return cache && cache->valid && cache->game_frame == game_frame &&
      cache->viewport.x == viewport.x && cache->viewport.y == viewport.y &&
      cache->viewport.w == viewport.w && cache->viewport.h == viewport.h &&
      cache->target_width == target_width &&
      cache->target_height == target_height &&
      cache->source_width == source_width;
}

static const ActionHeatRenderMesh *ActionHeatMeshFor(
    uint16_t game_frame, ArRenderRectI viewport,
    int target_width, int target_height, int source_width) {
  if (ActionHeatMeshMatches(
          &s_action_heat_mesh_cache, game_frame, viewport,
          target_width, target_height, source_width))
    return &s_action_heat_mesh_cache.mesh;
  s_action_heat_mesh_cache.valid = false;
  if (!ActionHeatRender_Build(
          game_frame,
          viewport,
          target_width, target_height,
          source_width, &s_action_heat_mesh_cache.mesh))
    return NULL;
  s_action_heat_mesh_cache.viewport = viewport;
  s_action_heat_mesh_cache.target_width = target_width;
  s_action_heat_mesh_cache.target_height = target_height;
  s_action_heat_mesh_cache.source_width = source_width;
  s_action_heat_mesh_cache.game_frame = game_frame;
  s_action_heat_mesh_cache.valid = true;
  return &s_action_heat_mesh_cache.mesh;
}

/* Route the world composite into a viewport-sized texture. Letterbox pixels
 * never enter this target, which avoids reserving and clearing memory that the
 * heat pass cannot display. The matching end pass clears the real output once
 * and resolves this texture with one subtly UV-warped mesh; HUD and host
 * overlays are intentionally drawn afterward. */
bool PresentActionHeat_Begin(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport) {
  if (s_action_heat_engaged || !FrameUsesActionHeat(slot) ||
      !EffectRenderer_Available() || !s_action_heat_supported)
    return false;
  if (viewport.w <= 0 || viewport.h <= 0) {
    DisableActionHeat(device, "invalid viewport");
    return false;
  }
  const ArRenderTexture target = EnsureActionHeatTarget(
      device, viewport.w, viewport.h);
  if (!ArRenderTexture_IsValid(target)) return false;
  ActionHeatPassState saved = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      device, target, &saved.target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (begin == kArRenderTargetBegin_StateLost)
      FailActionHeatTargetState(device, "failed-begin state restore");
    else
      DisableActionHeat(device, "target bind");
    return false;
  }
  saved.valid = true;
  s_action_heat_saved_state = saved;
  s_action_heat_engaged = true;
  return true;
}

ArRenderRectI PresentActionHeat_SceneViewport(ArRenderRectI output_viewport) {
  if (!s_action_heat_engaged) return output_viewport;
  return (ArRenderRectI){0, 0, output_viewport.w, output_viewport.h};
}

void PresentActionHeat_Cancel(ArRenderDevice *device) {
  if (!s_action_heat_engaged) return;
  const ActionHeatPassState saved = s_action_heat_saved_state;
  const bool target_restored =
      saved.valid && ArRenderDevice_EndTarget(
          device, &saved.target_state);
  ClearActionHeatSavedState();
  s_action_heat_engaged = false;
  if (!target_restored)
    FailActionHeatTargetState(device, "cancel state restore");
}

void PresentActionHeat_End(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport) {
  if (!s_action_heat_engaged) return;
  const ActionHeatPassState saved = s_action_heat_saved_state;
  ClearActionHeatSavedState();
  s_action_heat_engaged = false;
  if (!saved.valid || !ArRenderDevice_EndTarget(
          device, &saved.target_state)) {
    FailActionHeatTargetState(device, "target restore");
    return;
  }

  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(
          device,
          viewport,
          black, black, &output_frame)) {
    DisableActionHeat(device, "scene resolve scope");
    return;
  }
  const ArRenderRectI local_viewport = {0, 0, viewport.w, viewport.h};
  const ActionHeatRenderMesh *mesh = ActionHeatMeshFor(
      slot->action_scene_effects.game_frame, local_viewport,
      s_action_heat_w, s_action_heat_h, slot->visible_width);
  const bool warped = mesh && ArRenderDevice_DrawGeometry(
      device, s_action_heat_target,
      mesh->vertices, mesh->vertex_count,
      mesh->indices, mesh->index_count);
  bool fallback = false;
  if (!warped) {
    /* A runtime geometry rejection must drop only the enhancement, not the
     * already-rendered world. Resolve the captured scene without refraction
     * for this frame, then disable future heat attempts. */
    const ArRenderRectF destination = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    fallback = ArRenderDevice_DrawTexture(
        device, s_action_heat_target, NULL, &destination);
  }
  if (!ArRenderOutputFrame_Finish(&output_frame)) {
    DisableActionHeat(device, "output-state restore");
    SessionFatal_Request(
        "The action heat-refraction pass could not restore the output "
        "viewport and clip state (%s). Restart the game; if this repeats, "
        "update your graphics driver or disable Environmental effects.",
        ArRenderDevice_LastError(device));
  } else if (!warped)
    DisableActionHeat(device, fallback ? "refraction mesh" : "fallback resolve");
}

/* ── Action-stage presentation effects ────────────────────────────────── */

static void SubmitSurfaceLight(ArRenderDevice *device, const EffectBatch *batch) {
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_Light,
  };
  /* Custom blending is optional (notably on software renderers). A rejection
   * must not trip the shared capability latch for ordinary spell/actor effects. */
  if (ArRenderDevice_DrawGeometryWithState(device, ArRenderTexture_Invalid(),
          batch->vertices, batch->vertex_count, batch->indices, batch->index_count, &state))
    return;
  s_action_surface_light_supported = false;
  fprintf(stderr, "[action-fx] surface lighting unavailable (%s); disabled\n",
          ArRenderDevice_LastError(device));
}

_Static_assert(kActionEffectObjPriorityCount ==
                   kDioramaObjectPriorityCount,
               "action effects and diorama must agree on OBJ bands");

void PresentActionEffects_Draw(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    const DioramaProjection *diorama_projection) {
  if (ActionEffectBrightness(slot) == 0 || (!slot->action_effects.visible_count &&
                !slot->action_scene_effects.visible_count &&
                !slot->action_scene_effects.decoration_visible_count) ||
      (!slot->action_effect_lighting && !slot->action_effect_particles &&
       !slot->action_environmental_effects) ||
      !EffectRenderer_Available())
    return;

  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .ws_extra_top = slot->ws_extra_top,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    .diorama_projection = diorama_projection,
    .viewport = {viewport.x, viewport.y, viewport.w, viewport.h},
  };
  ActionEffectRenderBatch *geometry = &s_action_effect_render_scratch.spell;
  ActionSceneEffectRenderBatch *scene_geometry =
      &s_action_effect_render_scratch.scene;
  geometry->vertex_count = geometry->index_count = 0;
  scene_geometry->vertex_count = scene_geometry->index_count = 0;
  if ((slot->action_effects.visible_count &&
       !ActionEffectRender_Build(
           &slot->action_effects, slot->action_effect_lighting,
           slot->action_effect_particles,
           ActionEffectProjection_ProjectPoint, &projection, geometry)) ||
      (slot->action_scene_effects.visible_count &&
       !ActionSceneEffectRender_Build(
           &slot->action_scene_effects, slot->action_effect_lighting,
           slot->action_effect_particles,
           ActionEffectProjection_ProjectPoint, &projection,
           scene_geometry)))
    return;
  const int actor_vertex_count = scene_geometry->vertex_count;
  const int actor_index_count = scene_geometry->index_count;
  FadeEffectVertices(slot, geometry->vertices, geometry->vertex_count);
  FadeEffectVertices(slot, scene_geometry->vertices, scene_geometry->vertex_count);

  EffectBatch spell_batch = {
    .vertices = geometry->vertices,
    .indices = geometry->indices,
    .vertex_count = geometry->vertex_count,
    .index_count = geometry->index_count,
    .vertex_capacity = kActionEffectRenderMaxVertices,
    .index_capacity = kActionEffectRenderMaxIndices,
  };
  EffectBatch scene_batch = {
    .vertices = scene_geometry->vertices,
    .indices = scene_geometry->indices,
    .vertex_count = scene_geometry->vertex_count,
    .index_count = scene_geometry->index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  bool spell_submitted = true;
  bool scene_submitted = true;
  if (spell_batch.index_count || scene_batch.index_count) {
    spell_submitted = EffectRenderer_Submit(
        device, &spell_batch, kArRenderBlendMode_Add);
    scene_submitted = EffectRenderer_Submit(
        device, &scene_batch, kArRenderBlendMode_Add);
  }

  /* Map-derived world decorations own a separate captured list and reuse the
   * same scratch batch after actor submission. This preserves the actor
   * budget without allocating another workspace. BG2 decorations and bottom
   * atmosphere are submitted by their dedicated depth-ordered passes. */
  bool decoration_submitted = false;
  if (slot->action_environmental_effects &&
      slot->action_scene_effects.decoration_visible_count &&
      ActionSceneDecorationRender_Build(
          &slot->action_scene_effects,
          kActionEffectRenderLayer_WorldOverlay,
          true, true,
          ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &projection,
          scene_geometry) && scene_geometry->index_count) {
    scene_batch.vertex_count = scene_geometry->vertex_count;
    scene_batch.index_count = scene_geometry->index_count;
    FadeEffectVertices(slot, scene_geometry->vertices, scene_geometry->vertex_count);
    decoration_submitted = EffectRenderer_Submit(
        device, &scene_batch, kArRenderBlendMode_Add);
  }
  /* Contact clouds are translucent matter, so share the bounded scratch but
   * submit with source alpha after the additive airborne specks. */
  if (slot->action_environmental_effects &&
      slot->action_scene_effects.decoration_visible_count &&
      ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, kActionEffectRenderLayer_WorldDust, false, true,
          ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &projection,
          scene_geometry) && scene_geometry->index_count) {
    scene_batch.vertex_count = scene_geometry->vertex_count;
    scene_batch.index_count = scene_geometry->index_count;
    FadeEffectVertices(slot, scene_geometry->vertices, scene_geometry->vertex_count);
    decoration_submitted |= EffectRenderer_Submit(
        device, &scene_batch, kArRenderBlendMode_Alpha);
  }
  if (slot->action_environmental_effects && s_action_surface_light_supported &&
      slot->action_scene_effects.decoration_visible_count &&
      ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, kActionEffectRenderLayer_ForegroundLight,
          true, false, ActionEffectProjection_ProjectPoint,
          ActionEffectProjection_ClipBounds, &projection, scene_geometry) &&
      scene_geometry->index_count) {
    scene_batch.vertex_count = scene_geometry->vertex_count;
    scene_batch.index_count = scene_geometry->index_count;
    FadeEffectVertices(slot, scene_geometry->vertices, scene_geometry->vertex_count);
    SubmitSurfaceLight(device, &scene_batch);
  }
  /* One line, once per process: the whole path (WRAM identity -> capture ->
   * projection -> geometry submit) either produced pixels or it did not, and
   * a run's console.log should say which without anyone re-deriving it. The
   * silent version of this is what let a 16-bit read of the animation-bank
   * BYTE reject every spell with no visible symptom but "nothing happens". */
  static bool announced;
  if (!announced && geometry->index_count && spell_submitted) {
    announced = true;
    fprintf(stderr, "[action-fx] first spell geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_effects.visible_count, geometry->vertex_count,
            geometry->index_count, slot->action_effect_lighting,
            slot->action_effect_particles);
  }
  static bool announced_scene;
  if (!announced_scene && actor_index_count && scene_submitted) {
    announced_scene = true;
    fprintf(stderr,
            "[action-fx] first scene accent geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_scene_effects.visible_count,
            actor_vertex_count, actor_index_count,
            slot->action_effect_lighting, slot->action_effect_particles);
  }
  static bool announced_decorations;
  if (!announced_decorations && decoration_submitted) {
    announced_decorations = true;
    fprintf(stderr,
            "[action-fx] first map decoration geometry submitted\n");
  }
}

/* Attachment is draw order, not the source camera. For example, shoreline
 * mist uses BG1 coordinates but belongs after BG2-high. Flat composition uses
 * winner masks; Diorama instead relies on later foreground/actor planes. */
typedef struct ActionDecorationPass {
  uint8_t layer;
  int attachment;
  int mask_plane; /* -1: Diorama-only atmosphere, no flat winner mask. */
  ArRenderBlendMode blend;
  bool diorama_lighting;
  bool finite_plane_only;
  const char *label;
} ActionDecorationPass;

/* Order is also the established flat composition order. */
static const ActionDecorationPass kDecorationPasses[] = {
  {kActionEffectRenderLayer_Bg1Plane, SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Add, true, false, "BG1-local decoration"},
  {kActionEffectRenderLayer_Bg1HighPlane, kDioramaPlane_Bg1Hi, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Add, true, false, "BG1-high lava decoration"},
  {kActionEffectRenderLayer_Bg2Plane, SR_PPU_OVERLAY_BG2, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Add, true, false, "BG2-local decoration"},
  {kActionEffectRenderLayer_Bg2HighPlane, kDioramaPlane_Bg2Hi, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Add, true, false, "BG2-high water decoration"},
  {kActionEffectRenderLayer_Bg2Alpha, SR_PPU_OVERLAY_BG2, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Alpha, true, false, "BG2 alpha atmosphere"},
  {kActionEffectRenderLayer_Bg2HighAlpha, kDioramaPlane_Bg2Hi, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Alpha, false, false, "BG2-high lake mist"},
  {kActionEffectRenderLayer_Bg1Mist, SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Alpha, false, false, "temple ground mist"},
  {kActionEffectRenderLayer_Atmosphere, SR_PPU_OVERLAY_BG2, -1,
   kArRenderBlendMode_Alpha, true, true, "waterfall bottom atmosphere"},
};

static ActionEffectProjectionContext DecorationProjection(
    const FrameSlot *slot, ArRenderRectI viewport, const DioramaProjection *diorama) {
  return (ActionEffectProjectionContext){
    .bg1_camera_x = slot->bg1_camera_x, .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x, .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra, .ws_extra_top = diorama ? slot->ws_extra_top : 0,
    .visible_x0 = slot->visible_x0, .visible_width = slot->visible_width,
    .snes_height = slot->snes_height, .diorama_projection = diorama,
    /* Flat geometry is target-local; its submission restores viewport offset. */
    .viewport = {diorama ? viewport.x : 0, diorama ? viewport.y : 0,
                 viewport.w, viewport.h},
  };
}

static EffectBatch SceneBatchView(ActionSceneEffectRenderBatch *geometry) {
  return (EffectBatch){
    .vertices = geometry->vertices, .indices = geometry->indices,
    .vertex_count = geometry->vertex_count, .index_count = geometry->index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
}

static bool DecorationAttachmentVisible(int plane, const DioramaProjection *projection) {
  switch (plane) {
    case SR_PPU_OVERLAY_BG1: return projection->bg1_plane.valid;
    case SR_PPU_OVERLAY_BG2:
      return projection->bg2_plane.valid || projection->bg2_skybox.count;
    case kDioramaPlane_Bg1Hi: return projection->bg1_high_plane.valid;
    case kDioramaPlane_Bg2Hi: return projection->bg2_high_plane.valid;
    default: return false;
  }
}

void PresentActionEffects_DrawDioramaPlane(
    void *userdata, int plane, const DioramaProjection *diorama_projection) {
  PresentActionPlaneEffectContext *context = userdata;
  if (!context || ActionEffectBrightness(context->slot) == 0 ||
      !context->slot->action_environmental_effects ||
      !context->slot->action_scene_effects.decoration_visible_count ||
      !diorama_projection || !EffectRenderer_Available() ||
      !DecorationAttachmentVisible(plane, diorama_projection)) return;
  const FrameSlot *slot = context->slot;
  ActionEffectProjectionContext projection =
      DecorationProjection(slot, context->viewport, diorama_projection);
  ActionSceneEffectRenderBatch *geometry = &s_action_effect_render_scratch.scene;
  static bool announced[kActionEffectRenderLayer_Count];
  for (unsigned i = 0; i < sizeof(kDecorationPasses)/sizeof(kDecorationPasses[0]); i++) {
    const ActionDecorationPass *pass = &kDecorationPasses[i];
    if (pass->attachment != plane ||
        (pass->finite_plane_only && diorama_projection->bg2_skybox.count)) continue;
    if (pass->layer == kActionEffectRenderLayer_Bg2Alpha && !FrameUsesBg2Alpha(slot))
      continue;
    if (!ActionSceneDecorationRender_Build(&slot->action_scene_effects, pass->layer,
            pass->diorama_lighting, true, ActionEffectProjection_ProjectPoint,
            ActionEffectProjection_ClipBounds, &projection, geometry)) {
      /* A malformed primary batch suppresses its attached atmosphere too. */
      if (pass->blend == kArRenderBlendMode_Add) return;
      continue;
    }
    if (!geometry->index_count) continue;
    FadeEffectVertices(slot, geometry->vertices, geometry->vertex_count);
    EffectBatch batch = SceneBatchView(geometry);
    if (EffectRenderer_Submit(context->device, &batch, pass->blend) &&
        !announced[pass->layer]) {
      announced[pass->layer] = true;
      fprintf(stderr, "[action-fx] first %s geometry submitted (Diorama)\n", pass->label);
    }
  }
}

static ArRenderTexture EnsureActionPlaneEffectTarget(ArRenderDevice *device, int w, int h) {
  if (!ArRenderDevice_IsReady(device) || w <= 0 || h <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_action_plane_effect_target) &&
      s_action_plane_effect_w == w &&
      s_action_plane_effect_h == h)
    return s_action_plane_effect_target;
  ArRenderDevice_DestroyTexture(
      device, s_action_plane_effect_target);
  s_action_plane_effect_target = ArRenderTexture_Invalid();
  s_action_plane_effect_w = w;
  s_action_plane_effect_h = h;
  const ArRenderTextureDesc desc = {
    .width = w,
    .height = h,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_AddPremultiplied,
  };
  if (!ArRenderDevice_CreateTexture(
          device, &desc, &s_action_plane_effect_target)) {
    DisableActionPlaneEffect(device, "premultiplied target creation");
  }
  return s_action_plane_effect_target;
}

static bool DrawAlphaMaskedGeometry(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    ArRenderTexture mask, bool alpha_blend, ActionSceneEffectRenderBatch *geometry) {
  /* The binary winner mask has white/opaque winning pixels and transparent
   * black elsewhere. Sample it in native screen coordinates: geometry color
   * and alpha are masked together, with no intermediate target or resolves.
   * The caller's scene viewport clips particles at the output edges. */
  for (int i = 0; i < geometry->vertex_count; i++) {
    ArRenderVertex2D *v = &geometry->vertices[i];
    v->tex_coord.x = (slot->visible_x0 + v->position.x * slot->visible_width / viewport.w) /
        kFrameSlotLayerTextureWidth;
    v->tex_coord.y = (v->position.y * slot->snes_height / viewport.h) /
        kFrameSlotAuthenticHeight;
    v->position.x += viewport.x;
    v->position.y += viewport.y;
  }
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend,
    .blend = alpha_blend ? kArRenderBlendMode_Alpha : kArRenderBlendMode_Add,
  };
  return ArRenderDevice_DrawGeometryWithState(device, mask, geometry->vertices,
      geometry->vertex_count, geometry->indices, geometry->index_count, &state);
}

static bool DrawActionPlaneEffectFlat(ArRenderDevice *device,
    const FrameSlot *slot, ArRenderRectI viewport, const ActionDecorationPass *pass) {
  const uint8_t render_layer = pass->layer;
  const bool bg1 = pass->mask_plane == SR_PPU_OVERLAY_BG1;
  const bool alpha = pass->blend == kArRenderBlendMode_Alpha;
  const bool mask_ready = bg1 ? s_action_bg1_mask_ready : s_action_bg2_mask_ready;
  const bool mask_has_alpha = bg1 ? s_action_bg1_mask_has_alpha : s_action_bg2_mask_has_alpha;
  const bool mask_valid = bg1 ? slot->action_bg1_mask_valid : slot->action_bg2_mask_valid;
  const ArRenderTexture mask_texture = bg1 ? s_action_bg1_mask_texture : s_action_bg2_mask_texture;
  /* Never reuse stale occlusion after an upload failure. Alpha passes require
   * an alpha mask; opaque legacy masks retain their intermediate-target path. */
  if ((!bg1 || alpha || mask_has_alpha) && !mask_ready) return true;
  if (alpha && !mask_has_alpha) return true;
  if (!slot->action_environmental_effects || !mask_valid ||
      !slot->action_scene_effects.decoration_visible_count ||
      !ArRenderTexture_IsValid(mask_texture) ||
      !s_action_plane_blend_supported ||
      !EffectRenderer_Available())
    return true;
  ActionEffectProjectionContext projection = DecorationProjection(slot, viewport, NULL);
  ActionSceneEffectRenderBatch *geometry =
      &s_action_effect_render_scratch.scene;
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, render_layer,
          true, true,
          ActionEffectProjection_ProjectPoint,
          ActionEffectProjection_ClipBounds, &projection, geometry) ||
      !geometry->index_count)
    return true;
  FadeEffectVertices(slot, geometry->vertices, geometry->vertex_count);
  if (mask_has_alpha) {
    if (!DrawAlphaMaskedGeometry(device, slot, viewport, mask_texture, alpha, geometry))
      DisableActionPlaneEffect(device, "masked geometry submit");
    return true;
  }
  const ArRenderTexture target =
      EnsureActionPlaneEffectTarget(device, viewport.w, viewport.h);
  if (!ArRenderTexture_IsValid(target)) return true;

  ArRenderTargetState target_state = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      device, target, &target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (begin == kArRenderTargetBegin_StateLost) {
      FailActionPlaneTargetState(device, "failed-begin state restore");
      return false;
    }
    DisableActionPlaneEffect(device, "effect-target bind");
    return true;
  }
  const bool target_ready = ArRenderDevice_Clear(
      device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  if (!target_ready)
    DisableActionPlaneEffect(device, "effect-target clear");
  EffectBatch batch = SceneBatchView(geometry);
  bool submitted = false;
  bool masked = false;
  if (target_ready)
    submitted = EffectRenderer_Submit(device, &batch, kArRenderBlendMode_Add);
  if (submitted && s_action_plane_blend_supported) {
    const ArRenderRectF src = {
      (float)slot->visible_x0, 0.0f,
      (float)slot->visible_width, (float)slot->snes_height,
    };
    const ArRenderRectF dst = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    const ArRenderDrawState mask_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Multiply,
    };
    masked = ArRenderDevice_DrawTextureWithState(
        device, mask_texture, &src, &dst, &mask_state);
    if (!masked) DisableActionPlaneEffect(device, "winner-mask draw");
  }
  if (!ArRenderDevice_EndTarget(device, &target_state)) {
    FailActionPlaneTargetState(device, "render-state restore");
    return false;
  }
  bool composited = false;
  if (masked && s_action_plane_blend_supported) {
    const ArRenderRectF dst = {
      (float)viewport.x, (float)viewport.y,
      (float)viewport.w, (float)viewport.h,
    };
    composited = ArRenderDevice_DrawTexture(
        device, target, NULL, &dst);
    if (!composited) DisableActionPlaneEffect(device, "masked-target composite");
  }
  static bool announced[kActionEffectRenderLayer_Count];
  if (render_layer < kActionEffectRenderLayer_Count &&
      !announced[render_layer] && composited) {
    announced[render_layer] = true;
    fprintf(stderr,
            "[action-fx] first %s geometry submitted "
            "(flat, winner-masked)\n",
            pass->label);
  }
  return true;
}

bool PresentActionEffects_DrawFlatPlanes(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport) {
  if (ActionEffectBrightness(slot) == 0) return true;
  const float dimming = PresentActionEffects_Bg1Dimming(slot);
  if (dimming > 0 && viewport.w > 0 && viewport.h > 0 && slot->action_bg1_mask_valid &&
      s_action_plane_blend_supported && s_action_bg1_mask_ready && s_action_bg1_mask_has_alpha &&
      ArRenderTexture_IsValid(s_action_bg1_mask_texture)) {
    /* Darken winning BG1 pixels before mist/light. The zero-alpha holes in
     * this mask preserve actors, HUD and other backgrounds. No scene resolve. */
    ActionSceneEffectRenderBatch *geometry = &s_action_effect_render_scratch.scene;
    const ArRenderPointF corners[] = {{0,0},{viewport.w,0},
                                      {viewport.w,viewport.h},{0,viewport.h}};
    for (unsigned i = 0; i < 4; i++)
      geometry->vertices[i] = (ArRenderVertex2D){corners[i],{0,0,0,dimming},{0,0}};
    const int32_t indices[] = {0,1,2,0,2,3};
    memcpy(geometry->indices,indices,sizeof(indices));
    geometry->vertex_count = 4;
    geometry->index_count = 6;
    if (!DrawAlphaMaskedGeometry(device,slot,viewport,s_action_bg1_mask_texture,true,geometry))
      DisableActionPlaneEffect(device,"temple scenery dimming");
  }
  for (unsigned i = 0; i < sizeof(kDecorationPasses)/sizeof(kDecorationPasses[0]); i++) {
    const ActionDecorationPass *pass = &kDecorationPasses[i];
    if (pass->mask_plane < 0) continue;
    if (!DrawActionPlaneEffectFlat(device, slot, viewport, pass)) return false;
  }
  return true;
}

void PresentActionEffects_Reset(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_action_bg1_mask_texture);
  ArRenderDevice_DestroyTexture(device, s_action_bg2_mask_texture);
  ArRenderDevice_DestroyTexture(
      device, s_action_plane_effect_target);
  ArRenderDevice_DestroyTexture(device, s_action_heat_target);
  s_action_bg1_mask_texture = ArRenderTexture_Invalid();
  s_action_bg2_mask_texture = ArRenderTexture_Invalid();
  s_action_plane_effect_target = ArRenderTexture_Invalid();
  s_action_plane_effect_w = s_action_plane_effect_h = 0;
  s_action_plane_blend_supported = true;
  s_action_surface_light_supported = true;
  s_action_heat_target = ArRenderTexture_Invalid();
  ClearActionHeatSavedState();
  s_action_heat_mesh_cache = (ActionHeatMeshCache){0};
  s_action_heat_w = s_action_heat_h = 0;
  s_action_heat_supported = true;
  s_action_heat_engaged = false;
  PresentationUploadMirror_Reset(&s_action_bg1_mask_mirror);
  PresentationUploadMirror_Reset(&s_action_bg2_mask_mirror);
  s_action_bg1_mask_has_alpha = s_action_bg1_mask_ready = false;
  s_action_bg2_mask_has_alpha = false;
  s_action_bg2_mask_ready = false;
}
