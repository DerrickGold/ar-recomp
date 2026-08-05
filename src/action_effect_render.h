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

/* Unknown effect/phase/geometry/layer values are ignored (fail closed).
 * False reports malformed input or an internal capacity violation; the output
 * is always initialized and remains safe to submit when true. */
bool ActionEffectRender_Build(const ActionEffectFrame *frame,
                              bool lighting_enabled,
                              bool particles_enabled,
                              ActionEffectProjectPointFn project_point,
                              void *project_userdata,
                              ActionEffectRenderBatch *batch);

#endif  /* ACTION_EFFECT_RENDER_H */
