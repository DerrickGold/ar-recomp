/* Internal boundary for the split presentation family.
 *
 * NOT a public API — that is present.h. This header declares the scene and
 * host-UI stages used by present_frame.c plus the helpers shared with the
 * split SIM/world-navigation renderers.
 *
 * It deliberately exposes present.c internals ONLY. It must never carry live
 * game state (g_ppu, g_settings, g_snes_width, g_ws_extra,
 * g_active_pixel_aspect): both translation units keep the D6 no-live-globals
 * invariant, under which every present-time decision arrives via the
 * `const FrameSlot *`. Widening this header with a live global would break the
 * invariant present.c exists to enforce. */
#ifndef AR_PRESENT_INTERNAL_H
#define AR_PRESENT_INTERNAL_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "present.h"
#include "presentation_outcome.h"
#include "render/render_types.h"

/* ---- shared effect types -------------------------------------------------
 * Both translation units need these by value / by field, so the definitions
 * live here rather than in either .c. Moved verbatim out of present.c. */
typedef struct EffectBatch {
  ArRenderVertex2D *vertices;
  int32_t *indices;
  int vertex_count, index_count;
  int vertex_capacity, index_capacity;
  bool overflow;
} EffectBatch;

/* ---- present.c helpers the sim renderer calls ----------------------------
 * These stay DEFINED in present.c and lost their `static` for this header.
 * ComputePresentationViewport is already public in present.h — not repeated. */
SDL_FRect ToFRect(SDL_Rect r);
void PresentHudOverlayComposited(const FrameSlot *slot, SDL_Rect viewport);
void PresentCompositeScene(const FrameSlot *slot, float alpha);
bool PresentAuthenticScene(const FrameSlot *slot, SDL_Rect viewport);
bool PresentAuthenticPictureInPicture(const FrameSlot *slot,
                                      SDL_Rect priority_viewport);
bool PresentComparisonTransitionOverlay(uint8_t alpha, const char *label);
void PresentHostUi(const FrameSlot *slot, SDL_Rect viewport,
                   SDL_Point output_size,
                   double presentation_fps);
bool EffectRendererAvailable(void);
void DisableEffectBlend(const char *operation);
bool SubmitEffectBatch(EffectBatch *batch, ArRenderBlendMode blend);

/* ---- sim entry points present.c calls back into --------------------------
 * Defined in present_sim3d.c. PresentSim3D requires entry without a custom GPU
 * render state, owns and unbinds every state it binds, and cannot preserve an
 * inherited binding because SDL exposes no getter for it. Apply an outer
 * shader after this stage instead. */
PresentationOutcome PresentSim3D(const FrameSlot *slot);
PresentationOutcome PresentWorldNavigation3D(const FrameSlot *slot);
void UploadSimTownCanvas(void);
void UploadWorldNavigationComposition(const FrameSlot *slot);

/* The sim half of PresentRendererResources_Reset (which stays in present.c and
 * calls this). See its comment there for why the reset exists at all. */
void PresentSim3D_ResetResources(void);

/* Owned by present_sim3d.c: cleared by the reset above, cleared to 0 by the rim
 * blend path when a blend-mode set actually fails, and read by the public
 * Present_SimRimMaskSupported() accessor that stays in present.c. */
extern SDL_AtomicInt s_sim_rim_mask_supported;

#endif /* AR_PRESENT_INTERNAL_H */
