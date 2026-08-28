/* SIM 3D effect rendering: the per-effect style table, the glow/trail/particle
 * batches, local lighting and scene flash, the volcanic fireball heads, and
 * the promoted map-plane objects. Split out of present_sim3d.c; the
 * definitions are unchanged.
 *
 * The largest single stage in the SIM present path and the one with the most
 * authored policy in it -- one style row per effect family -- which is why it
 * is a unit of its own rather than part of the composite. */

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deterministic_hash.h"
#include "present_internal.h"
#include "present_sim3d_effects.h"
#include "present_sim3d_project.h"
#include "platform/sdl/render_sdl.h"
#include "render/render_device.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim3d.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

extern SDL_Renderer *g_renderer;
extern ArRenderTexture g_sim_obj_atlas_texture;

static SDL_Texture *NativeAtlasTexture(void) {
  return ArSdlRenderBackend_UnwrapTexture(g_sim_obj_atlas_texture);
}

enum {
  kSimMaxParticlesPerEffect = 12,
  /* A trail spends its budget along the retained path instead of around one
   * point, so it is sized separately and the batch is cut to whichever of the
   * two is larger. */
  kSimMaxTrailPuffsPerSample = 2,
  kSimMaxTrailPuffsPerEffect =
      kSimEffectTrailSamples * kSimMaxTrailPuffsPerSample,
  kSimMaxEffectQuadsPerEffect =
      kSimMaxParticlesPerEffect > kSimMaxTrailPuffsPerEffect
          ? kSimMaxParticlesPerEffect : kSimMaxTrailPuffsPerEffect,
};

typedef enum SimEffectParticleMotion {
  kSimEffectParticle_Burst,
  kSimEffectParticle_Flame,
  /* Embers that stream off a moving body rather than rising from a fixed
   * one. Placed along the effect's own retained path, so the tail follows the
   * authentic trajectory instead of a guessed heading. */
  kSimEffectParticle_Trail,
} SimEffectParticleMotion;

typedef struct SimEffectStyle {
  float strength;
  SDL_FColor glow_center;
  SDL_FColor glow_edge;
  SDL_FColor particle_color;
  SDL_FColor flash_color;
  float glow_radius_x;
  float glow_radius_y;
  /* Screen-upright visual offset from the semantic contact point. The
   * metadata keeps the exact gameplay/art attachment while tall sprites can
   * center their light within the visible body. */
  float glow_origin_lift;
  /* Screen-upright distance, in authentic billboard pixels, from the
   * semantic attachment/contact point to the particle source. Lighting keeps
   * its own independently styled origin. Keeping the two roles separate
   * prevents a grounded semantic contact from forcing embers to emerge
   * underneath tall source art. */
  float particle_origin_lift;
  uint8_t particle_count;
  uint8_t particle_motion;
  bool scene_flash;
  /* Trail styling. Ignored unless particle_motion is kSimEffectParticle_Trail.
   * The tail fades from flame to smoke along the retained path: samples up to
   * trail_flame_samples old keep particle_color, older ones cross into
   * trail_smoke_color, and every puff grows as it cools. */
  SDL_FColor trail_smoke_color;
  float trail_flame_samples;
  float trail_puff_radius;
  float trail_spread;
  uint8_t trail_puffs_per_sample;
} SimEffectStyle;

static SimEffectStyle SimLightningBaseStyle(float strength,
                                            float radius_x,
                                            float radius_y) {
  return (SimEffectStyle){
    .strength = strength,
    .glow_center = { 0.48f, 0.72f, 1.0f, 0.46f },
    .glow_edge = { 0.20f, 0.42f, 1.0f, 0.0f },
    .particle_color = { 0.62f, 0.82f, 1.0f, 1.0f },
    .flash_color = { 0.38f, 0.59f, 1.0f, 1.0f },
    .glow_radius_x = radius_x,
    .glow_radius_y = radius_y,
    .particle_count = kSimMaxParticlesPerEffect,
    .particle_motion = kSimEffectParticle_Burst,
    .scene_flash = true,
  };
}

static bool SimEffectStyleFor(const SimEffectInstance *effect,
                              SimEffectStyle *style) {
  if (!effect || !style) return false;
  SimEffectPhase phase = (SimEffectPhase)effect->phase;
  switch ((SimEffectKind)effect->kind) {
    case kSimEffect_LightningMiracle: {
      float strength = 1.0f;
      if (phase == kSimEffectPhase_LightningLead)
        strength = 210.0f / 255.0f;
      else if (phase == kSimEffectPhase_LightningBranch)
        strength = 235.0f / 255.0f;
      *style = SimLightningBaseStyle(strength, 30.0f, 12.0f);
      return true;
    }
    case kSimEffect_BlueDragonLightning: {
      float strength = phase == kSimEffectPhase_BlueDragonBoltA ? 0.82f :
          phase == kSimEffectPhase_BlueDragonBoltB ? 0.92f : 1.0f;
      *style = SimLightningBaseStyle(strength, 24.0f, 11.0f);
      return true;
    }
    case kSimEffect_TownCreationLightning: {
      float strength = phase == kSimEffectPhase_TownCreationBoltA ? 0.82f :
          phase == kSimEffectPhase_TownCreationBoltB ? 0.90f :
          phase == kSimEffectPhase_TownCreationBoltC ? 0.96f : 1.0f;
      *style = SimLightningBaseStyle(strength, 27.0f, 12.0f);
      return true;
    }
    case kSimEffect_RedDemonFire: {
      float strength = phase == kSimEffectPhase_RedFireSmall ? 0.62f :
          phase == kSimEffectPhase_RedFireMedium ? 0.82f : 1.0f;
      *style = (SimEffectStyle){
        .strength = strength,
        .glow_center = { 1.0f, 0.46f, 0.08f, 0.38f },
        .glow_edge = { 1.0f, 0.12f, 0.01f, 0.0f },
        .particle_color = { 1.0f, 0.60f, 0.16f, 1.0f },
        .glow_radius_x = 18.0f,
        .glow_radius_y = 12.0f,
        .particle_count = 8,
        .particle_motion = kSimEffectParticle_Flame,
      };
      return true;
    }
    case kSimEffect_GroundFire: {
      float strength = phase == kSimEffectPhase_GroundFireA ? 0.78f :
          phase == kSimEffectPhase_GroundFireB ? 0.90f : 1.0f;
      switch ((SimEffectColorFamily)effect->color_family) {
        case kSimEffectColor_FireRed:
          *style = (SimEffectStyle){
            .strength = strength,
            .glow_center = { 1.0f, 0.40f, 0.04f, 0.46f },
            .glow_edge = { 1.0f, 0.09f, 0.0f, 0.0f },
            .particle_color = { 1.0f, 0.66f, 0.12f, 1.0f },
            .glow_radius_x = 21.0f,
            .glow_radius_y = 13.0f,
            .particle_count = 10,
            .particle_motion = kSimEffectParticle_Flame,
          };
          return true;
        case kSimEffectColor_FireBlue:
          *style = (SimEffectStyle){
            .strength = strength,
            .glow_center = { 0.24f, 0.68f, 1.0f, 0.58f },
            .glow_edge = { 0.04f, 0.20f, 1.0f, 0.0f },
            .particle_color = { 0.68f, 0.94f, 1.0f, 1.0f },
            .glow_radius_x = 24.0f,
            .glow_radius_y = 15.0f,
            .particle_count = kSimMaxParticlesPerEffect,
            .particle_motion = kSimEffectParticle_Flame,
          };
          return true;
        case kSimEffectColor_None:
        case kSimEffectColor_LightningBlue:
          return false;
      }
      return false;
    }
    /* One style for one animation. The eruption's ground fire is the burning
     * house's own three frames -- same tiles, same palette, same phase family
     * -- so it shares the ramp rather than restating it; the kinds stay
     * distinct only so a trace can tell burning ground from a burning house. */
    case kSimEffect_VolcanoGroundFire:
    case kSimEffect_HouseFire: {
      float strength = phase == kSimEffectPhase_HouseFireA ? 0.78f :
          phase == kSimEffectPhase_HouseFireB ? 0.90f : 1.0f;
      *style = (SimEffectStyle){
        .strength = strength,
        .glow_center = { 1.0f, 0.42f, 0.05f, 0.48f },
        .glow_edge = { 1.0f, 0.08f, 0.0f, 0.0f },
        .particle_color = { 1.0f, 0.68f, 0.14f, 1.0f },
        .glow_radius_x = 22.0f,
        .glow_radius_y = 14.0f,
        /* Run 20260803-134746 shows the youngest diamonds straddling the
         * bottom of this 16px sprite when both use ground contact (8,16).
         * The old capture also places the radial light below the visible
         * body, so center both presentation origins at local y=12 while the
         * semantic contact remains (8,16). */
        .glow_origin_lift = 4.0f,
        .particle_origin_lift = 4.0f,
        .particle_count = 10,
        .particle_motion = kSimEffectParticle_Flame,
      };
      return true;
    }
    case kSimEffect_VolcanoFireball: {
      /* Both authored frames are one blob of the same two tiles, so the two
       * phases differ only in how hot the head reads. */
      float strength =
          phase == kSimEffectPhase_VolcanoFireballA ? 0.88f : 1.0f;
      *style = (SimEffectStyle){
        .strength = strength,
        .glow_center = { 1.0f, 0.52f, 0.10f, 0.52f },
        .glow_edge = { 1.0f, 0.14f, 0.01f, 0.0f },
        .particle_color = { 1.0f, 0.74f, 0.22f, 1.0f },
        .glow_radius_x = 16.0f,
        .glow_radius_y = 16.0f,
        /* Trail motion spends its budget along the retained path, so the
         * per-point particle_count does not apply; trail_puffs_per_sample is
         * what sizes it. */
        .particle_motion = kSimEffectParticle_Trail,
        .trail_smoke_color = { 0.40f, 0.35f, 0.34f, 0.85f },
        /* Flame for the first few samples, smoke for the long tail behind
         * it. At kSimEffectTrailStride these three samples are about a
         * dozen builds -- long enough to read as a burning head, short
         * enough that the rest of the throw is a smoke path. */
        .trail_flame_samples = 3.0f,
        .trail_puff_radius = 3.0f,
        .trail_spread = 2.6f,
        .trail_puffs_per_sample = 2,
      };
      return true;
    }
    case kSimEffect_None:
      return false;
  }
  return false;
}

static bool EffectBatchReserve(EffectBatch *batch,
                               int vertices, int indices) {
  if (!batch || vertices < 0 || indices < 0 ||
      batch->vertex_count + vertices > batch->vertex_capacity ||
      batch->index_count + indices > batch->index_capacity) {
    if (batch) batch->overflow = true;
    return false;
  }
  return true;
}

static const float kEffectCircle32[32][2] = {
  { 1.0f, 0.0f }, { 0.980785f, 0.195090f },
  { 0.923880f, 0.382683f }, { 0.831470f, 0.555570f },
  { 0.707107f, 0.707107f }, { 0.555570f, 0.831470f },
  { 0.382683f, 0.923880f }, { 0.195090f, 0.980785f },
  { 0.0f, 1.0f }, { -0.195090f, 0.980785f },
  { -0.382683f, 0.923880f }, { -0.555570f, 0.831470f },
  { -0.707107f, 0.707107f }, { -0.831470f, 0.555570f },
  { -0.923880f, 0.382683f }, { -0.980785f, 0.195090f },
  { -1.0f, 0.0f }, { -0.980785f, -0.195090f },
  { -0.923880f, -0.382683f }, { -0.831470f, -0.555570f },
  { -0.707107f, -0.707107f }, { -0.555570f, -0.831470f },
  { -0.382683f, -0.923880f }, { -0.195090f, -0.980785f },
  { 0.0f, -1.0f }, { 0.195090f, -0.980785f },
  { 0.382683f, -0.923880f }, { 0.555570f, -0.831470f },
  { 0.707107f, -0.707107f }, { 0.831470f, -0.555570f },
  { 0.923880f, -0.382683f }, { 0.980785f, -0.195090f },
};

static bool AppendSimEffectGlow(
    EffectBatch *batch, const FrameSlot *slot,
    const SimEffectInstance *effect, const SimEffectStyle *style,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16]) {
  enum { kSegments = 32 };
  if (!EffectBatchReserve(batch, kSegments + 1, kSegments * 3))
    return false;
  Scene3DPoint strike;
  float scale_x, scale_y;
  if (!ProjectSimEffectPoint(
          slot, effect, &effect->geometry.data.point, source, viewport,
          camera, matrix, &strike, &scale_x, &scale_y))
    return true;
  strike.y -= style->glow_origin_lift * scale_y;

  int base_vertex = batch->vertex_count;
  SDL_FColor center = style->glow_center;
  center.a *= style->strength;
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    { strike.x, strike.y },
    center,
    { 0.0f, 0.0f },
  };
  float radius_x = style->glow_radius_x * scale_x;
  float radius_y = style->glow_radius_y * scale_y;
  if (radius_x < 3.0f) radius_x = 3.0f;
  if (radius_y < 2.0f) radius_y = 2.0f;
  for (int i = 0; i < kSegments; i++) {
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { strike.x + kEffectCircle32[i][0] * radius_x,
        strike.y + kEffectCircle32[i][1] * radius_y },
      style->glow_edge,
      { 0.0f, 0.0f },
    };
    batch->indices[batch->index_count++] = base_vertex;
    batch->indices[batch->index_count++] = base_vertex + i + 1;
    batch->indices[batch->index_count++] =
        base_vertex + (i + 1) % kSegments + 1;
  }
  return true;
}

static void AppendEffectQuad(EffectBatch *batch, float x, float y,
                             float size, SDL_FColor color) {
  int base_vertex = batch->vertex_count;
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    { x, y - size }, color, { 0.0f, 0.0f } };
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    { x + size, y }, color, { 0.0f, 0.0f } };
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    { x, y + size }, color, { 0.0f, 0.0f } };
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    { x - size, y }, color, { 0.0f, 0.0f } };
  static const int diamond[] = { 0, 1, 2, 0, 2, 3 };
  for (int n = 0; n < 6; n++)
    batch->indices[batch->index_count++] = base_vertex + diamond[n];
}

/* Flame and smoke streaming off a body that is actually moving.
 *
 * The tail is laid along the effect's own retained world path rather than
 * behind a heading derived from one frame: a lobbed fireball curves, and a
 * straight tail pinned to an instantaneous velocity separates from the arc at
 * exactly the moment the arc is most visible. Sample 0 is where the sprite is
 * now and is left alone -- the glow already sits there -- so the tail starts
 * one tick back and cools toward the oldest retained sample.
 *
 * Jitter is keyed on each sample's own world position, never on the array
 * index or the pulse clock. A sample slides one slot further down the array
 * every tick, so index-keyed noise would make the whole plume appear to crawl
 * forward through itself while the fireball flew. Keyed on the position, a
 * given puff of smoke keeps the offset it was born with and simply ages. */
static bool AppendSimEffectTrail(
    EffectBatch *batch, const FrameSlot *slot,
    const SimEffectInstance *effect, const SimEffectStyle *style,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16]) {
  if (effect->trail_count < 2) return true;
  unsigned puffs = style->trail_puffs_per_sample;
  if (puffs > kSimMaxTrailPuffsPerSample)
    puffs = kSimMaxTrailPuffsPerSample;
  if (!puffs) return true;

  unsigned samples = effect->trail_count;
  if (samples > kSimEffectTrailSamples) samples = kSimEffectTrailSamples;

  for (unsigned i = 1; i < samples; i++) {
    /* Project this sample by moving the instance's world origin onto it; the
     * local point, height and space all stay exactly as classified. */
    /* Each sample carries the altitude the emitter had when it was taken, so
     * a tail left by something in flight hangs in the air along the arc
     * instead of being painted onto the ground beneath it. */
    SimEffectLocalPoint local = effect->geometry.data.point;
    local.height = effect->trail[i].height;
    Scene3DPoint point;
    float scale_x, scale_y;
    if (!ProjectSimEffectPointAt(
            slot, effect, effect->trail[i].world_x, effect->trail[i].world_y,
            &local, source, viewport,
            camera, matrix, &point, &scale_x, &scale_y))
      continue;
    float output_scale = (fabsf(scale_x) + fabsf(scale_y)) * 0.5f;
    if (output_scale < 0.5f) output_scale = 0.5f;

    /* Age along the tail, 0 at the head, normalised against the CAPACITY
     * rather than against how much of it is filled. Absolute age is what a
     * smoke plume needs: with the live count as the denominator a given puff
     * would slide back toward 0 as the trail lengthened behind it, so the
     * whole plume brightened while the fireball flew. It also keeps the
     * oldest sample below 1, so a full trail still ends on something rather
     * than on a wasted transparent quad. */
    float t = (float)i / (float)kSimEffectTrailSamples;
    /* How far this sample has crossed from flame to smoke. */
    float cool = style->trail_flame_samples > 0.0f
        ? (float)i / style->trail_flame_samples : 1.0f;
    if (cool > 1.0f) cool = 1.0f;

    SDL_FColor color;
    color.r = style->particle_color.r +
        (style->trail_smoke_color.r - style->particle_color.r) * cool;
    color.g = style->particle_color.g +
        (style->trail_smoke_color.g - style->particle_color.g) * cool;
    color.b = style->particle_color.b +
        (style->trail_smoke_color.b - style->particle_color.b) * cool;
    color.a = style->particle_color.a +
        (style->trail_smoke_color.a - style->particle_color.a) * cool;
    /* Held, then dropped. A linear fade over a path this long makes the
     * middle of the trajectory -- the part that shows where the fireball
     * actually went -- the faintest thing on screen; 1 - t^2 keeps the first
     * half close to full and spends the fade at the far end where the smoke
     * is meant to be dissipating anyway. */
    color.a *= (1.0f - t * t) * style->strength;

    for (unsigned p = 0; p < puffs; p++) {
      if (!EffectBatchReserve(batch, 4, 6)) return false;
      uint32_t seed = DeterministicHash_Mix32(
          (uint32_t)effect->trail[i].world_x * 0x9E3779B9u ^
          (uint32_t)effect->trail[i].world_y * 0x85EBCA6Bu ^
          effect->generation * 0xC2B2AE35u ^ p * 0x27D4EB2Fu);
      const float *direction = kEffectCircle32[(seed >> 16) & 31u];
      /* Smoke swells and drifts outward as it cools; the flame end stays
       * tight against the path. */
      /* Smoke billows as it cools, and over a full throw's worth of path it
       * has time to: the tail spreads several times as wide as the flame end
       * rather than the little under twice a short tail managed. */
      float spread = style->trail_spread * (0.25f + 3.2f * t) * output_scale;
      float x = point.x + direction[0] * spread;
      float y = point.y + direction[1] * spread * 0.5f -
          /* Screen-upright buoyancy, so the plume lifts off the arc instead
           * of tracing it exactly. */
          (1.5f + 5.0f * t) * output_scale;
      float size = style->trail_puff_radius * (0.5f + 2.8f * t) *
          output_scale;
      if (size < 0.75f) size = 0.75f;
      AppendEffectQuad(batch, x, y, size, color);
    }
  }
  return true;
}

static bool AppendSimEffectParticles(
    EffectBatch *batch, const FrameSlot *slot,
    const SimEffectInstance *effect, const SimEffectStyle *style,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16]) {
  if (!effect->pulse_generation || effect->ticks_since_visible > 5) return true;
  if (style->particle_motion == kSimEffectParticle_Trail)
    return AppendSimEffectTrail(batch, slot, effect, style, source, viewport,
                                camera, matrix);
  Scene3DPoint strike;
  float scale_x, scale_y;
  if (!ProjectSimEffectPoint(
          slot, effect, &effect->geometry.data.point, source, viewport,
          camera, matrix, &strike, &scale_x, &scale_y))
    return true;
  strike.y -= style->particle_origin_lift * scale_y;
  float output_scale = (fabsf(scale_x) + fabsf(scale_y)) * 0.5f;
  if (output_scale < 0.5f) output_scale = 0.5f;

  for (uint32_t i = 0; i < style->particle_count; i++) {
    uint32_t seed = DeterministicHash_Mix32(
        (uint32_t)effect->record_address * 0x9E3779B9u ^
        effect->generation * 0x85EBCA6Bu ^
        effect->pulse_generation * 0xC2B2AE35u ^
        (uint32_t)effect->kind * 0x165667B1u ^ i * 0x27D4EB2Fu);
    bool flame = style->particle_motion == kSimEffectParticle_Flame;
    unsigned lifetime = flame
        ? 24u + ((seed >> 4) & 15u)
        : 6u + (seed & 3u);
    unsigned age;
    if (flame) {
      /* A flame is already populated when its authentic sprite appears. Seed
       * each ember at a different point in a slower 0.4-0.65 second cycle;
       * the old 6-9 tick loop made every fire look frantic and repeatedly
       * emptied the emitter during its short visible animation. */
      age = (effect->pulse_ticks + seed % lifetime) % lifetime;
    } else {
      unsigned spawn_delay = i / 4u;
      if (effect->pulse_ticks < spawn_delay) continue;
      age = effect->pulse_ticks - spawn_delay;
      if (age >= lifetime) continue;
    }
    if (!EffectBatchReserve(batch, 4, 6)) return false;

    float t = lifetime > 1 ? (float)age / (float)(lifetime - 1) : 1.0f;
    const float *direction = kEffectCircle32[(seed >> 16) & 31u];
    float x, y, size;
    if (flame) {
      float lateral = direction[0] * (2.0f + 6.0f * t) * output_scale;
      x = strike.x + lateral;
      y = strike.y - (2.0f + 19.0f * t) * output_scale;
      size = (2.20f - 1.20f * t) * output_scale;
    } else {
      float distance = (4.0f + 22.0f * t) * output_scale;
      float arc = 4.0f * t * (1.0f - t);
      x = strike.x + direction[0] * distance;
      y = strike.y + direction[1] * distance * 0.42f -
          arc * 12.0f * output_scale;
      size = (1.8f - 1.15f * t) * output_scale;
    }
    if (size < 0.75f) size = 0.75f;
    SDL_FColor color = style->particle_color;
    color.a *= (1.0f - t) * style->strength;
    AppendEffectQuad(batch, x, y, size, color);
  }
  return true;
}

void DrawSimEffectLocalLighting(
    const FrameSlot *slot, bool lighting, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16]) {
  if (!lighting || !slot->sim.effect_visible_count ||
      !EffectRendererAvailable())
    return;
  enum {
    kVertices = kSimMaxEffectInstances * 33,
    kIndices = kSimMaxEffectInstances * 32 * 3,
  };
  SDL_Vertex vertices[kVertices];
  int indices[kIndices];
  EffectBatch batch = {
    .vertices = vertices, .indices = indices,
    .vertex_capacity = kVertices, .index_capacity = kIndices,
  };
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    const SimEffectInstance *effect = &slot->sim.effects[i];
    SimEffectStyle style;
    if (!(effect->flags & kSimEffectFlag_Visible) ||
        !SimEffectStyleFor(effect, &style))
      continue;
    if (!AppendSimEffectGlow(&batch, slot, effect, &style, source,
                             viewport, camera, matrix))
      break;
  }
  if (!batch.index_count && !batch.overflow) return;
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  SubmitEffectBatch(&batch);
  EndEffectBlend(&state);
}

void DrawSimEffectSceneFlash(const FrameSlot *slot, bool lighting,
                                    SDL_Rect viewport) {
  if (!lighting || !slot->sim.effect_visible_count ||
      !EffectRendererAvailable())
    return;
  SimEffectStyle strongest_style = {0};
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    SimEffectStyle style;
    if ((slot->sim.effects[i].flags & kSimEffectFlag_Visible) &&
        SimEffectStyleFor(&slot->sim.effects[i], &style) &&
        style.scene_flash && style.strength > strongest_style.strength)
      strongest_style = style;
  }
  if (strongest_style.strength <= 0.0f) return;
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  bool color_ok = SDL_SetRenderDrawColor(
      g_renderer,
      (Uint8)(strongest_style.flash_color.r * 255.0f),
      (Uint8)(strongest_style.flash_color.g * 255.0f),
      (Uint8)(strongest_style.flash_color.b * 255.0f),
      (Uint8)(strongest_style.strength * 12.0f));
  SDL_FRect flash = ToFRect(viewport);
  bool fill_ok = color_ok && SDL_RenderFillRect(g_renderer, &flash);
  if (!fill_ok) DisableEffectBlend("scene flash");
  EndEffectBlend(&state);
}


/* Screen angle the UNROTATED eruption fireball art points at, in degrees and
 * on atan2's own scale: -90 is up-screen, +90 is down-screen.
 *
 * THE TWO AUTHORED FRAMES POINT OPPOSITE WAYS, and that is not an accident --
 * the ROM drew a climbing fireball pointing up and a falling one pointing
 * down, and swaps between them at the moment the record starts descending.
 * Measured on record $0FA4: composition goes $E7D0 -> $E7A6 on the same build
 * the arc's own height rate goes +568 to -547, and the same holds on every
 * cycle of every record. So the art already carries half the rotation, and a
 * single constant for both is wrong for one phase by exactly 180 degrees.
 *
 * That 180 is why this was hard to see. It reads as CORRECT for whichever
 * half of the arc it happens to suit, so the first report was "wrong on the
 * climb, right after the apex" and, once the constant was flipped, "right on
 * the climb, wrong after". Neither is a bug at the apex; both are one frame
 * being drawn with the other frame's assumption.
 *
 * Derived from that behaviour rather than from reading pixels off a rotated,
 * composited, CRT-filtered screenshot: with phase A at -90 the climb is
 * right, so A points up and B, its opposite, points down. If a fireball ever
 * looks 180 out again, check WHICH PHASE it is in before touching a number --
 * and note that the zero-rotation heading is a free measurement, since some
 * heading always rotates the art by exactly nothing. */
static double SimFireballArtHeadingDegrees(uint8_t phase) {
  return phase == kSimEffectPhase_VolcanoFireballB ? 90.0 : -90.0;
}

/* The ROM's own fireball art, drawn at the head of OUR arc.
 *
 * The eruption's billboards are withheld (kSimEruptionWithholdFireballBillboard):
 * the projected view replaces the ROM's three-phase fireball routine outright,
 * so drawing the record where the ROM put it puts a second, wrong fireball on
 * screen. But the ART is the right art, and a flame trail with nothing leading
 * it reads as smoke from nowhere. So this takes the two apart: the atlas
 * rectangle and the composition's own local extents come from the suppressed
 * OBJECT, and the position comes from the EFFECT -- which is the arc head, the
 * same number the trail's newest sample was taken from.
 *
 * That split is the point. Position through the object path would go back
 * through the virtual-height switch, the terrain shear, the height pop and the
 * depth bands, each of which can move a billboard somewhere its own trail is
 * not; through the effect there is one number and the art cannot disagree with
 * the smoke it is leading.
 *
 * Drawn after the particles so a fireball is in front of its own plume. */
/* One atlas entry placed by its own local rectangle relative to a shared
 * anchor. Returns whether anything was drawn. */
static bool DrawSimFireballHeadFragment(
    const SimRenderObject *object, Scene3DPoint anchor,
    float scale_x, float scale_y, bool rotate, double degrees) {
  SDL_FRect atlas = {
    object->atlas_x, object->atlas_y, object->atlas_w, object->atlas_h,
  };
  SDL_FRect destination = {
    anchor.x + object->local_x0 * scale_x,
    anchor.y + object->local_y0 * scale_y,
    (object->local_x1 - object->local_x0) * scale_x,
    (object->local_y1 - object->local_y0) * scale_y,
  };
  if (destination.w <= 0.0f || destination.h <= 0.0f) return false;
  if (!rotate) {
    /* Upright is the honest fallback, not a failure: each authored frame is
     * already drawn pointing the way its own phase travels, so an unturned
     * fireball is merely one that has not been leaned into its arc. */
    SDL_RenderTexture(g_renderer, NativeAtlasTexture(), &atlas,
                      &destination);
    return true;
  }
  /* Every fragment turns about the SHARED anchor. Rotating each about its own
   * centre pulls a multi-tile composition apart as it turns. */
  SDL_FPoint centre = { anchor.x - destination.x, anchor.y - destination.y };
  SDL_RenderTextureRotated(g_renderer, NativeAtlasTexture(), &atlas,
                           &destination, degrees, &centre, SDL_FLIP_NONE);
  return true;
}

void DrawSimEffectFireballHeads(
    const FrameSlot *slot, bool billboards, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16]) {
  if (!billboards || !slot->sim.effect_count || !slot->sim.atlas_valid ||
      !ArRenderTexture_IsValid(g_sim_obj_atlas_texture))
    return;

  /* Atlas state is set on the first fireball actually reached, not up front:
   * most towns have effects and no eruption, and this pass should cost them
   * nothing but the walk that finds that out. */
  bool blend_ready = false;
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    const SimEffectInstance *effect = &slot->sim.effects[i];
    if (effect->kind != kSimEffect_VolcanoFireball) continue;
    if (effect->geometry.kind != kSimEffectGeometry_Point) continue;
    if (!blend_ready) {
      if (!SimApplyAtlasBlendMode(SDL_BLENDMODE_BLEND)) return;
      blend_ready = true;
    }

    /* The RECORD ORIGIN at the arc's altitude, not the effect's own point:
     * the classified point is already offset to the middle of the art, and
     * the atlas rectangles below are placed relative to the origin. Adding
     * both would double the offset. */
    SimEffectLocalPoint origin = { 0, 0,
                                   effect->geometry.data.point.height };
    Scene3DPoint anchor;
    float scale_x, scale_y;
    if (!ProjectSimEffectPoint(slot, effect, &origin, source, viewport,
                               camera, matrix, &anchor, &scale_x, &scale_y))
      continue;

    /* Heading, resolved in SCREEN space: the same throw leans differently
     * under yaw and pitch, so the published map-space tangent is stepped
     * along, projected through the live camera, and the angle read off the
     * result rather than off the map. */
    bool rotate = false;
    double degrees = 0.0;
    if (effect->travel_valid) {
      /* Short enough to still be the tangent rather than a chord across the
       * apex, long enough to survive the projection's rounding. */
      const float kStep = 0.05f;
      SimEffectLocalPoint ahead = origin;
      ahead.height =
          (int16_t)(origin.height + effect->travel_height * kStep);
      Scene3DPoint tip;
      float ignored_x, ignored_y;
      if (ProjectSimEffectPointAt(
              slot, effect,
              (uint16_t)(effect->world_x + (int16_t)(effect->travel_x * kStep)),
              (uint16_t)(effect->world_y + (int16_t)(effect->travel_y * kStep)),
              &ahead, source, viewport, camera, matrix, &tip,
              &ignored_x, &ignored_y)) {
        float dx = tip.x - anchor.x, dy = tip.y - anchor.y;
        if (dx * dx + dy * dy > 0.0001f) {
          /* Turn the art from where it already points to where it is going.
           * SDL rotates clockwise, and screen Y is down, so both angles are
           * measured the same way and the turn is their difference. */
          degrees = atan2((double)dy, (double)dx) * 57.29577951 -
              SimFireballArtHeadingDegrees(effect->phase);
          rotate = true;
        }
      }
    }

    /* Prefer the record's own fragments. Failing that, borrow an identical
     * entry from a sibling: every fireball in the fountain wears the same two
     * authored compositions, and the ROM's sprite window drops a record's own
     * art whenever the record is off the side of the widescreen view while
     * its arc is still over the town. Measured across the captured eruption,
     * the record's own art is there for 88% of samples and a sibling's covers
     * all but 1.3%; the rest draw no head and the trail carries the throw. A
     * rectangle cannot simply be remembered instead -- the atlas repacks
     * every frame, so last frame's coordinates point at someone else's art.
     *
     * One pass: own fragments draw as they are found, and the first sibling
     * that could stand in is kept in case none were. */
    const SimRenderObject *borrow = NULL;
    bool drew = false;
    for (uint16_t n = 0; n < slot->sim.object_count; n++) {
      const SimRenderObject *object = &slot->sim.objects[n];
      if (!object->atlas_valid) continue;
      if (object->record_address == effect->record_address) {
        drew |= DrawSimFireballHeadFragment(object, anchor, scale_x, scale_y,
                                            rotate, degrees);
      } else if (!borrow &&
                 Sim3D_VolcanoFireballPhase(object->composition) ==
                     effect->phase) {
        borrow = object;
      }
    }
    /* One entry, not a whole record's worth of fragments: this is a stand-in
     * for missing art, not another actor's composition. */
    if (!drew && borrow)
      DrawSimFireballHeadFragment(borrow, anchor, scale_x, scale_y, rotate,
                                  degrees);
  }
}

void DrawSimEffectParticles(
    const FrameSlot *slot, bool particles, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16]) {
  if (!particles || !slot->sim.effect_count ||
      !EffectRendererAvailable())
    return;
  enum {
    kVertices = kSimMaxEffectInstances * kSimMaxEffectQuadsPerEffect * 4,
    kIndices = kSimMaxEffectInstances * kSimMaxEffectQuadsPerEffect * 6,
  };
  /* Static, not automatic. A trail long enough to show a whole eruption throw
   * makes the worst case a third of a megabyte, which is more than a render
   * thread's stack should be asked for -- and sizing the visual to fit a
   * stack frame is the wrong trade. Safe because this runs only on the
   * present thread, once per frame, and the batch does not outlive the call. */
  static SDL_Vertex vertices[kVertices];
  static int indices[kIndices];
  EffectBatch batch = {
    .vertices = vertices, .indices = indices,
    .vertex_capacity = kVertices, .index_capacity = kIndices,
  };
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    const SimEffectInstance *effect = &slot->sim.effects[i];
    SimEffectStyle style;
    if (!SimEffectStyleFor(effect, &style)) continue;
    if (!AppendSimEffectParticles(&batch, slot, effect, &style, source,
                                  viewport, camera, matrix))
      break;
  }
  if (!batch.index_count && !batch.overflow) return;
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  SubmitEffectBatch(&batch);
  EndEffectBlend(&state);
}

bool SimObjectIsPromotedHud(const FrameSlot *slot,
                                   const SimRenderObject *object) {
  const FrameSlotOverlayCapture *capture =
      &slot->overlay_captures[kFrameSlotOverlay_Obj];
  return object->tier == kSimRecordTier_Fixed && capture->oamCount &&
      object->oam_first >= capture->oamFirst &&
      object->oam_first + object->oam_count <=
          capture->oamFirst + capture->oamCount;
}

void DrawSimMapPlaneObject(const FrameSlot *slot,
                                  const SimRenderObject *object,
                                  int screen_origin_x, int screen_origin_y,
                                  SDL_Rect source, SDL_Rect viewport,
                                  const float matrix[16]) {
  float x0 = (float)(object->local_x0 + screen_origin_x);
  float y0 = (float)(object->local_y0 + screen_origin_y);
  float x1 = (float)(object->local_x1 + screen_origin_x);
  float y1 = (float)(object->local_y1 + screen_origin_y);
  Scene3DPoint points[4];
  const float map_x0 = object->world_x + object->offset_x + object->local_x0;
  const float map_y0 = object->world_y + object->offset_y + object->local_y0;
  const float map_x1 = object->world_x + object->offset_x + object->local_x1;
  const float map_y1 = object->world_y + object->offset_y + object->local_y1;
  if (!ProjectSimTexturePoint(matrix, source, viewport, x0, y0,
                              SimTerrainGroundHeightWorld(
                                  slot, source, map_x0, map_y0),
                              &points[0]) ||
      !ProjectSimTexturePoint(matrix, source, viewport, x1, y0,
                              SimTerrainGroundHeightWorld(
                                  slot, source, map_x1, map_y0),
                              &points[1]) ||
      !ProjectSimTexturePoint(matrix, source, viewport, x0, y1,
                              SimTerrainGroundHeightWorld(
                                  slot, source, map_x0, map_y1),
                              &points[2]) ||
      !ProjectSimTexturePoint(matrix, source, viewport, x1, y1,
                              SimTerrainGroundHeightWorld(
                                  slot, source, map_x1, map_y1),
                              &points[3]))
    return;
  float u0 = object->atlas_x / (float)kSimObjAtlasWidth;
  float v0 = object->atlas_y / (float)kSimObjAtlasHeight;
  float u1 = (object->atlas_x + object->atlas_w) /
      (float)kSimObjAtlasWidth;
  float v1 = (object->atlas_y + object->atlas_h) /
      (float)kSimObjAtlasHeight;
  const SDL_FColor white = { 1.0f, 1.0f, 1.0f, 1.0f };
  SDL_Vertex vertices[] = {
    {{points[0].x, points[0].y}, white, {u0, v0}},
    {{points[1].x, points[1].y}, white, {u1, v0}},
    {{points[2].x, points[2].y}, white, {u0, v1}},
    {{points[3].x, points[3].y}, white, {u1, v1}},
  };
  const int indices[] = { 0, 1, 3, 0, 3, 2 };
  SDL_RenderGeometry(g_renderer, NativeAtlasTexture(),
                     vertices, 4, indices, 6);
}
