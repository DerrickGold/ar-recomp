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
#include "crt_post.h"
#include "gpu_shader_blob.h"
#include "present_internal.h"
#include "present_sim3d_internal.h"
#include "present_sim3d_project.h"
#include "present_sim3d_shadows.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim3d.h"
#include "sim/sim3d_performance.h"
#include "shaders/sim_shadow_blur_frag.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_sim_obj_atlas_texture;

/* The ordinary D4 mask remains a screen-space target so overlapping casters
 * accumulate once and soft blur stays inexpensive. Elevated towns defer its
 * composite to the shared depth pass, where it is sampled only by visible
 * terrain-top receiver geometry. */
static SDL_Texture *s_sim_shadow_texture;
static SDL_Texture *s_sim_shadow_scratch;
static int s_sim_shadow_w, s_sim_shadow_h;
static bool s_sim_shadow_alloc_failed;
static bool s_sim_shadow_scratch_alloc_failed;

typedef struct SimShadowBlurUniforms {
  float texel_x, texel_y;
  float radius;
  float pad0;
} SimShadowBlurUniforms;

static const GpuShaderBlobs kSimShadowBlurBlobs = {
  kSimShadowBlurFragMSL, kSimShadowBlurFragMSLSize,
  kSimShadowBlurFragSPV, kSimShadowBlurFragSPVSize,
  kSimShadowBlurFragDXIL, kSimShadowBlurFragDXILSize,
};
static SDL_GPUDevice *s_sim_shadow_blur_device;
static SDL_GPUShader *s_sim_shadow_blur_shader;
static SDL_GPURenderState *s_sim_shadow_blur_state;
static bool s_sim_shadow_blur_attempted;

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
static SDL_Vertex s_sim_shadow_vertices[kSimShadowMaxVertices];
static int s_sim_shadow_indices[kSimShadowMaxIndices];

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

SDL_Texture *CreateSimShadowTarget(int w, int h) {
  SDL_Texture *texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  }
  return texture;
}

static SDL_Texture *EnsureSimShadowTexture(int w, int h) {
  if (!g_renderer || w <= 0 || h <= 0) return NULL;
  if (s_sim_shadow_texture && s_sim_shadow_w == w && s_sim_shadow_h == h)
    return s_sim_shadow_texture;
  if (s_sim_shadow_texture) SDL_DestroyTexture(s_sim_shadow_texture);
  if (s_sim_shadow_scratch) SDL_DestroyTexture(s_sim_shadow_scratch);
  s_sim_shadow_scratch = NULL;
  s_sim_shadow_scratch_alloc_failed = false;
  s_sim_shadow_texture = CreateSimShadowTarget(w, h);
  s_sim_shadow_w = w;
  s_sim_shadow_h = h;
  if (!s_sim_shadow_texture && !s_sim_shadow_alloc_failed) {
    /* Geometry must survive a target-allocation failure; only the shadow
     * stage drops out. Logged once so it cannot flood a play session. */
    s_sim_shadow_alloc_failed = true;
    fprintf(stderr, "[sim3d-d4] shadow mask target unavailable: %s\n",
            SDL_GetError());
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

static bool EnsureSimShadowBlurShader(void) {
  if (s_sim_shadow_blur_attempted)
    return s_sim_shadow_blur_state != NULL;
  s_sim_shadow_blur_attempted = true;

  SDL_PropertiesID props = SDL_GetRendererProperties(g_renderer);
  SDL_GPUDevice *device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!device) return false;
  s_sim_shadow_blur_device = device;
  s_sim_shadow_blur_shader = GpuShaderBlob_CreateFragment(
      device, &kSimShadowBlurBlobs, "SIM shadow blur", 1, 1);
  if (!s_sim_shadow_blur_shader) return false;

  SDL_GPURenderStateCreateInfo state_info;
  SDL_zero(state_info);
  state_info.fragment_shader = s_sim_shadow_blur_shader;
  s_sim_shadow_blur_state = SDL_CreateGPURenderState(
      g_renderer, &state_info);
  if (!s_sim_shadow_blur_state) {
    fprintf(stderr, "[sim3d-d4] shadow blur render state failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUShader(device, s_sim_shadow_blur_shader);
    s_sim_shadow_blur_shader = NULL;
    return false;
  }
  return true;
}

static SDL_BlendMode SimShadowAccumulateBlend(void) {
  static SDL_BlendMode mode = SDL_BLENDMODE_INVALID;
  if (mode == SDL_BLENDMODE_INVALID)
    mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
  return mode;
}

static void BlurSimShadowAxis(SDL_Texture *source, SDL_Texture *destination,
                              int w, int h, float radius, bool horizontal) {
  SDL_SetRenderTarget(g_renderer, destination);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);

  if (EnsureSimShadowBlurShader()) {
    SimShadowBlurUniforms uniforms = {
      horizontal ? 1.0f / (float)w : 0.0f,
      horizontal ? 0.0f : 1.0f / (float)h,
      radius,
      0.0f,
    };
    SDL_SetTextureAlphaMod(source, 255);
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    const bool bound = SDL_SetGPURenderStateFragmentUniforms(
            s_sim_shadow_blur_state, 0, &uniforms,
            (Uint32)sizeof(uniforms)) &&
        SDL_SetGPURenderState(g_renderer, s_sim_shadow_blur_state);
    if (bound) {
      /* Once submitted, do not run the fallback on an SDL draw error: a
       * backend may already have recorded the draw, and repeating it would
       * double the shadow. The next axis/frame can still continue safely. */
      if (SDL_RenderTexture(g_renderer, source, NULL, NULL))
        Sim3DPerformance_AddDraw(0, 0);
      SDL_SetGPURenderState(g_renderer, NULL);
      SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
      return;
    }
    SDL_SetGPURenderState(g_renderer, NULL);
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
  }

  SDL_BlendMode accumulate = SimShadowAccumulateBlend();
  if (accumulate == SDL_BLENDMODE_INVALID) {
    /* Without the custom blend the taps would composite instead of average,
     * which reads as a smeared double image rather than a soft edge. Copy the
     * mask through unchanged and leave the shadow hard. */
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    if (SDL_RenderTexture(g_renderer, source, NULL, NULL))
      Sim3DPerformance_AddDraw(0, 0);
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
    return;
  }
  SDL_SetTextureBlendMode(source, accumulate);
  int half = kSimShadowBlurTaps / 2;
  for (int tap = -half; tap <= half; tap++) {
    float offset = radius * (float)tap / (float)half;
    SDL_FRect destination_rect = {
      horizontal ? offset : 0.0f, horizontal ? 0.0f : offset,
      (float)w, (float)h,
    };
    SDL_SetTextureAlphaMod(source, (Uint8)(255 / kSimShadowBlurTaps));
    if (SDL_RenderTexture(g_renderer, source, NULL, &destination_rect))
      Sim3DPerformance_AddDraw(0, 0);
  }
  SDL_SetTextureAlphaMod(source, 255);
  SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
}

/* Softness is a radius in output pixels, scaled with the viewport so the look
 * is resolution-independent rather than shrinking as the window grows. */
static void BlurSimShadowMask(SDL_Texture *mask, int w, int h,
                              unsigned softness_pct) {
  if (!softness_pct) return;
  float radius =
      (float)softness_pct / (float)kPercentScale * (float)h * 0.02f;
  if (radius < 0.5f) return;
  /* D4b's separable blur alone needs the second full-viewport target. Keep a
   * hard-shadow run from reserving it, which is 31.6 MiB at 4K. Allocation
   * failure degrades softness without touching the primary mask. */
  if (!s_sim_shadow_scratch && !s_sim_shadow_scratch_alloc_failed) {
    s_sim_shadow_scratch = CreateSimShadowTarget(w, h);
    s_sim_shadow_scratch_alloc_failed = !s_sim_shadow_scratch;
  }
  if (!s_sim_shadow_scratch) return;
  BlurSimShadowAxis(mask, s_sim_shadow_scratch, w, h, radius, true);
  BlurSimShadowAxis(s_sim_shadow_scratch, mask, w, h, radius, false);
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
void DrawSimShadowMask(
    const FrameSlot *slot, bool virtual_height, bool soft_shadows,
    bool terrain_depth_receiver, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16]) {
  if (!g_sim_obj_atlas_texture || !slot->sim.atlas_valid) return;
  if (!slot->sim.shadow_opacity_pct) return;
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
  if (!any_caster && !voxel_caster) return;
  int shadow_w, shadow_h;
  Scene3D_CappedTargetSize(
      viewport.w, viewport.h, kSimShadowMaxTargetPixels,
      &shadow_w, &shadow_h);
  SDL_Texture *mask = EnsureSimShadowTexture(shadow_w, shadow_h);
  if (!mask) return;

  SDL_Rect local_viewport = { 0, 0, shadow_w, shadow_h };
  float unit_x = ((float)shadow_w / (float)shadow_h) / (float)source.w;
  float unit_y = 1.0f / (float)source.h;
  float light_x, light_y;
  SimShadowLight(slot, &light_x, &light_y);

  /* The clip rect belongs to the current target, and the caller may have set
   * one on the output before this runs; the mask must be built unclipped, and
   * the composite below then re-clips it. */
  SDL_Rect saved_clip;
  bool clipped = SDL_RenderClipEnabled(g_renderer);
  if (clipped) SDL_GetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetRenderTarget(g_renderer, mask);
  SDL_SetRenderClipRect(g_renderer, NULL);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);
  SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureAlphaMod(g_sim_obj_atlas_texture, 255);

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
    const SDL_FColor black = { 0.0f, 0.0f, 0.0f, 1.0f };
    SDL_Vertex vertices[kSimShadowVerticesPerCaster] = {
      {{corner[0].x, corner[0].y}, black, {u0, v0}},
      {{corner[1].x, corner[1].y}, black, {u1, v0}},
      {{corner[2].x, corner[2].y}, black, {u0, v1}},
      {{corner[3].x, corner[3].y}, black, {u1, v1}},
    };
    const int base = vertex_count;
    memcpy(&s_sim_shadow_vertices[vertex_count], vertices, sizeof(vertices));
    vertex_count += kSimShadowVerticesPerCaster;
    const int indices[kSimShadowIndicesPerCaster] = {
      base, base + 1, base + 3, base, base + 3, base + 2,
    };
    memcpy(&s_sim_shadow_indices[index_count], indices, sizeof(indices));
    index_count += kSimShadowIndicesPerCaster;
  }

  if (index_count) {
    if (SDL_RenderGeometry(g_renderer, g_sim_obj_atlas_texture,
                           s_sim_shadow_vertices, vertex_count,
                           s_sim_shadow_indices, index_count)) {
      Sim3DPerformance_AddDraw(
          (uint64_t)vertex_count, (uint64_t)index_count);
    }
  }

  if (voxel_caster) {
    SimBackgroundVoxelRenderParams voxel_params =
        SimVoxelRenderParams(slot, source, local_viewport, matrix);
    SimBackgroundVoxelRenderer_DrawShadowMask(
        g_renderer, &voxel_params, light_x, light_y);
  }

  if (soft_shadows)
    BlurSimShadowMask(mask, shadow_w, shadow_h,
                      slot->sim.shadow_softness_pct);

  SDL_SetRenderTarget(g_renderer, CrtPost_BaseTarget());
  if (clipped) SDL_SetRenderClipRect(g_renderer, &saved_clip);

  /* In an elevated voxel town this texture is consumed later by the same GPU
   * pass that owns terrain/model depth. Leaving it uncomposited here prevents
   * a screen-space quad from painting straight through a cliff face. */
  if (terrain_depth_receiver) {
    SDL_SetTextureAlphaMod(mask, 255);
    SimBackgroundVoxelRenderParams voxel_params =
        SimVoxelRenderParams(slot, source, viewport, matrix);
    voxel_params.shadow_mask = mask;
    SimBackgroundVoxelRenderer_DrawTerrainShadow(
        g_renderer, &voxel_params);
    return;
  }

  SDL_SetTextureAlphaMod(
      mask, (Uint8)(slot->sim.shadow_opacity_pct * 255 / kPercentScale));
  SDL_FRect dst = ToFRect(viewport);
  if (SDL_RenderTexture(g_renderer, mask, NULL, &dst))
    Sim3DPerformance_AddDraw(0, 0);
  SDL_SetTextureAlphaMod(mask, 255);
}

void PresentSim3DShadows_ResetResources(void) {
  if (g_renderer) SDL_SetGPURenderState(g_renderer, NULL);
  SDL_GPUDevice *current_device = NULL;
  if (g_renderer) {
    current_device = (SDL_GPUDevice *)SDL_GetPointerProperty(
        SDL_GetRendererProperties(g_renderer),
        SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  }
  const bool same_shadow_device = !s_sim_shadow_blur_device ||
      current_device == s_sim_shadow_blur_device;
  if (same_shadow_device && s_sim_shadow_blur_state)
    SDL_DestroyGPURenderState(s_sim_shadow_blur_state);
  if (same_shadow_device && current_device && s_sim_shadow_blur_shader)
    SDL_ReleaseGPUShader(current_device, s_sim_shadow_blur_shader);
  s_sim_shadow_blur_state = NULL;
  s_sim_shadow_blur_shader = NULL;
  s_sim_shadow_blur_device = NULL;
  s_sim_shadow_blur_attempted = false;
  if (s_sim_shadow_texture) SDL_DestroyTexture(s_sim_shadow_texture);
  s_sim_shadow_texture = NULL;
  if (s_sim_shadow_scratch) SDL_DestroyTexture(s_sim_shadow_scratch);
  s_sim_shadow_scratch = NULL;
  s_sim_shadow_w = s_sim_shadow_h = 0;
  s_sim_shadow_alloc_failed = false;
  s_sim_shadow_scratch_alloc_failed = false;
}
