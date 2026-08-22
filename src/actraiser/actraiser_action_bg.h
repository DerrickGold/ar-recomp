#ifndef ACTRAISER_ACTION_BG_H
#define ACTRAISER_ACTION_BG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "action/action_bg_plan.h"
#include "action/action_bg_world.h"

struct Ppu;
struct Dma;
struct DioramaRoomOverride;
struct ActionRoomScene;
struct ActionRoomSceneFrameState;

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

typedef struct ActRaiserActionRoomSceneCompareResult {
  size_t compared;
  size_t mismatches;
  int first_tile_x;
  int first_tile_y;
  uint16_t first_immutable;
  uint16_t first_live;
} ActRaiserActionRoomSceneCompareResult;

typedef enum ActRaiserActionRoomSceneFrameField {
  kActRaiserActionRoomSceneFrameField_Bg1HScroll = 0,
  kActRaiserActionRoomSceneFrameField_Bg1VScroll,
  kActRaiserActionRoomSceneFrameField_Bg2HScroll,
  kActRaiserActionRoomSceneFrameField_Bg2VScroll,
  kActRaiserActionRoomSceneFrameField_Mosaic,
  kActRaiserActionRoomSceneFrameField_MainScreen,
  kActRaiserActionRoomSceneFrameField_SubScreen,
  kActRaiserActionRoomSceneFrameField_MainWindow,
  kActRaiserActionRoomSceneFrameField_SubWindow,
  kActRaiserActionRoomSceneFrameField_Cgwsel,
  kActRaiserActionRoomSceneFrameField_Cgadsub,
  kActRaiserActionRoomSceneFrameField_Bgmode,
  kActRaiserActionRoomSceneFrameField_Bg1Sc,
  kActRaiserActionRoomSceneFrameField_Bg2Sc,
  kActRaiserActionRoomSceneFrameField_Count,
} ActRaiserActionRoomSceneFrameField;

typedef struct ActRaiserActionRoomSceneFrameCompareResult {
  size_t compared;
  size_t mismatches;
  ActRaiserActionRoomSceneFrameField first_field;
  uint16_t first_immutable;
  uint16_t first_live;
} ActRaiserActionRoomSceneFrameCompareResult;

typedef enum ActRaiserActionBgFallbackReason {
  kActRaiserActionBgFallback_ForcedBlank = 0,
  kActRaiserActionBgFallback_WrongMode,
  kActRaiserActionBgFallback_LayerDisabled,
  kActRaiserActionBgFallback_NativeTilemap,
  kActRaiserActionBgFallback_InvalidSource,
  kActRaiserActionBgFallback_Allocation,
  kActRaiserActionBgFallback_ScrollPhase,
  kActRaiserActionBgFallback_AuthenticEdge,
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
  uint64_t provider_preflight_layers;
  uint64_t provider_preflight_tiles;
  uint64_t provider_preflight_mismatches;
  uint64_t provider_preflight_outside_world;
  uint64_t provider_eligible_layers;
  uint64_t provider_layers;
  uint64_t provider_lookups;
  uint64_t provider_tiles;
  uint64_t provider_outside_world;
  uint64_t room_scene_loads;
  uint64_t room_scene_load_failures;
  uint64_t room_scene_layers_compared;
  uint64_t room_scene_tiles_compared;
  uint64_t room_scene_mismatches;
  uint64_t room_scene_frames_built;
  uint64_t room_scene_raster_hold_frames;
  uint64_t room_scene_scanlines_compared;
  uint64_t room_scene_registers_compared;
  uint64_t room_scene_register_mismatches;
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
/* Compare the authentic viewport against the live native ring. Cyclic worlds
 * wrap only the decoded lookup X; the native address retains the original
 * world coordinate so this remains an exact streamer oracle. */
bool ActRaiserActionBg_CompareLayer(
    const ActionBgWorld *world,
    const ActRaiserActionBgLayerSnapshot *snapshot,
    const uint16_t *vram, size_t vram_words,
    bool wrap_world_x,
    ActRaiserActionBgCompareResult *result);

/* Resolve the real finite-world rows immediately above and below ActRaiser's
 * authentic 224-line action viewport. The game clamps its camera against
 * world_height - 225, so the lower expression intentionally uses 225 rather
 * than 224. Each result is independently capped by budget. */
void ActRaiserActionBg_ResolveVerticalMargins(
    int camera_y, int world_height, int budget,
    int *top, int *bottom);

/* Capture the complete action-background decision record and build its pure
 * plan plus the mechanical generic-PPU projection. No renderer state changes. */
bool ActRaiserActionBg_BuildPlan(
    const uint8_t *wram, size_t wram_size, const struct Ppu *ppu,
    bool decorative_padding_enabled, ActionBgPlan *plan,
    ActionBgPresentationPolicy *presentation);

/* Resolve a validated plan's extent inheritance into the generic runtime PPU
 * caps. Call after PpuSetExtraSpace, which resets the frame-scoped caps.
 * Source selection and edge strategy remain separate seams. */
bool ActRaiserActionBg_ApplyPlanExtents(
    const ActionBgPlan *plan, struct Ppu *ppu);

/* Production provider gate shared with presentation-aware camera policy.
 * Default-on; AR_ACTION_BG_HLE=0 is the exact native control. Keeping the
 * environment decision here prevents related HLE seams from drifting. */
bool ActRaiserActionBg_HleEnabled(void);

/* Registers immutable cart bytes for the default-off room-scene shadow
 * comparator. AR_ACTION_ROOM_SCENE_COMPARE=1 compares each newly published
 * WRAM world with the shared arbitrary-room loader; it never changes provider
 * eligibility, PPU state, or gameplay. The ROM storage remains caller-owned. */
bool ActRaiserActionBg_InitRoomScenes(const uint8_t *rom, size_t rom_size);

/* Pure full-world comparator used by the game-side shadow observer and ROM-free
 * tests. False means either source is incomplete or their dimensions differ. */
bool ActRaiserActionBg_CompareRoomSceneLayer(
    const struct ActionRoomScene *scene, uint8_t bg_layer,
    const ActionBgWorld *world,
    ActRaiserActionRoomSceneCompareResult *result);

/* Compare one visible scanline's resolved immutable frame record with the PPU
 * registers that are about to render it. Row 0 additionally checks the stable
 * video-profile registers. This is pure and never changes PPU state. */
bool ActRaiserActionBg_CompareRoomSceneFrameLine(
    const struct ActionRoomSceneFrameState *state, const struct Ppu *ppu,
    unsigned output_y,
    ActRaiserActionRoomSceneFrameCompareResult *result);

/* Optional live shadow for the shared raster/compositor bootstrap. Begin once
 * after the frame's HDMA channels are initialized, then observe row N just
 * before ppu_runLine(N + 1). Both calls are inert unless
 * AR_ACTION_ROOM_SCENE_COMPARE=1. */
void ActRaiserActionBg_BeginRoomSceneFrame(
    const uint8_t *wram, size_t wram_size, const struct Ppu *ppu,
    const struct Dma *dma);
void ActRaiserActionBg_ObserveRoomSceneFrameLine(
    const struct Ppu *ppu, unsigned output_y);

/* Default-on BH7 renderer adapter. Unless `AR_ACTION_BG_HLE=0`, publish and
 * bind every plan layer whose source is a finite world map. A zero-mismatch,
 * zero-outside comparison against the exact live native viewport is required
 * before provider ownership includes authentic pixels. Returns the bitmask of
 * bound PPU layers. The function always clears prior bindings first, so a
 * rejected/disabled frame fails closed. */
uint8_t ActRaiserActionBg_BindPlan(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    struct Ppu *ppu);

/* Diorama render-only variant. `virtual_room` is the base action-room record
 * authored by the standalone editor; NULL preserves the authentic priority
 * split. Classification changes only captured presentation surfaces, never
 * the native PPU composition. */
uint8_t ActRaiserActionBg_BindPlanWithVirtualLayers(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    const struct DioramaRoomOverride *virtual_room, struct Ppu *ppu);

/* Default-off frame observer. `AR_ACTION_BG_HLE_COMPARE=1` enables it; it only
 * reads WRAM/PPU state and never binds a renderer provider or mutates emulated
 * memory. */
void ActRaiserActionBg_ObserveFrame(const uint8_t *wram, size_t wram_size,
                                    const struct Ppu *ppu);
void ActRaiserActionBg_Reset(void);
void ActRaiserActionBg_Shutdown(void);
const ActRaiserActionBgDiagnostics *ActRaiserActionBg_GetDiagnostics(void);

#endif  /* ACTRAISER_ACTION_BG_H */
