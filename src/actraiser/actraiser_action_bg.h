#ifndef ACTRAISER_ACTION_BG_H
#define ACTRAISER_ACTION_BG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "action/action_bg_plan.h"
#include "action/action_bg_world.h"

struct Ppu;

/* ActRaiser-specific capture and differential observer for SPEC-bg-hle BH2.
 * The pure world decoder remains game-agnostic; this adapter is the only place
 * that knows the game's low-WRAM state layout and 64x64 action tilemap rings. */

typedef struct ActRaiserActionBgLayerSnapshot {
  ActionBgDecodeInput decode;
  uint16_t camera_x;
  uint16_t camera_y;
  uint16_t tilemap_base;
  uint8_t bgsc;
} ActRaiserActionBgLayerSnapshot;

typedef struct ActRaiserActionBgCompareResult {
  size_t compared;
  size_t mismatches;
  size_t outside_world;
  int first_tile_x;
  int first_tile_y;
  int first_outside_tile_x;
  int first_outside_tile_y;
  uint16_t first_hle;
  uint16_t first_native;
} ActRaiserActionBgCompareResult;

typedef enum ActRaiserActionBgFallbackReason {
  kActRaiserActionBgFallback_ForcedBlank = 0,
  kActRaiserActionBgFallback_WrongMode,
  kActRaiserActionBgFallback_LayerDisabled,
  kActRaiserActionBgFallback_NativeTilemap,
  kActRaiserActionBgFallback_InvalidSource,
  kActRaiserActionBgFallback_Allocation,
  kActRaiserActionBgFallback_CompareFailure,
  kActRaiserActionBgFallback_Count,
} ActRaiserActionBgFallbackReason;

typedef struct ActRaiserActionBgDiagnostics {
  uint64_t frames_observed;
  uint64_t layer_activations;
  uint64_t layers_compared;
  uint64_t tiles_compared;
  uint64_t mismatches;
  uint64_t outside_world;
  uint64_t provider_frames;
  uint64_t provider_layers;
  uint64_t provider_lookups;
  uint64_t provider_tiles;
  uint64_t provider_outside_world;
  uint64_t fallbacks[kActRaiserActionBgFallback_Count];
} ActRaiserActionBgDiagnostics;

/* Pure helpers kept public so the capture and ring comparison are pinned by a
 * ROM-free target instead of being trusted only through runtime logs. */
bool ActRaiserActionBg_CaptureLayer(
    const uint8_t *wram, size_t wram_size, unsigned layer, uint8_t bgsc,
    ActRaiserActionBgLayerSnapshot *out);
bool ActRaiserActionBg_WorldRingEligible(
    const ActRaiserActionBgLayerSnapshot *snapshot, size_t vram_words);
bool ActRaiserActionBg_RingAddress(uint16_t tilemap_base, int tile_x,
                                   int tile_y, size_t vram_words,
                                   size_t *address);
bool ActRaiserActionBg_CompareLayer(
    const ActionBgWorld *world,
    const ActRaiserActionBgLayerSnapshot *snapshot,
    const uint16_t *vram, size_t vram_words,
    ActRaiserActionBgCompareResult *result);

/* Capture the complete action-background decision record and build its pure
 * BH3 plan plus temporary PPU-policy projection. No renderer state is changed. */
bool ActRaiserActionBg_BuildPlan(
    const uint8_t *wram, size_t wram_size, const struct Ppu *ppu,
    bool decorative_padding_enabled, ActionBgPlan *plan,
    ActionBgPresentationPolicy *presentation);

/* Default-off BH4 renderer adapter. With `AR_ACTION_BG_HLE=1`, publish and
 * bind every plan layer whose source is a finite world map. Only synthetic
 * margins consume these bindings; authentic pixels remain on the native ring.
 * Returns the bitmask of bound PPU layers. The function always clears prior
 * bindings first, so a rejected/disabled frame fails closed. */
uint8_t ActRaiserActionBg_BindPlan(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    struct Ppu *ppu);

/* Default-off frame observer. `AR_ACTION_BG_HLE_COMPARE=1` enables it; it only
 * reads WRAM/PPU state and never binds a renderer provider or mutates emulated
 * memory. */
void ActRaiserActionBg_ObserveFrame(const uint8_t *wram, size_t wram_size,
                                    const struct Ppu *ppu);
void ActRaiserActionBg_Reset(void);
void ActRaiserActionBg_Shutdown(void);
const ActRaiserActionBgDiagnostics *ActRaiserActionBg_GetDiagnostics(void);

#endif  /* ACTRAISER_ACTION_BG_H */
