/* The present_sim3d.c <-> present_world_nav.c boundary.
 *
 * Same role as present_internal.h one level up: NOT a public API, just the
 * declared list of what crosses between the SIM town renderer and the world-map
 * navigation renderer that was split out of it. If this header grows, the split
 * is eroding and the two files are becoming one again.
 *
 * The D6 no-live-globals invariant applies here as it does to present.c and
 * present_sim3d.c: nothing in this header may carry live game state. Every
 * present-time decision still arrives via the `const FrameSlot *`. */
#ifndef AR_PRESENT_SIM3D_INTERNAL_H
#define AR_PRESENT_SIM3D_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "present.h"
#include "render/render_types.h"

/* ---- shared cloud model ---------------------------------------------------
 * The world-map sky reuses the town's cloud layers and noise so the two skies
 * cannot drift apart in look. */
enum { kSimCloudTexturePixels = 512 };

typedef struct SimCloudLayer {
  float scale, offset_x, offset_y, weight, drift_x, drift_y;
} SimCloudLayer;

extern const SimCloudLayer kSimCloudLayers[];
extern const int kSimCloudLayerCount;
uint32_t SimCloudTexel(int x, int y);

/* ---- SIM 3D helpers the world-map renderer calls -------------------------
 * These are defined by the SIM 3D presentation stage that owns the work and
 * are exposed only because the world-map renderer deliberately shares it. */
extern const float kPi;

int InsertSimGroundCoordinate(float *coordinates, int count, int capacity,
                              float coordinate);
void SimShadowLight(const FrameSlot *slot, float *light_x, float *light_y);
ArRenderTexture EnsureSimUnderlayTexture(const FrameSlot *slot);
/* Returns the optional blur only when it represents the requested world-map
 * revision. Consumers must never infer freshness from allocation alone. */
ArRenderTexture SimUnderlayBlurTexture(uint32_t serial);
void DrawSimBackdrop(const FrameSlot *slot, ArRenderRectI viewport,
                     const float matrix[16]);

/* ---- world-map entry points ----------------------------------------------
 * Defined in present_world_nav.c. PresentWorldNavigation3D and
 * UploadWorldNavigationComposition are declared in present_internal.h because
 * present.c calls them directly; only the reset is local to this pair. */
void PresentWorldNav_ResetResources(void);

#endif /* AR_PRESENT_SIM3D_INTERNAL_H */
