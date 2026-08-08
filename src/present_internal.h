/* T2a: the present.c <-> present_sim3d.c boundary.
 *
 * NOT a public API — that is present.h. This header exists only because the
 * SIM-mode 3D renderer was split out of present.c into its own translation
 * unit; it declares the present.c internals that renderer calls, and the sim
 * entry points present.c calls back into.
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

/* ---- shared effect types -------------------------------------------------
 * Both translation units need these by value / by field, so the definitions
 * live here rather than in either .c. Moved verbatim out of present.c. */
typedef struct EffectRenderState {
  SDL_BlendMode blend;
  Uint8 r, g, b, a;
} EffectRenderState;

typedef struct EffectBatch {
  SDL_Vertex *vertices;
  int *indices;
  int vertex_count, index_count;
  int vertex_capacity, index_capacity;
  bool overflow;
} EffectBatch;

/* ---- present.c helpers the sim renderer calls ----------------------------
 * These stay DEFINED in present.c and lost their `static` for this header.
 * ComputePresentationViewport is already public in present.h — not repeated. */
SDL_FRect ToFRect(SDL_Rect r);
void ApplyLogicalPresentation(const FrameSlot *slot);
void PresentHudOverlayComposited(const FrameSlot *slot, SDL_Rect viewport);
void PresentSceneInspector(const FrameSlot *slot, SDL_Rect viewport);
void PresentCheatBadge(const FrameSlot *slot, SDL_Rect viewport);
bool EffectRendererAvailable(void);
void DisableEffectAdd(const char *operation);
bool BeginEffectAdd(EffectRenderState *state);
void EndEffectAdd(const EffectRenderState *state);
bool SubmitEffectBatch(EffectBatch *batch);

/* ---- sim entry points present.c calls back into --------------------------
 * Defined in present_sim3d.c. */
void PresentSim3D(const FrameSlot *slot);
bool PresentWorldNavigation3D(const FrameSlot *slot);
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
