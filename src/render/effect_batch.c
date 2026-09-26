#include "render/effect_batch.h"

#include <stdatomic.h>
#include <stdio.h>

/* Host effects use the renderer abstraction's standard additive/alpha blend
 * modes and untextured geometry, not a backend shader. Those are portable
 * API paths,
 * but not a promise of pixel-identical rasterization across Metal, Vulkan,
 * Direct3D and software. Capability is verified at the point of use: a backend
 * may legally substitute the closest blend mode, so a successful set is
 * followed by a get-and-compare. Any rejection or substitution fails closed. */
static atomic_int s_effect_blend_supported = ATOMIC_VAR_INIT(1);
static atomic_int s_effect_geometry_supported = ATOMIC_VAR_INIT(1);

void EffectRenderer_DisableBlend(ArRenderDevice *device, const char *operation) {
  int expected = 1;
  if (!atomic_compare_exchange_strong_explicit(
          &s_effect_blend_supported, &expected, 0,
          memory_order_acq_rel, memory_order_acquire))
    return;
  fprintf(stderr,
          "[host-effects] effect blend pass unavailable at %s (%s) — "
          "effect lighting and particles disabled\n",
          operation, ArRenderDevice_LastError(device));
}

static void DisableEffectGeometry(ArRenderDevice *device, const char *operation) {
  int expected = 1;
  if (!atomic_compare_exchange_strong_explicit(
          &s_effect_geometry_supported, &expected, 0,
          memory_order_acq_rel, memory_order_acquire))
    return;
  fprintf(stderr,
          "[host-effects] geometry pass unavailable at %s (%s) — "
          "effect lighting and particles disabled\n",
          operation, ArRenderDevice_LastError(device));
}

bool EffectRenderer_Available(void) {
  return atomic_load_explicit(
             &s_effect_blend_supported, memory_order_acquire) != 0 &&
      atomic_load_explicit(
             &s_effect_geometry_supported, memory_order_acquire) != 0;
}

bool EffectRenderer_Submit(ArRenderDevice *device, const EffectBatch *batch,
                           ArRenderBlendMode blend) {
  if (!batch || batch->overflow) {
    static bool logged;
    if (!logged) {
      logged = true;
      fprintf(stderr,
              "[host-effects] internal geometry batch capacity exceeded — "
              "effect pass skipped\n");
    }
    return false;
  }
  if (!batch->index_count) return true;
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend,
    .blend = blend,
  };
  if (ArRenderDevice_DrawGeometryWithState(
          device, ArRenderTexture_Invalid(), batch->vertices,
          batch->vertex_count, batch->indices, batch->index_count, &state))
    return true;
  DisableEffectGeometry(device, "geometry submit");
  return false;
}

void EffectRenderer_Reset(void) {
  atomic_store_explicit(&s_effect_blend_supported, 1, memory_order_release);
  atomic_store_explicit(&s_effect_geometry_supported, 1, memory_order_release);
}
