#ifndef ACTION_EFFECT_RENDER_H
#define ACTION_EFFECT_RENDER_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "action_effects.h"

enum {
  kActionEffectGlowSegments = 32,
  /* Each glow is one multi-ring gradient mesh, not a stack of overlapping
   * discs: a hot core ring, a body ring, and a transparent aura ring, joined
   * by triangle strips. Overlapping discs would double-blend along every
   * shared edge under SDL_BLENDMODE_ADD and band visibly; a single mesh gives
   * a continuous falloff with one colour stop per ring. */
  kActionEffectGlowRings = 3,
  kActionEffectGlowVertices =
      1 + kActionEffectGlowSegments * kActionEffectGlowRings,
  kActionEffectGlowIndices =
      kActionEffectGlowSegments * 3 +
      kActionEffectGlowSegments * 6 * (kActionEffectGlowRings - 1),

  /* One wide soft light-spill for the burst, plus one saturated flame body
   * per CLUSTER of touching parts. The worst case is every part landing in
   * its own cluster, hence +1 rather than +2 — the hot core is the flame
   * mesh's own centre vertex, not a glow of its own. */
  kActionEffectMaxGlows = kActionEffectMaxInstances + 1,
  /* Embers belong to the burst, not to a part, so this is a whole-effect
   * budget rather than a per-instance one — four quadrants must not each
   * throw their own separate spray. */
  /* 64, not 48: an impact splits this budget across every simultaneous burst
   * (Stardust detonates up to four at once), and a dozen sparks per blast is
   * too thin to read once half of them are still inside the source sprite. */
  kActionEffectMaxEmbers = 64,

  kActionEffectRenderMaxVertices =
      kActionEffectMaxGlows * kActionEffectGlowVertices +
      kActionEffectMaxEmbers * 4,
  kActionEffectRenderMaxIndices =
      kActionEffectMaxGlows * kActionEffectGlowIndices +
      kActionEffectMaxEmbers * 6,

  /* Scene actors remain independent light sources. Two gradient meshes give
   * each a soft environmental spill plus a tighter luminous body; the small
   * fixed spark budget keeps the worst-case batch bounded and deterministic. */
  kActionSceneEffectGlowsPerInstance = 2,
  kActionSceneEffectParticlesPerInstance = 12,
  kActionSceneEffectMaxGlows =
      kActionSceneEffectMaxInstances * kActionSceneEffectGlowsPerInstance,
  kActionSceneEffectMaxParticles =
      kActionSceneEffectMaxInstances * kActionSceneEffectParticlesPerInstance,
  /* The boss strike adds two screen-space filament layers over at most 24
   * authored OAM-row segments: a broad amber corona and a narrow white-gold
   * core.
   * The mapped room has one boss and its lifecycle can publish only one active
   * strike (the separate floor child is Impact), so keep that cardinality in
   * the bounded contract rather than inflating every scene slot by 80 verts. */
  kActionSceneEffectMaxLightningFilaments = 1,
  kActionSceneEffectLightningSegments = 24,
  kActionSceneEffectLightningLayers = 2,
  kActionSceneEffectLightningVerticesPerInstance =
      kActionSceneEffectLightningSegments * 4 *
      kActionSceneEffectLightningLayers,
  kActionSceneEffectLightningIndicesPerInstance =
      kActionSceneEffectLightningSegments * 6 *
      kActionSceneEffectLightningLayers,
  kActionSceneEffectRenderMaxVertices =
      kActionSceneEffectMaxGlows * kActionEffectGlowVertices +
      kActionSceneEffectMaxParticles * 4 +
      kActionSceneEffectMaxLightningFilaments *
          kActionSceneEffectLightningVerticesPerInstance,
  kActionSceneEffectRenderMaxIndices =
      kActionSceneEffectMaxGlows * kActionEffectGlowIndices +
      kActionSceneEffectMaxParticles * 6 +
      kActionSceneEffectMaxLightningFilaments *
          kActionSceneEffectLightningIndicesPerInstance,
};

typedef bool (*ActionEffectProjectPointFn)(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, SDL_FPoint *point);

/* Renderer-independent output. The game-specific module owns spell styles,
 * clocks and geometry while present.c owns only the backend submission. */
typedef struct ActionEffectRenderBatch {
  SDL_Vertex vertices[kActionEffectRenderMaxVertices];
  int indices[kActionEffectRenderMaxIndices];
  int vertex_count;
  int index_count;
} ActionEffectRenderBatch;

typedef struct ActionSceneEffectRenderBatch {
  SDL_Vertex vertices[kActionSceneEffectRenderMaxVertices];
  int indices[kActionSceneEffectRenderMaxIndices];
  int vertex_count;
  int index_count;
} ActionSceneEffectRenderBatch;

/* Unknown effect/phase/geometry/layer values are ignored (fail closed).
 * False reports malformed input or an internal capacity violation; the output
 * is always initialized and remains safe to submit when true. */
bool ActionEffectRender_Build(const ActionEffectFrame *frame,
                              bool lighting_enabled,
                              bool particles_enabled,
                              ActionEffectProjectPointFn project_point,
                              void *project_userdata,
                              ActionEffectRenderBatch *batch);

bool ActionSceneEffectRender_Build(const ActionSceneEffectFrame *frame,
                                   bool lighting_enabled,
                                   bool particles_enabled,
                                   ActionEffectProjectPointFn project_point,
                                   void *project_userdata,
                                   ActionSceneEffectRenderBatch *batch);

#endif  /* ACTION_EFFECT_RENDER_H */
