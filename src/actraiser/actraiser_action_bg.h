#ifndef AR_ACTRAISER_ACTION_BG_H
#define AR_ACTRAISER_ACTION_BG_H
/* ActRaiserActionBg: the game adapter for the action background HLE
 * (SPEC-bg-hle). Captures the action stage's BG layers from the game's
 * low-WRAM layout and 64 x 64 tilemap rings, builds and binds the level plan,
 * and compares the pure world decoder against the native rings.
 * Phase: capture.
 * Tests: tests/actraiser_action_bg_test.c */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "action/action_bg_plan.h"
#include "action/action_bg_world.h"
#include "regional/media/regional_media.h"
#include "snesrecomp/runner.h"

uint8_t ActRaiserActionBg_TerrainProfile(void);

struct DioramaRoomOverride;
struct ActionEnvironmentScene;
bool ActRaiserActionBg_BindEnvironmentScenery(struct ActionEnvironmentScene *scene);
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

typedef enum ActRaiserActionRoomStageField {
  kActRaiserActionRoomStageField_Dimensions = 0,
  kActRaiserActionRoomStageField_Map,
  kActRaiserActionRoomStageField_MetatileDefinitions,
  kActRaiserActionRoomStageField_Count,
} ActRaiserActionRoomStageField;

typedef struct ActRaiserActionRoomStageCompareResult {
  size_t compared;
  size_t mismatches;
  size_t first_offset;
  ActRaiserActionRoomStageField first_field;
  uint8_t first_immutable;
  uint8_t first_live;
} ActRaiserActionRoomStageCompareResult;

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
  size_t mismatches_by_field[kActRaiserActionRoomSceneFrameField_Count];
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
  uint64_t provider_batches;
  uint64_t provider_tiles;
  uint64_t provider_outside_world;
  uint64_t provider_tile_band_cache_builds;
  uint64_t provider_tile_band_cache_hits;
  uint64_t room_scene_loads;
  uint64_t room_scene_load_failures;
  uint64_t room_scene_layers_compared;
  uint64_t room_scene_tiles_compared;
  uint64_t room_scene_mismatches;
  uint64_t room_scene_stage_layers_compared;
  uint64_t room_scene_stage_bytes_compared;
  uint64_t room_scene_stage_mismatches;
  uint64_t room_scene_frames_built;
  uint64_t room_scene_raster_hold_frames;
  uint64_t room_scene_scanlines_compared;
  uint64_t room_scene_registers_compared;
  uint64_t room_scene_register_mismatches;
  uint64_t room_scene_hle_layers;
  uint64_t room_scene_hle_fallbacks;
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
 * authentic 224-line action viewport. Scanlines 1..224 sample camera_y + 1
 * through camera_y + 224: row 0 is an upper-margin row at camera_y = 0.
 * The lower expression intentionally uses 225, matching the game's camera
 * bound. Each result is independently capped by budget. */
void ActRaiserActionBg_ResolveVerticalMargins(
    int camera_y, int world_height, int budget,
    int *top, int *bottom);

/* Keep the requested two-sided capture height at finite world edges by
 * spending unavailable rows on the opposite side, within the shared surface
 * capacity. Small worlds still stop at their actual top and bottom. */
void ActRaiserActionBg_ResolveVerticalCaptureMargins(
    int camera_y, int world_height, int budget,
    int *top, int *bottom);

/* Union a native axis with the active terrain's pasted cells, ignoring empty
 * editor workspace. Extents remain in native world coordinates. Use these
 * for capture margins and presentation camera padding; native gameplay world
 * dimensions remain unchanged. */
void ActRaiserActionBg_ResolveDioramaVerticalExtent(
    const struct DioramaRoomOverride *room, unsigned layer, int native_height,
    int *world_y0, int *world_height);
void ActRaiserActionBg_ResolveDioramaHorizontalExtent(
    const struct DioramaRoomOverride *room, unsigned layer, int native_width,
    int *world_x0, int *world_width);

/* Capture the complete action-background decision record and build its pure
 * plan plus the mechanical generic-PPU projection. No renderer state changes. */
bool ActRaiserActionBg_BuildPlan(
    const uint8_t *wram, size_t wram_size,
    const SrPpuStateSnapshot *ppu,
    bool decorative_padding_enabled, ActionBgPlan *plan,
    ActionBgPresentationPolicy *presentation);

/* Capture one coherent public PPU snapshot and build the same pure plan. */
bool ActRaiserActionBg_BuildCurrentPlan(
    const uint8_t *wram, size_t wram_size,
    bool decorative_padding_enabled, ActionBgPlan *plan,
    ActionBgPresentationPolicy *presentation);

/* Resolve a validated plan's extent inheritance into generic renderer caps.
 * Call after the frame's horizontal-margin reset. Source selection and edge
 * strategy remain separate seams. */
bool ActRaiserActionBg_ApplyPlanExtents(
    const ActionBgPlan *plan);

/* Bind the opaque runner used by the frame-scoped adapter services. */
void ActRaiserActionBg_BindRunner(SrRunnerHandle *runner);

/* Production provider gate shared with presentation-aware camera policy.
 * Default-on; AR_ACTION_BG_HLE=0 is the exact native control. Keeping the
 * environment decision here prevents related HLE seams from drifting. */
bool ActRaiserActionBg_HleEnabled(void);

/* Registers immutable cart bytes for the default-off room-scene shadows.
 * AR_ACTION_ROOM_SCENE_COMPARE=1 compares expanded worlds plus frame state;
 * AR_ACTION_ROOM_STAGE_COMPARE=1 independently compares exact command-4/5
 * WRAM outputs. Their live publication cache is host-isolated from the
 * production provider, so neither changes provider ownership, PPU state, or
 * gameplay. The ROM storage remains caller-owned. */
bool ActRaiserActionBg_InitRoomScenes(const uint8_t *rom, size_t rom_size);

/* Pure full-world comparator used by the game-side shadow observer and ROM-free
 * tests. False means either source is incomplete or their dimensions differ. */
bool ActRaiserActionBg_CompareRoomSceneLayer(
    const struct ActionRoomScene *scene, uint8_t bg_layer,
    const ActionBgWorld *world,
    ActRaiserActionRoomSceneCompareResult *result);

/* Exact command-4/5 background asset image. Stage writes only the active map
 * bytes, its two pixel dimensions, and the selected 2 KiB definition table;
 * bytes outside those native destinations are preserved. This is the pure
 * core for the future loader HLE and is not invoked on game state yet.
 * Compare is its read-only differential oracle against native staging. */
bool ActRaiserActionBg_StageRoomSceneLayer(
    uint8_t *wram, size_t wram_size,
    const struct ActionRoomScene *scene, uint8_t bg_layer);
bool ActRaiserActionBg_CompareRoomSceneStage(
    const struct ActionRoomScene *scene, uint8_t bg_layer,
    const uint8_t *wram, size_t wram_size,
    ActRaiserActionRoomStageCompareResult *result);

/* Publish one immutable room-scene background through the production finite
 * world representation. This is the pure adapter used by the default
 * ROM-derived provider source and by ROM-free parity tests. */
bool ActRaiserActionBg_UpdateWorldFromRoomScene(
    ActionBgWorld *world, const struct ActionRoomScene *scene,
    uint8_t bg_layer);

/* Compare one visible scanline's resolved immutable frame record with the PPU
 * registers that are about to render it. Row 0 additionally checks the stable
 * video-profile registers. This is pure and never changes PPU state. */
bool ActRaiserActionBg_CompareRoomSceneFrameLine(
    const struct ActionRoomSceneFrameState *state,
    const SrPpuStateSnapshot *ppu,
    unsigned output_y,
    ActRaiserActionRoomSceneFrameCompareResult *result);

/* Optional live shadow for the shared raster/compositor bootstrap. Begin once
 * after the frame's HDMA channels are initialized, then observe row N just
 * before ppu_runLine(N + 1). Both calls are inert unless
 * AR_ACTION_ROOM_SCENE_COMPARE=1. */
void ActRaiserActionBg_BeginRoomSceneFrame(
    const uint8_t *wram, size_t wram_size);
void ActRaiserActionBg_ObserveRoomSceneFrameLine(
    const SrPpuStateSnapshot *ppu, unsigned output_y);
bool ActRaiserActionBg_RoomSceneFrameObserverActive(void);

/* Default-on BH7 renderer adapter. Unless `AR_ACTION_BG_HLE=0`, publish and
 * bind every plan layer whose source is a finite world map. The publication
 * defaults to the immutable ROM-derived room scene, with automatic live-WRAM
 * fallback. `AR_ACTION_ROOM_SCENE_HLE=0` is the exact staged-WRAM source
 * control. A zero-mismatch, zero-outside comparison against the exact live
 * native viewport is still required before provider ownership includes
 * authentic pixels. Returns the bitmask of bound PPU layers. The function
 * always clears prior bindings first, so a rejected or disabled frame fails
 * closed. */
uint8_t ActRaiserActionBg_BindPlan(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan);

/* Diorama render-only variant. `virtual_room` is the base action-room record
 * authored by the standalone editor; NULL preserves the authentic priority
 * split. Classification changes only captured presentation surfaces, never
 * the native PPU composition. */
uint8_t ActRaiserActionBg_BindPlanWithVirtualLayers(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    const struct DioramaRoomOverride *virtual_room);

/* Default-off frame observer. `AR_ACTION_BG_HLE_COMPARE=1` enables it; it only
 * reads WRAM/PPU state and publishes into a private diagnostic world. It never
 * binds or republishes a renderer provider and never mutates emulated memory. */
void ActRaiserActionBg_ObserveFrame(const uint8_t *wram, size_t wram_size);
void ActRaiserActionBg_Reset(void);
/* Room-load publication only, after native terrain has been projected. The
 * renderer receives a value, never mutable settings or CPU ownership. */
void ActRaiserActionBg_BeginRoomVariants(uint8_t terrain, uint8_t mosaic,
                                         ArRegionalMediaBytes death_heim_characters);
void ActRaiserActionBg_Shutdown(void);
const ActRaiserActionBgDiagnostics *ActRaiserActionBg_GetDiagnostics(void);

/* Presentation-only black pixels; callers pass the live PPU coordinate policy
 * and scroll state. No character/palette memory or normal output is changed. */
bool ActRaiserActionBg_PixelEditsActive(void);
bool ActRaiserActionBg_PixelLayerHasEdits(unsigned bg);
/* True only after the current capture accepted this layer's authored callback. */
bool ActRaiserActionBg_CaptureTilesBound(unsigned bg);
/* Bind authored tiles/masks and guard terrain to native BG capture export. */
bool ActRaiserActionBg_BindCaptureTiles(uint8_t capture_mask, uint8_t apron_mask);
/* A current world binding with ordinary live-world edges and no tuned caps
 * can supply diorama guard columns without requiring authored tile edits. */
bool ActRaiserActionBg_WorldApronAvailable(unsigned bg);
/* The bound finite source, including active pasted terrain, in screen X.
 * Uses the same raster-scroll delta as native/scenery lookup. False for
 * unbound or cyclic worlds; outputs are cleared on failure. */
bool ActRaiserActionBg_HorizontalSourceBounds(
    unsigned bg, uint16_t hscroll, int *x0, int *x1);
/* Authored scenery may address signed cells beyond the immutable map edges.
 * Local x/y are displayed 16px cell coordinates, before character flips. */
bool ActRaiserActionBg_StampAt(unsigned bg, int source_x, int sample_y,
                              uint16_t hscroll, uint16_t vscroll,
                              uint16_t *entry, uint8_t *band,
                              uint8_t *local_x, uint8_t *local_y,
                              bool *black, bool *blank);
bool ActRaiserActionBg_PixelBlackAt(unsigned bg, int source_x, int sample_y,
                                    uint16_t hscroll, uint16_t vscroll,
                                    uint8_t *band);
bool ActRaiserActionBg_PixelTransparentAt(unsigned bg, int source_x, int sample_y,
                                         uint16_t hscroll, uint16_t vscroll,
                                         uint8_t *band);
/* Native scenery for extra horizontal capture. Requires a current verified
 * world binding eligible for guard columns or authored scenery/framing. */
bool ActRaiserActionBg_NativeSceneryAt(unsigned bg, int source_x, int sample_y,
                                      uint16_t hscroll, uint16_t vscroll,
                                      uint16_t *entry, uint8_t *band,
                                      uint8_t *local_x, uint8_t *local_y,
                                      bool *black);

#endif  /* AR_ACTRAISER_ACTION_BG_H */
