/* SIM 3D shadows: the directional light, the accumulated screen-space caster
 * mask, and its separable blur. Split out of present_sim3d.c; the definitions
 * are unchanged.
 *
 * The mask is a screen-space target so overlapping casters accumulate once.
 * An elevated town hands its composite to the shared depth pass instead, where
 * it is sampled only by visible terrain-top geometry -- so this unit owns both
 * paths and the choice between them. */

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present_internal.h"
#include "present_sim3d_internal.h"
#include "present_sim3d_project.h"
#include "present_sim3d_shadows.h"
#include "render/render_device.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim3d.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_shadow_effect_backend.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

extern ArRenderDevice g_render_device;
extern ArRenderTexture g_sim_obj_atlas_texture;

/* The ordinary D4 mask remains a screen-space target so overlapping casters
 * accumulate once and soft blur stays inexpensive. Elevated towns defer its
 * composite to the shared depth pass, where it is sampled only by visible
 * terrain-top receiver geometry. */
static ArRenderTexture s_sim_shadow_texture;
static ArRenderTexture s_sim_shadow_scratch;
static int s_sim_shadow_w, s_sim_shadow_h;
static bool s_sim_shadow_unavailable;
static bool s_sim_shadow_scratch_alloc_failed;

enum {
  kSimShadowVerticesPerCaster = 4,
  kSimShadowIndicesPerCaster = 6,
  kSimShadowMaxVertices =
      kSimMaxRenderObjects * kSimShadowVerticesPerCaster,
  kSimShadowMaxIndices =
      kSimMaxRenderObjects * kSimShadowIndicesPerCaster,
};

/* Every actor silhouette samples the same atlas with the same material state.
 * Accumulate them into one geometry submission; primitive order remains the
 * source object order, while the renderer avoids up to 128 tiny API calls. */
static ArRenderVertex2D s_sim_shadow_vertices[kSimShadowMaxVertices];
static int32_t s_sim_shadow_indices[kSimShadowMaxIndices];

/* The directional light, resolved into shear per world unit of height (the
 * light is infinitely far and the ground is always z=0, so only the ratio
 * matters). Elevation 90 is straight overhead and shears nothing.
 *
 * The shipped default is deliberately near-overhead: the ground point then
 * sits directly under the lifted billboard on screen, so the gap between actor
 * and shadow reads unambiguously as altitude. A strongly angled light slides
 * the shadow sideways, where the eye reads the offset as lateral position
 * instead. Both angles are player settings, so this is a starting point rather
 * than a constraint. */
const float kPi = 3.14159265f;

void SimShadowLight(const FrameSlot *slot, float *light_x,
                           float *light_y) {
  float elevation = (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  float azimuth = (float)slot->sim.light_azimuth_deg * kPi / 180.0f;
  float sine = sinf(elevation);
  /* cot(elevation), clamped so a near-horizon light cannot throw a shadow to
   * infinity and blow out the mask. */
  float shear = sine > 0.05f ? cosf(elevation) / sine : 20.0f;
  if (shear > 4.0f) shear = 4.0f;
  if (shear < 0.0f) shear = 0.0f;
  *light_x = shear * cosf(azimuth);
  *light_y = shear * sinf(azimuth);
}
/* Footprint shrink per world unit of height. A caster on the standard 24px
 * flight plane sits at height_world = 24/224, so 6.0 puts its shadow at
 * 1/(1 + 24/224*6) = ~61% -- enough to read as "up there" without the shadow
 * losing its shape (4.0, the original value, gave ~70% and read as too close
 * to the ground). This is the right knob for that reading rather than
 * `height_scale_x100`: the height dial lifts the sprite AND shears its shadow,
 * reframing the actor against the map, while this only resizes the footprint.
 *
 * Only casters with a nonzero classified height are affected, which is exactly
 * the flying class -- the angel and enemy classes $12-$15 (Sim3D_ClassifyObject,
 * sim_render_metadata.c) -- plus the 8px semi-grounded Napper pluck. Grounded
 * townspeople, scene composites, and every fixed-tier object stay at
 * footprint 1.0, so their silhouettes still meet their own feet. */
static const float kSimShadowHeightShrink = 6.0f;

/* Extra billboard scale on top of the perspective scale the lift already
 * produces. That true component is only about 1.5% at the standard flight
 * plane -- correct, and far too subtle to read -- so `height_pop_pct` adds a
 * deliberate presentation pop, normalized against the catalogue flight plane
 * so the setting's percentage means what it says. It scales the sprite in
 * place rather than biasing its depth: pulling a flyer toward the camera would
 * move it back down-screen and close the very gap to its own shadow that sells
 * the altitude. Paired with kSimShadowHeightShrink, a rising actor grows while
 * its shadow shrinks, which is what reads as height.
 *
 * The alternative -- shrinking the ground and grounded actors instead -- was
 * considered and rejected: scaling the ground with its actors is just a camera
 * zoom-out (same relative effect, but it reframes the town and fights the
 * distance setting), and shrinking grounded actors alone breaks their
 * footprint against the map tiles they stand on. A flyer has no such fixed
 * reference, so it is the cheap place to put the difference. */
float SimBillboardHeightPop(SDL_Rect source, float height_world,
                                   unsigned height_pop_pct) {
  if (height_world <= 0.0f || !height_pop_pct || source.h <= 0) return 1.0f;
  float reference = (float)kSimVirtualHeight_Flying / (float)source.h;
  if (reference <= 0.0f) return 1.0f;
  return 1.0f + (height_world / reference) * (float)height_pop_pct /
      (float)kPercentScale;
}
/* How much of a caster's art height becomes ground depth. A billboard has no
 * depth, so shearing its silhouette along the light the way a solid body would
 * collapses it to a sliver under this shallow camera; the silhouette is
 * instead laid flat on the ground, foreshortened by the same projection as the
 * ground texture, and shortened here because the light is high. */
static const float kSimShadowFootprintDepth = 0.6f;

static ArRenderTexture CreateSimShadowTarget(int w, int h) {
  ArRenderTexture texture = ArRenderTexture_Invalid();
  const ArRenderTextureDesc desc = {
    .width = w,
    .height = h,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Alpha,
  };
  (void)ArRenderDevice_CreateTexture(&g_render_device, &desc, &texture);
  return texture;
}

static ArRenderTexture EnsureSimShadowTexture(int w, int h) {
  if (!ArRenderDevice_IsReady(&g_render_device) || w <= 0 || h <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_sim_shadow_texture) &&
      s_sim_shadow_w == w && s_sim_shadow_h == h)
    return s_sim_shadow_texture;
  if (s_sim_shadow_unavailable) return ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, s_sim_shadow_texture);
  ArRenderDevice_DestroyTexture(&g_render_device, s_sim_shadow_scratch);
  s_sim_shadow_texture = ArRenderTexture_Invalid();
  s_sim_shadow_scratch = ArRenderTexture_Invalid();
  s_sim_shadow_scratch_alloc_failed = false;
  s_sim_shadow_texture = CreateSimShadowTarget(w, h);
  s_sim_shadow_w = w;
  s_sim_shadow_h = h;
  if (!ArRenderTexture_IsValid(s_sim_shadow_texture)) {
    /* A large target rejection is stable for this renderer generation. Do not
     * turn an optional shadow into a per-frame allocation/stutter loop. */
    s_sim_shadow_unavailable = true;
    fprintf(stderr, "[sim3d-d4] shadow mask target unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
  }
  return s_sim_shadow_texture;
}

/* D4b separable blur. The shadow mask is pure alpha, so a box blur is exactly
 * a weighted sum of alpha taps. The generated Metal/Vulkan/D3D12 shader does
 * that sum in one draw per axis. The original custom-blend implementation
 * remains the fallback for renderers without custom GPU shader support.
 *
 * Two passes over one axis each, ping-ponging through the scratch target:
 * the shader costs two full-target draws rather than the fallback's 2N. */
enum {
  kSimShadowBlurTaps = 7,
  /* A pair of full-size ARGB shadow targets costs about 63 MiB at 4K before
   * backend alignment. Shadows contain only soft silhouettes, so cap their
   * working resolution at roughly 1440p and let linear sampling restore the
   * viewport size. This leaves 1080p and 1440p untouched while bounding 4K+
   * memory and fill cost. */
  kSimShadowMaxTargetPixels = 4 * 1024 * 1024,
};

typedef struct SimShadowAxisResult {
  PresentationOutcome outcome;
  bool destination_valid;
} SimShadowAxisResult;

static SimShadowAxisResult BlurSimShadowAxis(
    ArRenderTexture source, ArRenderTexture destination,
    int w, int h, float radius, bool horizontal) {
  SimShadowAxisResult result = {
    kPresentationOutcome_Complete, false,
  };
  ArRenderTargetState target_state = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      &g_render_device, destination, &target_state);
  if (begin == kArRenderTargetBegin_StateLost) {
    result.outcome = kPresentationOutcome_CoreFailure;
    return result;
  }
  if (begin != kArRenderTargetBegin_Ready) {
    result.outcome = kPresentationOutcome_OptionalOmitted;
    return result;
  }
  bool valid = ArRenderDevice_Clear(
      &g_render_device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});

  if (valid && SimShadowEffectBackend_IsAvailable(&g_render_device)) {
    const SimShadowBlurEffectParams params = {
      .texel_x = horizontal ? 1.0f / (float)w : 0.0f,
      .texel_y = horizontal ? 0.0f : 1.0f / (float)h,
      .radius = radius,
    };
    const bool bound = SimShadowEffectBackend_BindBlur(
        &g_render_device, &params);
    if (bound) {
      /* Once submitted, do not run the fallback on a backend draw error: a
       * backend may already have recorded the draw, and repeating it would
       * double the shadow. The next axis/frame can still continue safely. */
      const ArRenderDrawState opaque = {
        .flags = kArRenderDrawState_Blend,
        .blend = kArRenderBlendMode_Opaque,
      };
      valid = ArRenderDevice_DrawTextureWithState(
          &g_render_device, source, NULL, NULL, &opaque);
      if (valid)
        Sim3DPerformance_AddDraw(0, 0);
      const bool gpu_state_restored =
          SimShadowEffectBackend_Unbind(&g_render_device);
      const bool target_restored = ArRenderDevice_EndTarget(
          &g_render_device, &target_state);
      if (!gpu_state_restored || !target_restored) {
        result.outcome = kPresentationOutcome_CoreFailure;
        return result;
      }
      result.destination_valid = valid;
      if (!valid)
        result.outcome = kPresentationOutcome_OptionalOmitted;
      return result;
    }
    const bool gpu_state_restored =
        SimShadowEffectBackend_Unbind(&g_render_device);
    if (!gpu_state_restored) {
      (void)ArRenderDevice_EndTarget(&g_render_device, &target_state);
      result.outcome = kPresentationOutcome_CoreFailure;
      return result;
    }
    /* Binding failed before a draw was submitted. Keep the temporary target
     * active and run the established multi-draw fallback into it; ending the
     * scope here would redirect the fallback to the caller and then restore
     * the same target a second time below. */
  }

  if (valid) {
    int half = kSimShadowBlurTaps / 2;
    const uint8_t tap_alpha = (uint8_t)(255 / kSimShadowBlurTaps);
    const ArRenderDrawState tap_state = {
      .flags = kArRenderDrawState_Tint | kArRenderDrawState_Blend,
      .tint = {1.0f, 1.0f, 1.0f, (float)tap_alpha / 255.0f},
      .blend = kArRenderBlendMode_AlphaAccumulate,
    };
    for (int tap = -half; valid && tap <= half; tap++) {
      float offset = radius * (float)tap / (float)half;
      const ArRenderRectF destination_rect = {
        horizontal ? offset : 0.0f, horizontal ? 0.0f : offset,
        (float)w, (float)h,
      };
      valid = ArRenderDevice_DrawTextureWithState(
          &g_render_device, source, NULL, &destination_rect, &tap_state);
      if (valid) Sim3DPerformance_AddDraw(0, 0);
    }
  }
  const bool target_restored = ArRenderDevice_EndTarget(
      &g_render_device, &target_state);
  if (!target_restored) {
    result.outcome = kPresentationOutcome_CoreFailure;
    return result;
  }
  result.destination_valid = valid;
  if (!valid) result.outcome = kPresentationOutcome_OptionalOmitted;
  return result;
}

/* Softness is a radius in output pixels, scaled with the viewport so the look
 * is resolution-independent rather than shrinking as the window grows. */
static PresentationOutcome BlurSimShadowMask(
    ArRenderTexture mask, int w, int h, unsigned softness_pct,
    bool *mask_valid) {
  if (mask_valid) *mask_valid = true;
  if (!softness_pct) return kPresentationOutcome_Complete;
  float radius =
      (float)softness_pct / (float)kPercentScale * (float)h * 0.02f;
  if (radius < 0.5f) return kPresentationOutcome_Complete;
  /* D4b's separable blur alone needs the second full-viewport target. Keep a
   * hard-shadow run from reserving it, which is 31.6 MiB at 4K. Allocation
   * failure degrades softness without touching the primary mask. */
  if (!ArRenderTexture_IsValid(s_sim_shadow_scratch) &&
      !s_sim_shadow_scratch_alloc_failed) {
    s_sim_shadow_scratch = CreateSimShadowTarget(w, h);
    s_sim_shadow_scratch_alloc_failed =
        !ArRenderTexture_IsValid(s_sim_shadow_scratch);
  }
  if (!ArRenderTexture_IsValid(s_sim_shadow_scratch))
    return kPresentationOutcome_OptionalOmitted;
  const SimShadowAxisResult horizontal = BlurSimShadowAxis(
      mask, s_sim_shadow_scratch, w, h, radius, true);
  if (!PresentationOutcome_IsUsable(horizontal.outcome))
    return horizontal.outcome;
  if (!horizontal.destination_valid) {
    return kPresentationOutcome_OptionalOmitted;
  }
  const SimShadowAxisResult vertical = BlurSimShadowAxis(
      s_sim_shadow_scratch, mask, w, h, radius, false);
  if (!vertical.destination_valid && mask_valid) *mask_valid = false;
  return PresentationOutcome_Combine(horizontal.outcome, vertical.outcome);
}

/* Where an object is actually DRAWN, in authentic map pixels.
 *
 * A presentation stage may hold art away from the record's own cell for part
 * of its life (the eruption fountain launches a fireball from the crater and
 * converges it home), and every consumer that asks "where is this" has to get
 * the same answer -- above all the depth sort. Sorting on the record's
 * position while drawing somewhere else puts the art in front of geometry it
 * is visually behind, and the error grows with the offset. */
void SimObjectDrawnWorld(const SimRenderObject *object,
                                int *world_x, int *world_y) {
  bool foot_anchor = object->tier == kSimRecordTier_World &&
      !(object->traits & kSimObjectTrait_RecordOriginAnchor);
  *world_x = (foot_anchor ? object->foot_x : object->world_x) +
      object->offset_x;
  *world_y = (foot_anchor ? object->foot_y : object->world_y) +
      object->offset_y;
}

/* Silhouettes are accumulated into a transparent mask and composited once, so
 * overlapping casters cannot double-darken the ground and the darkened result
 * can never touch sky, dialogs, HUD, or settings. */
PresentationOutcome DrawSimShadowMask(
    const FrameSlot *slot, bool virtual_height, bool soft_shadows,
    bool terrain_depth_receiver, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16]) {
  if (!ArRenderTexture_IsValid(g_sim_obj_atlas_texture) ||
      !slot->sim.atlas_valid)
    return kPresentationOutcome_Complete;
  if (!slot->sim.shadow_opacity_pct)
    return kPresentationOutcome_Complete;
  bool any_caster = false;
  for (size_t i = 0; i < slot->sim.object_count; i++) {
    if (Sim3D_ObjectCastsShadow(&slot->sim.objects[i])) {
      any_caster = true;
      break;
    }
  }
  bool voxel_caster = slot->sim.background_voxel_enabled &&
      SimBackgroundVoxelRenderer_Ready(slot->sim.background_voxel_serial);
  /* An empty mask contributes nothing. Avoid a full-viewport target clear,
   * optional fourteen-draw blur and full-viewport composite on such frames. */
  if (!any_caster && !voxel_caster)
    return kPresentationOutcome_Complete;
  int shadow_w, shadow_h;
  Scene3D_CappedTargetSize(
      viewport.w, viewport.h, kSimShadowMaxTargetPixels,
      &shadow_w, &shadow_h);
  const ArRenderTexture mask = EnsureSimShadowTexture(shadow_w, shadow_h);
  if (!ArRenderTexture_IsValid(mask))
    return kPresentationOutcome_OptionalOmitted;

  SDL_Rect local_viewport = { 0, 0, shadow_w, shadow_h };
  float unit_x = ((float)shadow_w / (float)shadow_h) / (float)source.w;
  float unit_y = 1.0f / (float)source.h;
  float light_x, light_y;
  SimShadowLight(slot, &light_x, &light_y);

  ArRenderTargetState target_state = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      &g_render_device, mask, &target_state);
  if (begin == kArRenderTargetBegin_StateLost)
    return kPresentationOutcome_CoreFailure;
  if (begin != kArRenderTargetBegin_Ready)
    return kPresentationOutcome_OptionalOmitted;
  bool mask_valid = ArRenderDevice_Clear(
      &g_render_device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});

  int vertex_count = 0;
  int index_count = 0;
  for (size_t i = 0; i < slot->sim.object_count; i++) {
    const SimRenderObject *object = &slot->sim.objects[i];
    if (object->hidden || !Sim3D_ObjectCastsShadow(object)) continue;

    /* foot_dx/foot_dy are needed below for the silhouette's own local
     * coordinates, so this keeps the split form rather than collapsing to
     * SimObjectDrawnWorld -- but it applies the same presentation offset. */
    bool foot_anchor = !(object->traits & kSimObjectTrait_RecordOriginAnchor);
    int foot_dx = foot_anchor ? object->foot_x - (int)object->world_x : 0;
    int foot_dy = foot_anchor ? object->foot_y - (int)object->world_y : 0;
    int screen_anchor_x = (int16_t)(uint16_t)(
        object->world_x + object->offset_x + foot_dx - slot->sim.camera_x);
    int screen_anchor_y = (int16_t)(uint16_t)(
        object->world_y + object->offset_y + foot_dy - slot->sim.camera_y);
    float texture_anchor_x = slot->ws_extra + screen_anchor_x;
    float texture_anchor_y = screen_anchor_y;
    float anchor_world_x =
        ((texture_anchor_x - source.x) / source.w - 0.5f) *
        ((float)viewport.w / (float)viewport.h);
    float anchor_world_y =
        0.5f - (texture_anchor_y - source.y) / source.h;
    const float local_ground_world = SimTerrainGroundHeightWorld(
        slot, source, object->world_x + object->offset_x + foot_dx,
        object->world_y + object->offset_y + foot_dy);
    const float caster_base_world = SimObjectAltitudeBaseWorld(
        slot, object, source,
        object->world_x + object->offset_x + foot_dx,
        object->world_y + object->offset_y + foot_dy);
    float height_world = caster_base_world - local_ground_world;
    if (virtual_height)
      height_world += SimHeightWorldUnits(
          source, object->virtual_height, slot->sim.height_scale_x100);
    if (height_world < 0.0f) height_world = 0.0f;

    /* The silhouette is laid flat on the ground about the caster's foot: art
     * to the left stays left, art above the foot extends away from the camera.
     * The whole quad then shears along the light by the caster's height, so a
     * grounded actor is shadowed under its own feet and a flying one throws
     * its shadow clear of itself. */
    float offset_x[2] = {
      (float)(object->local_x0 - foot_dx),
      (float)(object->local_x1 - foot_dx),
    };
    float offset_y[2] = {
      (float)(object->local_y0 - foot_dy),
      (float)(object->local_y1 - foot_dy),
    };
    /* Height shrinks the footprint about the caster's ground point, so the
     * classified height and the player's height-scale tuning both feed the
     * shadow's size as well as its offset. */
    float footprint = Scene3D_ShadowFootprintScale(height_world,
                                                   kSimShadowHeightShrink);
    Scene3DPoint corner[4];
    bool shadow_visible = true;
    const float map_anchor_x =
        object->world_x + object->offset_x + foot_dx;
    const float map_anchor_y =
        object->world_y + object->offset_y + foot_dy;
    for (int c = 0; c < 4; c++) {
      const float footprint_x = offset_x[c & 1] * footprint;
      const float footprint_y = offset_y[c >> 1] *
          kSimShadowFootprintDepth * footprint;
      const float ground_world = SimTerrainGroundHeightWorld(
          slot, source, map_anchor_x + footprint_x,
          map_anchor_y + footprint_y);
      if (!Scene3D_ProjectWorldPoint(
              matrix,
              anchor_world_x + footprint_x * unit_x +
                  height_world * light_x,
              anchor_world_y - footprint_y * unit_y +
                  height_world * light_y,
              ground_world, local_viewport.w, local_viewport.h, &corner[c])) {
        shadow_visible = false;
        break;
      }
    }
    if (!shadow_visible) continue;

    float u0 = object->atlas_x / (float)kSimObjAtlasWidth;
    float v0 = object->atlas_y / (float)kSimObjAtlasHeight;
    float u1 = (object->atlas_x + object->atlas_w) /
        (float)kSimObjAtlasWidth;
    float v1 = (object->atlas_y + object->atlas_h) /
        (float)kSimObjAtlasHeight;
    const ArRenderColorF black = { 0.0f, 0.0f, 0.0f, 1.0f };
    const ArRenderVertex2D vertices[kSimShadowVerticesPerCaster] = {
      {{corner[0].x, corner[0].y}, black, {u0, v0}},
      {{corner[1].x, corner[1].y}, black, {u1, v0}},
      {{corner[2].x, corner[2].y}, black, {u0, v1}},
      {{corner[3].x, corner[3].y}, black, {u1, v1}},
    };
    const int32_t base = vertex_count;
    memcpy(&s_sim_shadow_vertices[vertex_count], vertices, sizeof(vertices));
    vertex_count += kSimShadowVerticesPerCaster;
    const int32_t indices[kSimShadowIndicesPerCaster] = {
      base, base + 1, base + 3, base, base + 3, base + 2,
    };
    memcpy(&s_sim_shadow_indices[index_count], indices, sizeof(indices));
    index_count += kSimShadowIndicesPerCaster;
  }

  if (mask_valid && index_count) {
    const ArRenderDrawState caster_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Alpha,
    };
    mask_valid = ArRenderDevice_DrawGeometryWithState(
        &g_render_device, g_sim_obj_atlas_texture,
        s_sim_shadow_vertices, vertex_count,
        s_sim_shadow_indices, index_count, &caster_state);
    if (mask_valid) {
      Sim3DPerformance_AddDraw(
          (uint64_t)vertex_count, (uint64_t)index_count);
    }
  }

  if (mask_valid && voxel_caster) {
    SimBackgroundVoxelRenderParams voxel_params =
        SimVoxelRenderParams(slot, source, local_viewport, matrix);
    SimBackgroundVoxelRenderer_DrawShadowMask(
        &g_render_device, &voxel_params, light_x, light_y);
  }

  PresentationOutcome outcome = mask_valid
      ? kPresentationOutcome_Complete
      : kPresentationOutcome_OptionalOmitted;
  if (mask_valid && soft_shadows) {
    const PresentationOutcome blur = BlurSimShadowMask(
        mask, shadow_w, shadow_h, slot->sim.shadow_softness_pct,
        &mask_valid);
    outcome = PresentationOutcome_Combine(outcome, blur);
  }

  const bool target_restored = ArRenderDevice_EndTarget(
      &g_render_device, &target_state);
  if (!target_restored || !PresentationOutcome_IsUsable(outcome))
    return kPresentationOutcome_CoreFailure;
  if (!mask_valid) return kPresentationOutcome_OptionalOmitted;

  /* In an elevated voxel town this texture is consumed later by the same GPU
   * pass that owns terrain/model depth. Leaving it uncomposited here prevents
   * a screen-space quad from painting straight through a cliff face. */
  if (terrain_depth_receiver) {
    SimBackgroundVoxelRenderParams voxel_params =
        SimVoxelRenderParams(slot, source, viewport, matrix);
    voxel_params.shadow_mask = mask;
    SimBackgroundVoxelRenderer_DrawTerrainShadow(
        &g_render_device, &voxel_params);
    return outcome;
  }

  const uint8_t mask_alpha = (uint8_t)(
      slot->sim.shadow_opacity_pct * 255 / kPercentScale);
  const ArRenderDrawState composite_state = {
    .flags = kArRenderDrawState_Tint | kArRenderDrawState_Blend,
    .tint = {1.0f, 1.0f, 1.0f, (float)mask_alpha / 255.0f},
    .blend = kArRenderBlendMode_Alpha,
  };
  const ArRenderRectF destination = {
    viewport.x, viewport.y, viewport.w, viewport.h,
  };
  const bool composited = ArRenderDevice_DrawTextureWithState(
      &g_render_device, mask, NULL, &destination, &composite_state);
  if (composited)
    Sim3DPerformance_AddDraw(0, 0);
  if (!composited)
    outcome = PresentationOutcome_Combine(
        outcome, kPresentationOutcome_OptionalOmitted);
  return outcome;
}

void PresentSim3DShadows_ResetResources(void) {
  SimShadowEffectBackend_Reset(&g_render_device);
  ArRenderDevice_DestroyTexture(&g_render_device, s_sim_shadow_texture);
  s_sim_shadow_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, s_sim_shadow_scratch);
  s_sim_shadow_scratch = ArRenderTexture_Invalid();
  s_sim_shadow_w = s_sim_shadow_h = 0;
  s_sim_shadow_unavailable = false;
  s_sim_shadow_scratch_alloc_failed = false;
}
