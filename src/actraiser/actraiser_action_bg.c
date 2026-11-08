#include "diorama/diorama_camera_policy.h"
#include "actraiser_action_bg.h"
#include "diorama/diorama_scene_extent.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "action/action_room_scene.h"
#include "action/action_trunk.h"
#include "action/action_voxel.h"
#include "action/action_environment_scene.h"
#include "action/action_room_terrain.h"
#include "action/action_room_mosaic.h"
#include "action/action_lava_bubbles.h"
#include "deterministic_hash.h"
#include "diorama/diorama_layer_order.h"
#include "snesrecomp/runner.h"

enum {
  kActionBgLayerCount = 2,
  kActionBgRingTiles = 64,
  kActionBgRingWords = kActionBgRingTiles * kActionBgRingTiles,
};

typedef struct ActRaiserActionBgObserver {
  ActionBgWorld *provider_world[kActionBgLayerCount];
  ActionBgWorld *comparison_world[kActionBgLayerCount];
  ActRaiserActionBgDiagnostics diagnostics;
  uint64_t room_scene_register_mismatches_by_field[
      kActRaiserActionRoomSceneFrameField_Count];
  uint32_t provider_reported_mismatch_serial[kActionBgLayerCount];
  uint32_t provider_reported_outside_serial[kActionBgLayerCount];
  uint32_t comparison_reported_mismatch_serial[kActionBgLayerCount];
  uint32_t comparison_reported_outside_serial[kActionBgLayerCount];
  uint32_t room_scene_compared_serial[kActionBgLayerCount];
  bool room_scene_stage_compared[kActionBgLayerCount];
  const uint8_t *rom;
  size_t rom_size;
  ActionRoomScene *room_scene;
  uint8_t terrain_profile, mosaic_pattern;
  ArRegionalMediaBytes death_heim_characters;
  ActionRoomSceneFrameState room_frame;
  ActionRoomSceneFrameState previous_room_frame;
  ActionRoomSceneFrameState authentic_room_frame;
  uint16_t last_game_frame;
  uint16_t last_room_camera_x;
  uint16_t prior_room_camera_x;
  uint16_t room_frame_current_camera_x;
  uint16_t room_frame_raster_camera_x;
  uint16_t room_frame_prior_camera_x;
  uint16_t last_authentic_camera_x;
  uint16_t last_capture_live_camera_x;
  uint8_t map_group;
  uint8_t map_number;
  bool reported_fallback[kActRaiserActionBgFallback_Count];
  bool frame_valid;
  bool map_valid;
  bool room_scene_attempted;
  bool room_scene_valid;
  bool room_frame_valid;
  bool previous_room_frame_valid;
  bool room_frame_uses_previous;
  bool room_frame_raster_matches_current;
  bool room_camera_valid;
  bool authentic_camera_valid;
  bool capture_live_camera_valid;
  bool room_scene_entry_frame;
  bool reported_room_frame_mismatch;
  bool reported_room_scene_hle_fallback[kActionBgLayerCount];
  bool forced_blank;
  int enabled;
  int hle_enabled;
  int room_scene_hle_enabled;
  int room_scene_compare_enabled;
  int room_scene_stage_compare_enabled;
  int room_scene_compare_verbose;
  unsigned room_scene_verbose_reports;
} ActRaiserActionBgObserver;

typedef struct ActRaiserActionBgProvider {
  const ActionBgWorld *world;
  const DioramaRoomOverride *virtual_room;
  ActionBgLayerPlan capture_plan;
  uint8_t *tile_band_cache;
  size_t tile_band_capacity;
  size_t tile_band_count;
  const ActionBgWorld *tile_band_world;
  uint64_t tile_band_rules_hash;
  uint32_t tile_band_world_serial;
  unsigned tile_band_width;
  unsigned tile_band_height;
  bool wrap_world_x;
  bool tile_band_cache_valid;
  bool reported_tile_band_cache_failure;
  uint8_t layer;
  bool pixel_edits_active, pixel_band_cache_active;
  bool capture_native_tiles;
  bool world_apron_available;
  bool horizontal_bounds_available;
  int scenery_x0, scenery_width;
  int camera_x, camera_y;
  uint16_t hscroll_anchor, vscroll_anchor;
  int pixel_cell_x, pixel_cell_y;
  const DioramaPixelEdit *pixel_edit;
  ActionLavaBubbles bubbles;
} ActRaiserActionBgProvider;

static ActRaiserActionBgObserver s_observer = {
  .enabled = -1,
  .hle_enabled = -1,
  .room_scene_hle_enabled = -1,
  .room_scene_compare_enabled = -1,
  .room_scene_stage_compare_enabled = -1,
  .room_scene_compare_verbose = -1,
};
static ActRaiserActionBgProvider s_provider[kActionBgLayerCount];
static uint8_t s_capture_edit_mask;
static SrRunnerHandle *s_runner;
static const SnesRunnerApi *s_runner_api;

void ActRaiserActionBg_BindRunner(SrRunnerHandle *runner) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  const uint64_t required =
      SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_BORROWED_U16_SPANS |
      SR_RUNNER_CAP_DMA_STATE | SR_RUNNER_CAP_PPU_BACKGROUND_POLICY;
  s_runner = runner;
  s_runner_api =
      runner && api &&
          api->struct_size >= SNES_RUNNER_API_PPU_BACKGROUND_POLICY_SIZE &&
          (api->capabilities & required) == required
      ? api : NULL;
}

static bool QueryPpuState(SrPpuStateSnapshot *state) {
  if (!state || !s_runner || !s_runner_api) return false;
  *state = (SrPpuStateSnapshot){ .struct_size = sizeof(*state) };
  return s_runner_api->query_ppu_state(s_runner, state) == SR_RESULT_OK;
}

static bool QueryDmaState(SrDmaStateSnapshot *state) {
  if (!state || !s_runner || !s_runner_api) return false;
  *state = (SrDmaStateSnapshot){ .struct_size = sizeof(*state) };
  return s_runner_api->query_dma_state(s_runner, state) == SR_RESULT_OK;
}

static bool BorrowVram(uint64_t lifetime_generation,
                       SrBorrowedU16Span *vram) {
  if (!vram || !s_runner || !s_runner_api) return false;
  *vram = (SrBorrowedU16Span){ .struct_size = sizeof(*vram) };
  return s_runner_api->borrow_u16_memory(
             s_runner, SR_MEMORY_VRAM, vram) == SR_RESULT_OK &&
      vram->lifetime_generation == lifetime_generation &&
      vram->data != NULL && vram->element_count == SR_PPU_VRAM_WORD_COUNT;
}

_Static_assert(kActionBgLayerCount == 2,
               "ActRaiser action HLE currently owns BG1/BG2 only");
_Static_assert(kActionRoomSceneFrameHeight == SR_PPU_NATIVE_HEIGHT,
               "authentic camera frames must cover every PPU line");
_Static_assert(kActRaiserBgLayerStateStride == 4,
               "background capture offsets assume a four-byte layer stride");

static uint16_t ReadWram16(const uint8_t *wram, size_t address) {
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

static void WriteWram16(uint8_t *wram, size_t address, uint16_t value) {
  wram[address] = (uint8_t)value;
  wram[address + 1] = (uint8_t)(value >> 8);
}

static bool RoomSceneStageLayout(
    const ActionRoomScene *scene, uint8_t bg_layer,
    const ActionRoomSceneBg **bg, size_t *dimension_address,
    size_t *map_address, size_t *definitions_address) {
  if (!scene || bg_layer < 1 || bg_layer > kActionRoomSceneBgCount ||
      !ActionRoomScene_HasBackground(scene, bg_layer))
    return false;
  const ActionRoomSceneBg *layer = &scene->bg[bg_layer - 1u];
  const size_t layer_offset = (size_t)(bg_layer - 1u) *
      kActRaiserBgLayerStateStride;
  if (layer->map_size > 0x4000u ||
      (unsigned)layer->pages_wide * 256u > UINT16_MAX ||
      (unsigned)layer->pages_high * 256u > UINT16_MAX)
    return false;
  if (bg) *bg = layer;
  if (dimension_address)
    *dimension_address = kActRaiserWram_Bg1Width + layer_offset;
  if (map_address)
    *map_address = bg_layer == 1
        ? kActRaiserWram_Bg1Map : kActRaiserWram_Bg2Map;
  if (definitions_address)
    *definitions_address = bg_layer == 1
        ? kActRaiserWram_Bg1MetatileDefinitions
        : kActRaiserWram_Bg2MetatileDefinitions;
  return true;
}

bool ActRaiserActionBg_StageRoomSceneLayer(
    uint8_t *wram, size_t wram_size,
    const ActionRoomScene *scene, uint8_t bg_layer) {
  const ActionRoomSceneBg *bg = NULL;
  size_t dimension_address = 0, map_address = 0, definitions_address = 0;
  if (!wram ||
      !RoomSceneStageLayout(scene, bg_layer, &bg, &dimension_address,
                            &map_address, &definitions_address) ||
      dimension_address > wram_size || 4u > wram_size - dimension_address ||
      map_address > wram_size || bg->map_size > wram_size - map_address ||
      definitions_address > wram_size ||
      kActionRoomSceneMetatileBytes > wram_size - definitions_address)
    return false;

  WriteWram16(wram, dimension_address,
              (uint16_t)((unsigned)bg->pages_wide * 256u));
  WriteWram16(wram, dimension_address + 2u,
              (uint16_t)((unsigned)bg->pages_high * 256u));
  memcpy(wram + map_address, bg->map, bg->map_size);
  for (size_t offset = 0; offset < kActionRoomSceneMetatileBytes;
       offset += 2u) {
    wram[definitions_address + offset] = bg->metatiles[offset + 1u];
    wram[definitions_address + offset + 1u] = bg->metatiles[offset];
  }
  return true;
}

static void CompareRoomStageByte(
    ActRaiserActionRoomStageCompareResult *result,
    ActRaiserActionRoomStageField field, size_t offset,
    uint8_t immutable, uint8_t live) {
  result->compared++;
  if (immutable == live) return;
  if (!result->mismatches) {
    result->first_field = field;
    result->first_offset = offset;
    result->first_immutable = immutable;
    result->first_live = live;
  }
  result->mismatches++;
}

bool ActRaiserActionBg_CompareRoomSceneStage(
    const ActionRoomScene *scene, uint8_t bg_layer,
    const uint8_t *wram, size_t wram_size,
    ActRaiserActionRoomStageCompareResult *result) {
  if (result) {
    memset(result, 0, sizeof(*result));
    result->first_field = kActRaiserActionRoomStageField_Count;
  }
  const ActionRoomSceneBg *bg = NULL;
  size_t dimension_address = 0, map_address = 0, definitions_address = 0;
  if (!result || !wram ||
      !RoomSceneStageLayout(scene, bg_layer, &bg, &dimension_address,
                            &map_address, &definitions_address) ||
      dimension_address > wram_size || 4u > wram_size - dimension_address ||
      map_address > wram_size || bg->map_size > wram_size - map_address ||
      definitions_address > wram_size ||
      kActionRoomSceneMetatileBytes > wram_size - definitions_address)
    return false;

  const uint16_t width = (uint16_t)((unsigned)bg->pages_wide * 256u);
  const uint16_t height = (uint16_t)((unsigned)bg->pages_high * 256u);
  const uint8_t dimensions[4] = {
    (uint8_t)width, (uint8_t)(width >> 8),
    (uint8_t)height, (uint8_t)(height >> 8),
  };
  for (size_t offset = 0; offset < sizeof(dimensions); offset++)
    CompareRoomStageByte(result, kActRaiserActionRoomStageField_Dimensions,
                         offset, dimensions[offset],
                         wram[dimension_address + offset]);
  for (size_t offset = 0; offset < bg->map_size; offset++)
    CompareRoomStageByte(result, kActRaiserActionRoomStageField_Map,
                         offset, bg->map[offset], wram[map_address + offset]);
  for (size_t offset = 0; offset < kActionRoomSceneMetatileBytes;
       offset++) {
    const uint8_t immutable = bg->metatiles[offset ^ 1u];
    CompareRoomStageByte(
        result, kActRaiserActionRoomStageField_MetatileDefinitions,
        offset, immutable, wram[definitions_address + offset]);
  }
  return true;
}

void ActRaiserActionBg_ResolveVerticalMargins(
    int camera_y, int world_height, int budget,
    int *top, int *bottom) {
  if (budget < 0) budget = 0;
  /* PPU scanlines 1..224 sample camera_y + 1..224. Native world row 0
   * therefore belongs to the upper margin even when camera_y is zero. */
  int available_top = camera_y >= 0 ? camera_y + 1 : 0;
  int available_bottom =
      world_height - kActRaiserActionCameraViewportHeight - camera_y;
  if (available_bottom < 0) available_bottom = 0;
  if (available_top > budget) available_top = budget;
  if (available_bottom > budget) available_bottom = budget;
  if (top) *top = available_top;
  if (bottom) *bottom = available_bottom;
}

void ActRaiserActionBg_ResolveDioramaVerticalExtent(
    const DioramaRoomOverride *room, unsigned layer, int native_height,
    int *world_y0, int *world_height) {
  DioramaScenery_AxisExtent(room, layer, native_height, true, world_y0, world_height);
}

void ActRaiserActionBg_ResolveDioramaHorizontalExtent(
    const DioramaRoomOverride *room, unsigned layer, int native_width,
    int *world_x0, int *world_width) {
  DioramaScenery_AxisExtent(room, layer, native_width, false, world_x0, world_width);
}

void ActRaiserActionBg_ResolveVerticalCaptureMargins(
    int camera_y, int world_height, int budget,
    int *top, int *bottom) {
  DioramaCameraPolicy_VerticalMargins(camera_y, world_height, budget, top, bottom);
}

bool ActRaiserActionBg_CaptureLayer(
    const uint8_t *wram, size_t wram_size, unsigned layer, uint8_t bgsc,
    ActRaiserActionBgLayerSnapshot *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!wram || !out || layer >= kActionBgLayerCount ||
      wram_size < kActRaiserWram_BgAttributes +
                      kActRaiserBgLayerStateStride + 1u ||
      wram_size > kActionBgMaxWramBytes)
    return false;

  const size_t offset = layer * kActRaiserBgLayerStateStride;
  out->decode = (ActionBgDecodeInput) {
    .wram = wram,
    .wram_size = wram_size,
    .world_width = ReadWram16(wram, kActRaiserWram_Bg1Width + offset),
    .world_height = ReadWram16(wram, kActRaiserWram_Bg1Height + offset),
    .map_page = ReadWram16(wram, kActRaiserWram_BgMapPage + offset),
    .metatile_table =
        ReadWram16(wram, kActRaiserWram_BgMetatileTable + offset),
    .word_mask = ReadWram16(wram, kActRaiserWram_BgWordMask + offset),
    .attributes = wram[kActRaiserWram_BgAttributes + offset],
  };
  out->camera_x = ReadWram16(wram, kActRaiserWram_Bg1CameraX + offset);
  out->camera_y = ReadWram16(wram, kActRaiserWram_Bg1CameraY + offset);
  out->tilemap_base =
      ReadWram16(wram, kActRaiserWram_BgTilemapBase + offset);
  out->bgsc = bgsc;
  return true;
}

bool ActRaiserActionBg_WorldRingEligible(
    const ActRaiserActionBgLayerSnapshot *snapshot, size_t vram_words) {
  if (!snapshot || (snapshot->bgsc & 3u) != 3u) return false;
  const size_t ppu_base = (size_t)(snapshot->bgsc & 0xFCu) << 8;
  const size_t decoder_base = snapshot->tilemap_base;
  return decoder_base == ppu_base && decoder_base <= vram_words &&
      kActionBgRingWords <= vram_words - decoder_base;
}

bool ActRaiserActionBg_RingAddress(uint16_t tilemap_base, int tile_x,
                                   int tile_y, size_t vram_words,
                                   size_t *address) {
  if (!address || tile_x < 0 || tile_y < 0) return false;
  const unsigned x = (unsigned)tile_x & 63u;
  const unsigned y = (unsigned)tile_y & 63u;
  const size_t result = (size_t)tilemap_base + (x & 31u) +
      ((size_t)(y & 31u) << 5) + ((x & 32u) ? 0x400u : 0u) +
      ((y & 32u) ? 0x800u : 0u);
  if (result >= vram_words) return false;
  *address = result;
  return true;
}

static int32_t WrapWorldTile(int32_t coordinate, unsigned extent) {
  if (!extent) return coordinate;
  if ((uint32_t)coordinate < extent) return coordinate;
  /* Decoded Action worlds are usually power-of-two page grids. Unsigned
   * masking is defined for negative inputs after conversion and avoids a
   * signed divide in the provider's per-tile scanout callback. */
  if ((extent & (extent - 1u)) == 0)
    return (int32_t)((uint32_t)coordinate & (extent - 1u));
  int64_t wrapped = coordinate % (int64_t)extent;
  if (wrapped < 0) wrapped += extent;
  return (int32_t)wrapped;
}

static ActionBgLookupResult LookupWorldTile(
    const ActionBgWorld *world, bool wrap_world_x,
    int32_t tile_x, int32_t tile_y, uint16_t *entry) {
  if (!world) return kActionBgLookup_ProviderFailure;
  if (wrap_world_x)
    tile_x = WrapWorldTile(tile_x, ActionBgWorld_TileWidth(world));
  return ActionBgWorld_Lookup(world, tile_x, tile_y, entry);
}

bool ActRaiserActionBg_CompareLayer(
    const ActionBgWorld *world,
    const ActRaiserActionBgLayerSnapshot *snapshot,
    const uint16_t *vram, size_t vram_words,
    bool wrap_world_x, ActRaiserActionBgCompareResult *result) {
  if (result) {
    memset(result, 0, sizeof(*result));
    result->first_tile_x = -1;
    result->first_tile_y = -1;
    result->first_outside_tile_x = -1;
    result->first_outside_tile_y = -1;
  }
  if (!world || !snapshot || !vram || !result ||
      !ActRaiserActionBg_WorldRingEligible(snapshot, vram_words))
    return false;

  ActRaiserActionBgCompareResult built = {
    .first_tile_x = -1,
    .first_tile_y = -1,
    .first_outside_tile_x = -1,
    .first_outside_tile_y = -1,
  };
  const int first_x = snapshot->camera_x >> 3;
  /* Authentic PPU scanlines are numbered 1..224. Horizontal pixels remain
   * 0..255, so the two axes intentionally have different inclusive ranges. */
  const int first_y = (snapshot->camera_y + 1) >> 3;
  const int last_x = (snapshot->camera_x +
                      kActRaiserAuthenticWidth - 1) >> 3;
  const int last_y = (snapshot->camera_y +
                      kActRaiserAuthenticHeight) >> 3;
  for (int tile_y = first_y; tile_y <= last_y; tile_y++) {
    for (int tile_x = first_x; tile_x <= last_x; tile_x++) {
      uint16_t hle = 0;
      const ActionBgLookupResult lookup = LookupWorldTile(
          world, wrap_world_x, tile_x, tile_y, &hle);
      if (lookup == kActionBgLookup_OutsideWorld) {
        if (!built.outside_world) {
          built.first_outside_tile_x = tile_x;
          built.first_outside_tile_y = tile_y;
        }
        built.outside_world++;
        continue;
      }
      if (lookup != kActionBgLookup_Tile) return false;
      size_t address = 0;
      if (!ActRaiserActionBg_RingAddress(snapshot->tilemap_base,
                                         tile_x, tile_y, vram_words,
                                         &address))
        return false;
      const uint16_t native = vram[address];
      built.compared++;
      if (hle == native) continue;
      if (!built.mismatches) {
        built.first_tile_x = tile_x;
        built.first_tile_y = tile_y;
        built.first_hle = hle;
        built.first_native = native;
      }
      built.mismatches++;
    }
  }
  *result = built;
  return true;
}

bool ActRaiserActionBg_BuildPlan(
    const uint8_t *wram, size_t wram_size,
    const SrPpuStateSnapshot *ppu,
    bool decorative_padding_enabled, ActionBgPlan *plan,
    ActionBgPresentationPolicy *presentation) {
  if (plan) memset(plan, 0, sizeof(*plan));
  if (presentation) memset(presentation, 0, sizeof(*presentation));
  if (!wram || !ppu || !plan || !presentation ||
      wram_size <= kActRaiserWram_DeathHeimProgress)
    return false;

  ActionBgFrameState state = {
    .map_group = wram[kActRaiserWram_MapGroup],
    .map_number = wram[kActRaiserWram_CurrentMap],
    .death_heim_progress = wram[kActRaiserWram_DeathHeimProgress],
    .decorative_padding_enabled = decorative_padding_enabled,
  };
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActRaiserActionBgLayerSnapshot snapshot;
    if (!ActRaiserActionBg_CaptureLayer(
            wram, wram_size, layer,
            ppu->background_tilemap_control[layer], &snapshot))
      return false;
    state.layer[layer] = (ActionBgLayerState) {
      .camera_x = snapshot.camera_x,
      .camera_y = snapshot.camera_y,
      .world_width = snapshot.decode.world_width,
      .world_height = snapshot.decode.world_height,
      .tilemap_base = snapshot.tilemap_base,
      .bgsc = snapshot.bgsc,
    };
  }
  if (!ActionBgPlan_Build(&state, plan) ||
      !ActionBgPlan_CompilePresentation(plan, presentation)) {
    memset(plan, 0, sizeof(*plan));
    memset(presentation, 0, sizeof(*presentation));
    return false;
  }
  return true;
}

bool ActRaiserActionBg_BuildCurrentPlan(
    const uint8_t *wram, size_t wram_size,
    bool decorative_padding_enabled, ActionBgPlan *plan,
    ActionBgPresentationPolicy *presentation) {
  SrPpuStateSnapshot ppu;
  return QueryPpuState(&ppu) && ActRaiserActionBg_BuildPlan(
      wram, wram_size, &ppu, decorative_padding_enabled,
      plan, presentation);
}

static uint16_t PpuExtentCap(ActionBgExtentMode mode, uint16_t fixed) {
  return mode == kActionBgExtent_Fixed
      ? fixed : (uint16_t)SR_PPU_LAYER_EXTENT_AVAILABLE;
}

static bool HorizontalExtentsEqual(const ActionBgHorizontalExtent *a,
                                   const ActionBgHorizontalExtent *b) {
  return a->mode == b->mode && a->left == b->left && a->right == b->right;
}

bool ActRaiserActionBg_ApplyPlanExtents(const ActionBgPlan *plan) {
  SrPpuStateSnapshot ppu;
  SrPpuLayerExtentUpdate updates[SR_PPU_LAYER_EXTENT_UPDATE_MAX];
  uint32_t update_count = 0;
  if (!ActionBgPlan_Validate(plan) || !QueryPpuState(&ppu)) return false;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    const ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    const ActionBgHorizontalExtent *horizontal =
        &layer_plan->horizontal_extent;
    const ActionBgVerticalExtent *vertical = &layer_plan->vertical_extent;
    /* PpuSetExtraSpace already reset every side to Available. Avoid touching
     * 224 row entries on the behavior-neutral majority; only a real default
     * cap needs to seed the arrays before band overrides refine them. */
    if (horizontal->mode == kActionBgExtent_Fixed ||
        vertical->mode == kActionBgExtent_Fixed) {
      if (update_count >= SR_PPU_LAYER_EXTENT_UPDATE_MAX) return false;
      updates[update_count++] = (SrPpuLayerExtentUpdate) {
        .kind = SR_PPU_LAYER_EXTENT_DEFAULT,
        .layer = layer,
        .left = PpuExtentCap(horizontal->mode, horizontal->left),
        .right = PpuExtentCap(horizontal->mode, horizontal->right),
        .top = PpuExtentCap(vertical->mode, vertical->top),
        .bottom = PpuExtentCap(vertical->mode, vertical->bottom),
      };
    }
    /* Validation guarantees sorted, non-overlapping bands. Inherit requires
     * no write because the default has already seeded the row arrays (or the
     * per-frame reset left them Available). */
    for (unsigned i = 0; i < layer_plan->band_count; i++) {
      const ActionBgBand *band = &layer_plan->bands[i];
      int y0 = 0, y1 = 0;
      if (!ActionBgLayerPlan_ResolveBand(layer_plan, i, &y0, &y1))
        return false;
      if (y0 < 0) y0 = 0;
      if (y1 > kActRaiserAuthenticHeight) y1 = kActRaiserAuthenticHeight;
      if (y0 >= y1) continue;
      if (band->horizontal_extent.mode != kActionBgExtent_Inherit &&
          !HorizontalExtentsEqual(horizontal, &band->horizontal_extent)) {
        if (update_count >= SR_PPU_LAYER_EXTENT_UPDATE_MAX) return false;
        updates[update_count++] = (SrPpuLayerExtentUpdate) {
          .kind = SR_PPU_LAYER_EXTENT_HORIZONTAL_BAND,
          .layer = layer,
          .y0 = (uint32_t)y0,
          .y1 = (uint32_t)y1,
          .left = PpuExtentCap(
              band->horizontal_extent.mode,
              band->horizontal_extent.left),
          .right = PpuExtentCap(
              band->horizontal_extent.mode,
              band->horizontal_extent.right),
        };
      }
    }
  }
  if (update_count == 0u) return true;
  const SrPpuLayerExtentRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = ppu.lifetime_generation,
    .updates = updates,
    .update_count = update_count,
  };
  return s_runner_api->update_ppu_layer_extents(
      s_runner, &request) == SR_RESULT_OK;
}

static const char *FallbackName(ActRaiserActionBgFallbackReason reason) {
  static const char *const names[kActRaiserActionBgFallback_Count] = {
    [kActRaiserActionBgFallback_ForcedBlank] = "forced-blank",
    [kActRaiserActionBgFallback_WrongMode] = "non-mode1",
    [kActRaiserActionBgFallback_LayerDisabled] = "layer-disabled",
    [kActRaiserActionBgFallback_NativeTilemap] = "native-tilemap",
    [kActRaiserActionBgFallback_InvalidSource] = "invalid-world-source",
    [kActRaiserActionBgFallback_Allocation] = "allocation-failure",
    [kActRaiserActionBgFallback_ScrollPhase] = "scroll-phase-mismatch",
    [kActRaiserActionBgFallback_AuthenticEdge] = "authentic-world-edge",
    [kActRaiserActionBgFallback_CompareFailure] = "compare-failure",
  };
  return reason < kActRaiserActionBgFallback_Count ? names[reason] : "unknown";
}

static bool CompareEnabled(void) {
  if (s_observer.enabled < 0) {
    const char *value = getenv("AR_ACTION_BG_HLE_COMPARE");
    s_observer.enabled = value && value[0] && value[0] != '0';
    if (s_observer.enabled)
      fprintf(stderr,
              "[action-bg-hle] differential observer enabled\n");
  }
  return s_observer.enabled != 0;
}

bool ActRaiserActionBg_HleEnabled(void) {
  if (s_observer.hle_enabled < 0) {
    const char *value = getenv("AR_ACTION_BG_HLE");
    /* BH7: provider ownership is the production default. Keep one exact
     * explicit-off A/B (`AR_ACTION_BG_HLE=0`); an absent or empty variable
     * selects the default, while any non-zero value remains a compatible
     * explicit-on spelling for older harnesses. */
    s_observer.hle_enabled = !value || !value[0] || value[0] != '0';
    if (s_observer.hle_enabled)
      fprintf(stderr,
              "[action-bg-hle] full world-layer provider enabled\n");
    else
      fprintf(stderr,
              "[action-bg-hle] provider disabled by AR_ACTION_BG_HLE=0\n");
  }
  return s_observer.hle_enabled != 0;
}

static bool RoomSceneCompareEnabled(void) {
  if (s_observer.room_scene_compare_enabled < 0) {
    const char *value = getenv("AR_ACTION_ROOM_SCENE_COMPARE");
    s_observer.room_scene_compare_enabled =
        value && value[0] && value[0] != '0';
    if (s_observer.room_scene_compare_enabled)
      fprintf(stderr,
              "[action-room-scene] immutable/live comparison enabled\n");
  }
  return s_observer.room_scene_compare_enabled != 0;
}

static bool RoomSceneStageCompareEnabled(void) {
  if (s_observer.room_scene_stage_compare_enabled < 0) {
    const char *value = getenv("AR_ACTION_ROOM_STAGE_COMPARE");
    s_observer.room_scene_stage_compare_enabled =
        value && value[0] && value[0] != '0';
    if (s_observer.room_scene_stage_compare_enabled)
      fprintf(stderr,
              "[action-room-stage] native WRAM comparison enabled\n");
  }
  return s_observer.room_scene_stage_compare_enabled != 0;
}

static bool RoomSceneHleEnabled(void) {
  if (s_observer.room_scene_hle_enabled < 0) {
    const char *value = getenv("AR_ACTION_ROOM_SCENE_HLE");
    s_observer.room_scene_hle_enabled =
        !value || !value[0] || value[0] != '0';
    if (s_observer.room_scene_hle_enabled)
      fprintf(stderr,
              "[action-room-scene] ROM-derived world source enabled\n");
    else
      fprintf(stderr,
              "[action-room-scene] ROM-derived world source disabled by "
              "AR_ACTION_ROOM_SCENE_HLE=0\n");
  }
  return s_observer.room_scene_hle_enabled != 0;
}

static bool RoomSceneCompareVerbose(void) {
  if (s_observer.room_scene_compare_verbose < 0) {
    const char *value = getenv("AR_ACTION_ROOM_SCENE_COMPARE_VERBOSE");
    s_observer.room_scene_compare_verbose =
        value && value[0] && value[0] != '0';
  }
  return s_observer.room_scene_compare_verbose != 0;
}

/* One immutable native-art cache per supported room and terrain. Both queued
 * frame slots and the producer's landing observer borrow these assets. */
static ActionTrunkAssets *s_trunk_rooms[kDioramaTerrainProfileCount][2];
const ActionTrunkAssets *ActRaiserActionBg_TrunkAssets(unsigned group,unsigned room,
    const DioramaRoomOverride *edits) {
  if(!DioramaTrunk_Resolve(group,room,edits).enabled||!s_observer.rom||
      s_observer.terrain_profile>=kDioramaTerrainProfileCount)return NULL;
  ActionTrunkAssets **cache=&s_trunk_rooms[s_observer.terrain_profile][room-6];
  if(*cache)return *cache;
  ActionRoomScene *scene=malloc(sizeof(*scene));
  ActionTrunkAssets *assets=malloc(sizeof(*assets));
  const bool ok=scene&&assets&&ActionRoomScene_Load(scene,s_observer.rom,s_observer.rom_size,group,room)&&
      ActionRoomTerrain_Project(scene,s_observer.terrain_profile)&&ActionTrunk_Build(assets,scene,edits);
  free(scene);
  if(!ok){free(assets);return NULL;}
  *cache=assets;return assets;
}

/* The producer mutates only these source snapshots. Published geometry never
 * borrows them, and queued/displayed frames keep their own counted reference. */
static struct {
  ActionRoomScene *source;
  uint16_t *tiles;
  const ActionVoxelGeometry *geometry;
  /* Castle chains change their exact native opacity every two game frames.
   * Keep each finite phase, rather than rebuilding its entire room or using
   * another phase's front/matte mask. Each entry owns a separate reference. */
  const ActionVoxelGeometry *castle_phases[4];
  uint64_t terrain_key;
  int authored_bounds[4];
  bool authored_records[kDioramaVoxelCellMax];
  bool source_valid, terrain_valid, animation_safe;
} s_voxel;
static void ResetVoxelCastlePhases(void) {
  for(unsigned i=0;i<4;++i) {
    ActionVoxel_Release(s_voxel.castle_phases[i]);
    s_voxel.castle_phases[i]=NULL;
  }
}
static void ResetVoxel(void) {
  ActionVoxel_Release(s_voxel.geometry);
  s_voxel.geometry = NULL;
  ResetVoxelCastlePhases();
  s_voxel.source_valid = s_voxel.terrain_valid = s_voxel.animation_safe = false;
}
const ActionVoxelGeometry *ActRaiserActionBg_VoxelGeometry(unsigned group, unsigned room) {
  return ActionVoxel_Active(s_voxel.geometry, group, room) ? s_voxel.geometry : NULL;
}

bool ActRaiserActionBg_InitRoomScenes(const uint8_t *rom, size_t rom_size) {
  s_observer.rom = rom;
  s_observer.rom_size = rom_size;
  s_observer.terrain_profile = s_observer.mosaic_pattern = 0;
  s_observer.death_heim_characters=(ArRegionalMediaBytes){0};
  s_observer.room_scene_valid = false;
  s_observer.room_scene_attempted = false;
  s_observer.room_frame_valid = false;
  s_observer.previous_room_frame_valid = false;
  s_observer.room_frame_uses_previous = false;
  s_observer.room_camera_valid = false;
  s_observer.authentic_camera_valid = false;
  s_observer.capture_live_camera_valid = false;
  s_observer.room_scene_entry_frame = false;
  s_observer.reported_room_frame_mismatch = false;
  memset(s_observer.reported_room_scene_hle_fallback, 0,
         sizeof(s_observer.reported_room_scene_hle_fallback));
  s_observer.room_scene_verbose_reports = 0;
  memset(s_observer.room_scene_compared_serial, 0,
         sizeof(s_observer.room_scene_compared_serial));
  memset(s_observer.room_scene_stage_compared, 0,
         sizeof(s_observer.room_scene_stage_compared));
  if (!rom || !rom_size) return false;
  if (!s_observer.room_scene)
    s_observer.room_scene = calloc(1, sizeof(*s_observer.room_scene));
  return s_observer.room_scene != NULL;
}

static void ResetWorlds(void) {
  ResetVoxel();
  s_capture_edit_mask = 0;
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActionBgWorld_Reset(s_observer.provider_world[layer]);
    ActionBgWorld_Reset(s_observer.comparison_world[layer]);
    s_observer.provider_reported_mismatch_serial[layer] = 0;
    s_observer.provider_reported_outside_serial[layer] = 0;
    s_observer.comparison_reported_mismatch_serial[layer] = 0;
    s_observer.comparison_reported_outside_serial[layer] = 0;
    s_observer.room_scene_compared_serial[layer] = 0;
    s_observer.room_scene_stage_compared[layer] = false;
    s_provider[layer].pixel_edits_active = false;
    s_provider[layer].world_apron_available = false;
    s_provider[layer].horizontal_bounds_available = false;
    s_provider[layer].tile_band_count = 0;
    s_provider[layer].tile_band_world = NULL;
    s_provider[layer].tile_band_rules_hash = 0;
    s_provider[layer].tile_band_world_serial = 0;
    s_provider[layer].tile_band_width = 0;
    s_provider[layer].tile_band_height = 0;
    s_provider[layer].tile_band_cache_valid = false;
    s_provider[layer].reported_tile_band_cache_failure = false;
  }
  s_observer.room_scene_valid = false;
  s_observer.room_scene_attempted = false;
  s_observer.room_frame_valid = false;
  s_observer.previous_room_frame_valid = false;
  s_observer.room_frame_uses_previous = false;
  s_observer.room_camera_valid = false;
  s_observer.authentic_camera_valid = false;
  s_observer.capture_live_camera_valid = false;
  s_observer.reported_room_frame_mismatch = false;
  memset(s_observer.reported_room_scene_hle_fallback, 0,
         sizeof(s_observer.reported_room_scene_hle_fallback));
  s_observer.room_scene_verbose_reports = 0;
}

void ActRaiserActionBg_Reset(void) {
  ResetWorlds();
  s_observer.frame_valid = false;
  s_observer.map_valid = false;
  s_observer.forced_blank = false;
}

uint8_t ActRaiserActionBg_TerrainProfile(void) { return s_observer.terrain_profile; }

void ActRaiserActionBg_BeginRoomVariants(uint8_t profile, uint8_t mosaic,
                                         ArRegionalMediaBytes death_heim_characters) {
  s_observer.death_heim_characters=death_heim_characters;
  s_observer.mosaic_pattern = mosaic;
  s_observer.terrain_profile = profile;
  ResetWorlds();
}

static void RecordFallback(ActRaiserActionBgFallbackReason reason,
                           unsigned layer, uint8_t map_group,
                           uint8_t map_number,
                           const ActRaiserActionBgLayerSnapshot *snapshot) {
  if (reason >= kActRaiserActionBgFallback_Count) return;
  s_observer.diagnostics.fallbacks[reason]++;
  if (s_observer.reported_fallback[reason]) return;
  s_observer.reported_fallback[reason] = true;
  fprintf(stderr,
          "[action-bg-hle] fallback=%s map=%02X/%02X BG%u",
          FallbackName(reason), map_group, map_number, layer + 1);
  if (snapshot) {
    fprintf(stderr,
            " size=%ux%u map=$%04X table=$%04X tilemap=$%04X bgsc=$%02X",
            snapshot->decode.world_width, snapshot->decode.world_height,
            snapshot->decode.map_page, snapshot->decode.metatile_table,
            snapshot->tilemap_base, snapshot->bgsc);
  }
  fputc('\n', stderr);
}

/* The optional comparator observes the same preconditions later in the frame.
 * Keep shared fallback counters event-based rather than double-counting one
 * rejected layer merely because both diagnostics are enabled. */
static void RecordProviderFallback(
    ActRaiserActionBgFallbackReason reason, unsigned layer,
    uint8_t map_group, uint8_t map_number,
    const ActRaiserActionBgLayerSnapshot *snapshot) {
  if (!CompareEnabled() ||
      reason == kActRaiserActionBgFallback_ScrollPhase ||
      reason == kActRaiserActionBgFallback_AuthenticEdge ||
      reason == kActRaiserActionBgFallback_CompareFailure)
    RecordFallback(reason, layer, map_group, map_number, snapshot);
}

static ActionBgWorld *WorldForLayer(
    ActionBgWorld **worlds, unsigned layer,
    uint8_t map_group, uint8_t map_number) {
  if (worlds[layer]) return worlds[layer];
  worlds[layer] = ActionBgWorld_Create();
  if (!worlds[layer])
    RecordFallback(kActRaiserActionBgFallback_Allocation, layer,
                   map_group, map_number, NULL);
  return worlds[layer];
}

static bool EnsureRoomScene(uint8_t map_group, uint8_t map_number) {
  if (s_observer.room_scene_attempted)
    return s_observer.room_scene_valid && s_observer.room_scene &&
        s_observer.room_scene->group == map_group &&
        s_observer.room_scene->map == map_number;
  s_observer.room_scene_attempted = true;
  if (!s_observer.room_scene || !s_observer.rom || !s_observer.rom_size ||
      !ActionRoomScene_Load(s_observer.room_scene,
                            s_observer.rom, s_observer.rom_size,
                            map_group, map_number) ||
      (s_observer.terrain_profile &&
       !ActionRoomTerrain_Project(s_observer.room_scene,s_observer.terrain_profile)) ||
      (s_observer.mosaic_pattern &&
       !ActionRoomMosaic_Project(s_observer.room_scene,s_observer.mosaic_pattern))) {
    s_observer.diagnostics.room_scene_load_failures++;
    fprintf(stderr,
            "[action-room-scene] load failed map=%02X/%02X\n",
            map_group, map_number);
    return false;
  }
  const ArRegionalMediaBytes donor=s_observer.death_heim_characters;
  if(donor.data && donor.size==8192 && map_group==7 && (map_number==1 || map_number==8))
    memcpy(s_observer.room_scene->characters+8192,donor.data,8192);
  s_observer.room_scene_valid = true;
  s_observer.diagnostics.room_scene_loads++;
  return true;
}

bool ActRaiserActionBg_CompareRoomSceneLayer(
    const ActionRoomScene *scene, uint8_t bg_layer,
    const ActionBgWorld *world,
    ActRaiserActionRoomSceneCompareResult *result) {
  if (result) {
    memset(result, 0, sizeof(*result));
    result->first_tile_x = -1;
    result->first_tile_y = -1;
  }
  if (!scene || !world || !result || !scene->have_video_profile ||
      !ActionRoomScene_HasBackground(scene, bg_layer) ||
      !ActionBgWorld_IsValid(world))
    return false;
  const unsigned scene_width = ActionRoomScene_TileWidth(scene, bg_layer);
  const unsigned scene_height = ActionRoomScene_TileHeight(scene, bg_layer);
  if (scene_width != ActionBgWorld_TileWidth(world) ||
      scene_height != ActionBgWorld_TileHeight(world))
    return false;

  ActRaiserActionRoomSceneCompareResult built = {
    .first_tile_x = -1,
    .first_tile_y = -1,
  };
  for (unsigned y = 0; y < scene_height; y++) {
    for (unsigned x = 0; x < scene_width; x++) {
      uint16_t immutable = 0, live = 0;
      if (!ActionRoomScene_LookupTile(
              scene, bg_layer, x, y, &immutable, NULL) ||
          ActionBgWorld_Lookup(world, (int)x, (int)y, &live) !=
              kActionBgLookup_Tile)
        return false;
      built.compared++;
      if (immutable == live) continue;
      if (!built.mismatches) {
        built.first_tile_x = (int)x;
        built.first_tile_y = (int)y;
        built.first_immutable = immutable;
        built.first_live = live;
      }
      built.mismatches++;
    }
  }
  *result = built;
  return true;
}

bool ActRaiserActionBg_UpdateWorldFromRoomScene(
    ActionBgWorld *world, const ActionRoomScene *scene, uint8_t bg_layer) {
  if (!world || !scene || bg_layer < 1 ||
      bg_layer > kActionRoomSceneBgCount ||
      !scene->have_video_profile ||
      !ActionRoomScene_HasBackground(scene, bg_layer))
    return false;
  const unsigned tile_width = ActionRoomScene_TileWidth(scene, bg_layer);
  const unsigned tile_height = ActionRoomScene_TileHeight(scene, bg_layer);
  if (tile_width > UINT16_MAX / 8u || tile_height > UINT16_MAX / 8u)
    return false;
  const ActionRoomSceneBg *bg = &scene->bg[bg_layer - 1u];
  const ActionBgImmutableInput input = {
    .map = bg->map,
    .map_size = bg->map_size,
    .metatiles = bg->metatiles,
    .metatile_size = sizeof(bg->metatiles),
    .world_width = (uint16_t)(tile_width * 8u),
    .world_height = (uint16_t)(tile_height * 8u),
    .word_mask = kActionRoomSceneTileWordMask,
    .attributes = (uint8_t)(
        ActionRoomScene_BgAttributes(scene, bg_layer) >> 8),
    .metatile_words_big_endian = true,
  };
  return ActionBgWorld_UpdateImmutable(world, &input);
}

static void CompareRoomFrameValue(
    ActRaiserActionRoomSceneFrameCompareResult *result,
    ActRaiserActionRoomSceneFrameField field,
    uint16_t immutable, uint16_t live) {
  result->compared++;
  if (immutable == live) return;
  if (!result->mismatches) {
    result->first_field = field;
    result->first_immutable = immutable;
    result->first_live = live;
  }
  result->mismatches++;
  result->mismatches_by_field[field]++;
}

bool ActRaiserActionBg_CompareRoomSceneFrameLine(
    const ActionRoomSceneFrameState *state,
    const SrPpuStateSnapshot *ppu,
    unsigned output_y,
    ActRaiserActionRoomSceneFrameCompareResult *result) {
  if (result) {
    memset(result, 0, sizeof(*result));
    result->first_field = kActRaiserActionRoomSceneFrameField_Count;
  }
  if (!state || !ppu || !result ||
      output_y >= kActionRoomSceneFrameHeight)
    return false;

  ActRaiserActionRoomSceneFrameCompareResult built = {
    .first_field = kActRaiserActionRoomSceneFrameField_Count,
  };
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg1HScroll,
      state->bg_hscroll[0][output_y],
      ppu->backgrounds[0].h_scroll & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg1VScroll,
      state->bg_vscroll[0][output_y],
      ppu->backgrounds[0].v_scroll & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg2HScroll,
      state->bg_hscroll[1][output_y],
      ppu->backgrounds[1].h_scroll & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg2VScroll,
      state->bg_vscroll[1][output_y],
      ppu->backgrounds[1].v_scroll & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Mosaic,
      state->mosaic[output_y], ppu->mosaic_control);

  if (output_y == 0) {
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_MainScreen,
        state->screen_enabled[0], ppu->main_screen);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_SubScreen,
        state->screen_enabled[1], ppu->sub_screen);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_MainWindow,
        state->screen_windowed[0], ppu->main_windowed);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_SubWindow,
        state->screen_windowed[1], ppu->sub_windowed);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Cgwsel,
        state->cgwsel, ppu->color_math_control);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Cgadsub,
        state->cgadsub, ppu->color_math_designation);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Bgmode,
        state->bgmode, ppu->bg_mode_control);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Bg1Sc,
        state->bgsc[0], ppu->background_tilemap_control[0]);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Bg2Sc,
        state->bgsc[1], ppu->background_tilemap_control[1]);
  }
  *result = built;
  return true;
}

static void CompareRoomSceneStageOnce(
    const uint8_t *wram, size_t wram_size, unsigned layer,
    uint8_t map_group, uint8_t map_number) {
  if (!RoomSceneStageCompareEnabled() ||
      layer >= kActionBgLayerCount ||
      s_observer.room_scene_stage_compared[layer] ||
      !EnsureRoomScene(map_group, map_number))
    return;
  const uint8_t bg_layer = (uint8_t)(layer + 1u);
  if (!ActionRoomScene_HasBackground(s_observer.room_scene, bg_layer))
    return;
  s_observer.room_scene_stage_compared[layer] = true;

  ActRaiserActionRoomStageCompareResult stage;
  if (ActRaiserActionBg_CompareRoomSceneStage(
          s_observer.room_scene, bg_layer, wram, wram_size, &stage)) {
    static const char *const kStageFields[
        kActRaiserActionRoomStageField_Count] = {
      [kActRaiserActionRoomStageField_Dimensions] = "dimensions",
      [kActRaiserActionRoomStageField_Map] = "map",
      [kActRaiserActionRoomStageField_MetatileDefinitions] = "metatiles",
    };
    s_observer.diagnostics.room_scene_stage_layers_compared++;
    s_observer.diagnostics.room_scene_stage_bytes_compared += stage.compared;
    s_observer.diagnostics.room_scene_stage_mismatches += stage.mismatches;
    if (stage.mismatches) {
      const char *field = stage.first_field <
              kActRaiserActionRoomStageField_Count
          ? kStageFields[stage.first_field] : "unknown";
      fprintf(stderr,
              "[action-room-stage] MISMATCH map=%02X/%02X BG%u "
              "count=%zu/%zu first=%s+$%zX immutable=$%02X live=$%02X\n",
              map_group, map_number, bg_layer, stage.mismatches,
              stage.compared, field, stage.first_offset,
              stage.first_immutable, stage.first_live);
    } else {
      fprintf(stderr,
              "[action-room-stage] match map=%02X/%02X BG%u bytes=%zu\n",
              map_group, map_number, bg_layer, stage.compared);
    }
  } else {
    fprintf(stderr,
            "[action-room-stage] comparison failed map=%02X/%02X BG%u\n",
            map_group, map_number, bg_layer);
  }
}

static void CompareRoomSceneWorld(unsigned layer, uint8_t map_group,
                                  uint8_t map_number,
                                  const ActionBgWorld *world) {
  if (!RoomSceneCompareEnabled() || layer >= kActionBgLayerCount ||
      !world || !ActionBgWorld_IsValid(world))
    return;
  const uint32_t serial = ActionBgWorld_Serial(world);
  if (!serial || s_observer.room_scene_compared_serial[layer] == serial)
    return;
  s_observer.room_scene_compared_serial[layer] = serial;
  if (!EnsureRoomScene(map_group, map_number)) return;

  const uint8_t bg_layer = (uint8_t)(layer + 1);
  ActRaiserActionRoomSceneCompareResult comparison;
  if (!ActRaiserActionBg_CompareRoomSceneLayer(
          s_observer.room_scene, bg_layer, world, &comparison)) {
    fprintf(stderr,
            "[action-room-scene] shape mismatch map=%02X/%02X BG%u "
            "profile=%d immutable=%ux%u live=%ux%u\n",
            map_group, map_number, bg_layer,
            s_observer.room_scene->have_video_profile
                ? s_observer.room_scene->video_profile_index : -1,
            ActionRoomScene_TileWidth(s_observer.room_scene, bg_layer),
            ActionRoomScene_TileHeight(s_observer.room_scene, bg_layer),
            ActionBgWorld_TileWidth(world), ActionBgWorld_TileHeight(world));
    return;
  }
  s_observer.diagnostics.room_scene_layers_compared++;
  s_observer.diagnostics.room_scene_tiles_compared += comparison.compared;
  s_observer.diagnostics.room_scene_mismatches += comparison.mismatches;
  if (comparison.mismatches) {
    fprintf(stderr,
            "[action-room-scene] MISMATCH map=%02X/%02X BG%u "
            "serial=%u count=%zu/%zu first=(%d,%d) immutable=$%04X "
            "live=$%04X\n",
            map_group, map_number, bg_layer, serial,
            comparison.mismatches, comparison.compared,
            comparison.first_tile_x, comparison.first_tile_y,
            comparison.first_immutable, comparison.first_live);
  } else {
    fprintf(stderr,
            "[action-room-scene] match map=%02X/%02X BG%u "
            "profile=$%02X serial=%u tiles=%zu\n",
            map_group, map_number, bg_layer,
            s_observer.room_scene->video_profile_index, serial,
            comparison.compared);
  }
}

/* The room's persistent raster owns its HDMA channel(s) this frame: same
 * channel, mode, target register and WRAM table as the native builder. */
static bool RasterDmaContractMatches(ActionRoomRasterEffect effect) {
  SrDmaStateSnapshot dma;
  if (!QueryDmaState(&dma)) return false;
  uint16_t table = 0x6000;
  uint8_t target = 0x0f;
  uint8_t mode = 2;
  switch (effect) {
    case kActionRoomRaster_Bg2VerticalRipple:
      target = 0x10;
      break;
    case kActionRoomRaster_Bg2MosaicWave:
      target = 0x06;
      mode = 0;
      break;
    case kActionRoomRaster_Bg2AcceleratingWave:
      table = 0x6800;
      break;
    case kActionRoomRaster_Bg2CameraParallaxBands:
      table = 0x7000;
      break;
    default: break;
  }
  const SrDmaChannelState *channel = &dma.channels[2];
  if ((channel->flags & SR_DMA_CHANNEL_HDMA_ACTIVE) == 0u ||
      (channel->flags & SR_DMA_CHANNEL_INDIRECT) != 0u ||
      channel->mode != mode || channel->b_address != target ||
      channel->a_bank != 0x7e || channel->a_address != table)
    return false;
  if (effect == kActionRoomRaster_DualBgOpposedWaves) {
    channel = &dma.channels[3];
    if ((channel->flags & SR_DMA_CHANNEL_HDMA_ACTIVE) == 0u ||
        (channel->flags & SR_DMA_CHANNEL_INDIRECT) != 0u ||
        channel->mode != 2 || channel->b_address != 0x0d ||
        channel->a_bank != 0x7e || channel->a_address != 0x6800)
      return false;
  }
  return true;
}

/* The persistent table scanned this frame was built during the previous
 * frame's update, from that frame's camera. Track it once per game frame. */
static struct {
  uint16_t frame, camera;
  uint16_t previous_frame, previous_camera;
  bool valid, previous_valid;
} s_raster_camera;

static int32_t RasterTableCamera(const uint8_t *wram) {
  const uint16_t frame = ReadWram16(wram, kActRaiserWram_GameFrame);
  const uint16_t camera = ReadWram16(wram, kActRaiserWram_Bg1CameraX);
  if (!s_raster_camera.valid || s_raster_camera.frame != frame) {
    s_raster_camera.previous_frame = s_raster_camera.frame;
    s_raster_camera.previous_camera = s_raster_camera.camera;
    s_raster_camera.previous_valid = s_raster_camera.valid;
    s_raster_camera.frame = frame;
    s_raster_camera.camera = camera;
    s_raster_camera.valid = true;
  }
  return s_raster_camera.previous_valid &&
      s_raster_camera.previous_frame == (uint16_t)(frame - 1u)
      ? s_raster_camera.previous_camera : camera;
}

bool ActRaiserActionBg_LiveRasterCameraSensitivity(
    const uint8_t *wram, size_t wram_size, float rows[kActionRoomSceneFrameHeight],
    float subtexel[kActionRoomSceneFrameHeight], int32_t *raster_camera_x) {
  if (!wram || wram_size <= kActRaiserWram_GameFrame + 1u || !rows || !subtexel)
    return false;
  const int32_t table_camera = RasterTableCamera(wram);
  if (raster_camera_x) *raster_camera_x = table_camera;
  const uint8_t group = wram[kActRaiserWram_MapGroup];
  const uint8_t map = wram[kActRaiserWram_CurrentMap];
  if (!ActRaiser_IsActionMapGroup(group) || !EnsureRoomScene(group, map) ||
      s_observer.room_scene->raster_effect == kActionRoomRaster_None ||
      !RasterDmaContractMatches(s_observer.room_scene->raster_effect))
    return false;
  const ActionRoomSceneFrameRequest request = {
    .camera_x = ReadWram16(wram, kActRaiserWram_Bg1CameraX),
    .camera_y = ReadWram16(wram, kActRaiserWram_Bg1CameraY),
    .raster_camera_x = table_camera,
    .game_frame = ReadWram16(wram, kActRaiserWram_GameFrame),
    .animation_phase = -1,
    .page_phase = -1,
    .have_raster_camera_x = true,
  };
  return ActionRoomScene_RasterCameraSensitivity(s_observer.room_scene, &request, rows) &&
      ActionRoomScene_RasterSubtexel(s_observer.room_scene, &request, rows, subtexel);
}

static bool SyncFrameIdentity(const uint8_t *wram, size_t wram_size) {
  if (!wram || wram_size < kActRaiserWram_GameFrame + 2)
    return false;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  if (!ActRaiser_IsActionMapGroup(map_group)) {
    if (s_observer.map_valid) ActRaiserActionBg_Reset();
    return false;
  }
  const uint16_t game_frame = ReadWram16(wram, kActRaiserWram_GameFrame);
  const bool backwards = s_observer.frame_valid &&
      game_frame < s_observer.last_game_frame &&
      !(s_observer.last_game_frame == UINT16_MAX && game_frame == 0);
  const bool new_map = !s_observer.map_valid ||
      s_observer.map_group != map_group ||
      s_observer.map_number != map_number;
  if (new_map || backwards) ResetWorlds();
  if (new_map) s_observer.room_scene_entry_frame = true;
  s_observer.map_group = map_group;
  s_observer.map_number = map_number;
  s_observer.map_valid = true;
  s_observer.last_game_frame = game_frame;
  s_observer.frame_valid = true;
  return true;
}

static uint64_t VoxelHash(uint64_t key, const void *data, size_t bytes) {
  const uint8_t *p = data;
  for (size_t i = 0; i < bytes; ++i) key = (key ^ p[i]) * UINT64_C(1099511628211);
  return key;
}
static bool VoxelCopy(void *dst, const void *src, size_t size) {
  if (!memcmp(dst, src, size)) return false;
  memcpy(dst, src, size);
  return true;
}
static bool CaptureMasksSupported(void) {
  return s_runner_api && s_runner_api->struct_size >= SNES_RUNNER_API_PPU_CAPTURE_TILES_SIZE &&
      (s_runner_api->capabilities & SR_RUNNER_CAP_PPU_CAPTURE_TILES) &&
      (s_runner_api->capabilities & SR_RUNNER_CAP_PPU_CAPTURE_TRANSPARENCY) &&
      s_runner_api->replace_ppu_capture_tiles;
}

static bool VoxelSourceLayout(ActRaiserActionBgLayerSnapshot *live,
                              unsigned group, unsigned room, size_t vram_words) {
  if (ActRaiserActionBg_WorldRingEligible(live,vram_words)) return true;
  /* Resident pages have fixed local coordinates; their raster scroll is
   * presentation, not a rolling world-map publication. Death Heim's hub
   * stores its separate completion page immediately after the faces page. */
  const unsigned layout=live->bgsc & 3u;
  const bool repeated_celestial=layout==2 && s_observer.room_scene &&
      s_observer.room_scene->raster_effect==kActionRoomRaster_DualBgOpposedWaves;
  if ((layout && !repeated_celestial) || live->decode.world_height != 256) return false;
  const size_t base=(size_t)(live->bgsc & 0xfcu)<<8;
  if (base>vram_words || 1024>vram_words-base) return false;
  if (group==kActRaiserMapGroup_DeathHeim && room==kActRaiserDeathHeimMap_Hub) {
    if (live->decode.world_width!=512 || (live->bgsc & 0xfcu)!=0x60) return false;
    live->decode.world_width=256;
  } else if (live->decode.world_width!=256) return false;
  return true;
}

void ActRaiserActionBg_PrepareVoxel(bool enabled, const DioramaRoomOverride *terrain) {
  const unsigned group = g_ram[kActRaiserWram_MapGroup], room = g_ram[kActRaiserWram_CurrentMap];
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram, palette = {.struct_size = sizeof(palette)};
  ActRaiserActionBgLayerSnapshot live;
  ActionBgMapView map;
  if (!enabled || !ActionVoxel_Room(group, room) ||
      (terrain && (terrain->voxel.cell_count > kDioramaVoxelCellMax ||
                   terrain->voxel.object_count > kDioramaVoxelObjectMax)) ||
      !SyncFrameIdentity(g_ram, kActRaiserWramSize) || !EnsureRoomScene(group, room) ||
      !QueryPpuState(&ppu) || ppu.bg_mode != 1 || (ppu.flags & SR_PPU_STATE_FORCED_BLANK) ||
      ActRaiser_IsDeathHeimCompletionScene(group, room,
          g_ram[kActRaiserWram_DeathHeimProgress],
          ppu.background_tilemap_control[0], ppu.background_tilemap_control[1]) ||
      !BorrowVram(ppu.lifetime_generation, &vram) ||
      s_runner_api->borrow_u16_memory(s_runner, SR_MEMORY_CGRAM, &palette) != SR_RESULT_OK ||
      !palette.data || palette.element_count < 128 || palette.lifetime_generation != ppu.lifetime_generation ||
      !ActRaiserActionBg_CaptureLayer(g_ram, kActRaiserWramSize, 0, ppu.background_tilemap_control[0], &live) ||
      !VoxelSourceLayout(&live, group, room, vram.element_count) ||
      !ActionBgMapView_Init(&map, g_ram, kActRaiserWramSize, live.decode.world_width,
                            live.decode.world_height, live.decode.map_page) ||
      map.map_size > kActionRoomSceneMaxMapBytes ||
      !ActionVoxel_ValidBounds(0, 0, live.decode.world_width, live.decode.world_height) ||
      live.decode.word_mask != kActionRoomSceneTileWordMask ||
      (live.decode.attributes & ~0x20u) != kActionRoomSceneBg1AttributeByte) {
    ResetVoxel();
    return;
  }
  if (!s_voxel.source) s_voxel.source = calloc(1, sizeof(*s_voxel.source));
  if (!s_voxel.tiles) s_voxel.tiles = calloc(kActionRoomSceneMaxMapBytes * 4, sizeof(*s_voxel.tiles));
  if (!s_voxel.source || !s_voxel.tiles) { ResetVoxel(); return; }
  ActionRoomScene *source = s_voxel.source;
  const bool new_room = !s_voxel.source_valid || source->group != group || source->map != room;
  if(new_room)ResetVoxelCastlePhases();
  if (new_room) *source = *s_observer.room_scene;
  ActionRoomSceneBg *bg = &source->bg[0];
  bool layout_changed = new_room || bg->map_size != map.map_size ||
      bg->pages_wide != map.pages_wide || bg->pages_high != live.decode.world_height / 256;
  bg->map_size = map.map_size; bg->pages_wide = map.pages_wide;
  bg->pages_high = live.decode.world_height / 256;
  bg->have_map = bg->have_metatiles = true;
  layout_changed |= VoxelCopy(bg->map, map.map, map.map_size);
  for (unsigned i = 0; i < kActionRoomSceneMetatileBytes; ++i) {
    const uint8_t byte = g_ram[live.decode.metatile_table + (i ^ 1u)];
    layout_changed |= bg->metatiles[i] != byte;
    bg->metatiles[i] = byte;
  }
  const uint8_t priority = (live.decode.attributes & 0x20u) ? 1 : 0;
  layout_changed |= (source->video_profile[4] & 1u) != priority;
  source->video_profile[4] = (source->video_profile[4] & ~1u) | priority;
  source->have_video_profile = true;
  bool changed = layout_changed;
  if (ActionVoxel_BackgroundWater(ActionVoxel_ProfileForRoom(group,room))) {
    /* Pool topology follows the live BG2 map, but its animation stays in the
     * captured texture. An ordinary animation tick must not rebuild geometry. */
    ActRaiserActionBgLayerSnapshot water_live;
    ActionBgMapView water_map;
    if (!ActRaiserActionBg_CaptureLayer(g_ram,kActRaiserWramSize,1,ppu.background_tilemap_control[1],&water_live) ||
        !ActionBgMapView_Init(&water_map,g_ram,kActRaiserWramSize,water_live.decode.world_width,
                              water_live.decode.world_height,water_live.decode.map_page) ||
        water_map.map_size > kActionRoomSceneMaxMapBytes ||
        !ActionVoxel_ValidBounds(0,0,water_live.decode.world_width,water_live.decode.world_height) ||
        water_live.decode.word_mask != kActionRoomSceneTileWordMask ||
        (water_live.decode.attributes & ~0x20u) != kActionRoomSceneBg2AttributeByte) {
      ResetVoxel(); return;
    }
    ActionRoomSceneBg *water = &source->bg[1];
    changed |= water->map_size != water_map.map_size || water->pages_wide != water_map.pages_wide ||
        water->pages_high != water_live.decode.world_height/256;
    water->map_size = water_map.map_size; water->pages_wide = water_map.pages_wide;
    water->pages_high = water_live.decode.world_height/256;
    water->have_map = water->have_metatiles = true;
    changed |= VoxelCopy(water->map,water_map.map,water_map.map_size);
    for (unsigned i = 0; i < kActionRoomSceneMetatileBytes; ++i) {
      const uint8_t byte = g_ram[water_live.decode.metatile_table+(i^1u)];
      changed |= water->metatiles[i] != byte; water->metatiles[i] = byte;
    }
    const uint8_t high = (water_live.decode.attributes & 0x20u) ? 2 : 0;
    changed |= (source->video_profile[4]&2u) != high;
    source->video_profile[4] = (source->video_profile[4]&~2u)|high;
  }
  /* BG2 animation must not rebuild the castle. Capture only character slots
   * referenced by BG1 definitions or authored terrain, with bounds checks. */
  uint64_t key = UINT64_C(14695981039346656037);
  const unsigned terrain_layers = ActionVoxel_BackgroundWater(ActionVoxel_ProfileForRoom(group,room)) ? 2 : 1;
  if (terrain) for (unsigned layer = 0; layer < terrain_layers; ++layer) {
    const DioramaStampLayerOverride *stamps = &terrain->stamp_layers[layer];
    key = VoxelHash(key, &stamps->set_bounds, sizeof(stamps->set_bounds));
    if (stamps->set_bounds) {
      const int16_t bounds[] = {stamps->x0, stamps->y0, stamps->x1, stamps->y1};
      key = VoxelHash(key, bounds, sizeof(bounds));
    }
    key = VoxelHash(key, stamps->cells, stamps->count * sizeof(*stamps->cells));
    key = VoxelHash(key, terrain->pixel_layers[layer].edits,
                     terrain->pixel_layers[layer].count * sizeof(DioramaPixelEdit));
  }
  if (terrain && (terrain->voxel.cell_count || terrain->voxel.object_count))
    key = DioramaVoxelEdits_Hash(key, &terrain->voxel);
  const bool terrain_changed = !s_voxel.terrain_valid || key != s_voxel.terrain_key;
  changed |= terrain_changed;
  const DioramaStampLayerOverride *stamps = terrain ? &terrain->stamp_layers[0] : NULL;
  const int authored_bounds[] = {
      stamps && stamps->set_bounds ? stamps->x0 : 0,
      stamps && stamps->set_bounds ? stamps->y0 : 0,
      stamps && stamps->set_bounds ? stamps->x1 : bg->pages_wide * 16,
      stamps && stamps->set_bounds ? stamps->y1 : bg->pages_high * 16};
  if (new_room || terrain_changed ||
      memcmp(authored_bounds, s_voxel.authored_bounds, sizeof(authored_bounds))) {
    /* Region history is not visible geometry. Resolve the same last-covering
     * owner grid as the compiler once per edit/bounds change, so an older
     * animated donor hidden by a later reset cannot force per-frame rebuilds.
     * No owner grid is allocated or scanned on ordinary animation frames. */
    memset(s_voxel.authored_records, 0, sizeof(s_voxel.authored_records));
    if (terrain && terrain->voxel.cell_count) {
      const int columns = authored_bounds[2] - authored_bounds[0];
      const int rows = authored_bounds[3] - authored_bounds[1];
      uint16_t *owners = ActionVoxel_RecordOwners(&terrain->voxel,
          authored_bounds[0], authored_bounds[1], columns, rows);
      if (!owners) { ResetVoxel(); return; }
      for (size_t i = 0; i < (size_t)columns * rows; ++i)
        if (owners[i] && ActionVoxel_CellOwnsArtwork(&terrain->voxel.cells[owners[i] - 1]))
          s_voxel.authored_records[owners[i] - 1] = true;
      free(owners);
    }
    memcpy(s_voxel.authored_bounds, authored_bounds, sizeof(authored_bounds));
  }
  bool used[kActionRoomSceneCharacterBytes / 32] = {false};
  bool extra_used[kActionRoomSceneExtraCharacterBytes / 32] = {false};
  bool authored_animation = false;
  size_t animation_offset = 0, animation_bytes = 0;
  const ActionRoomScene *reference = s_observer.room_scene;
  const unsigned phase_count=ActionRoomScene_CharacterAnimationPhaseCount(reference);
  const bool castle_animation=ActionVoxel_ProfileForRoom(group,room)==kActionVoxelProfile_Castle &&
      phase_count>0 && phase_count<=4;
  const bool have_animation = (castle_animation ||
      ActionVoxel_AnimationCacheCandidate(ActionVoxel_ProfileForRoom(group,room))) &&
      ActionVoxel_AnimationMatches(reference,reference->characters,&animation_offset,&animation_bytes);
  /* Read the complete window so a partially published or edited phase cannot
   * masquerade as a known native frame. Unrelated BG2 characters stay out. */
  if (have_animation) for (size_t at = animation_offset; at < animation_offset+animation_bytes; at += 32)
    used[at/32] = true;
  for (unsigned i = 0; i < kActionRoomSceneMetatileBytes; i += 2)
    used[bg->metatiles[i + 1]] = true;
  if (terrain) for (size_t i = 0; i < terrain->stamp_layers[0].count; ++i)
    for (unsigned q = 0; q < 4; ++q) {
      const unsigned character = terrain->stamp_layers[0].cells[i].words[q] & 1023;
      if (character < sizeof(used) / sizeof(*used)) used[character] = true;
    }
  if (terrain) for (unsigned i = 0; i < terrain->voxel.cell_count; ++i) {
    if (!s_voxel.authored_records[i]) continue;
    const DioramaVoxelCell *cell = &terrain->voxel.cells[i];
    const DioramaVoxelArt *arts[] = {&cell->backing_art, &cell->top_art, &cell->side_art,
        &cell->front_art, &cell->left_art, &cell->right_art, &cell->bottom_art};
    for (unsigned a = 0; a < sizeof(arts) / sizeof(*arts); ++a) {
      if (!(cell->fields & (kDioramaVoxelCell_BackArt << a)) || !arts[a]->set) continue;
      for (unsigned q = 0; q < 4; ++q) {
        const unsigned ch = arts[a]->words[q] & 1023u;
        if (ch < sizeof(used) / sizeof(*used)) {
          used[ch] = true;
          authored_animation |= have_animation && ch * 32 >= animation_offset &&
              ch * 32 < animation_offset + animation_bytes;
        } else if (ch >= 0x200 && ch < 0x300) extra_used[ch - 0x200] = true;
      }
    }
  }
  bool animation_changed = false;
  for (unsigned ch = 0; ch < sizeof(used) / sizeof(*used); ++ch) if (used[ch]) {
    for (unsigned byte = 0; byte < 32; ++byte) {
      const size_t word = ppu.backgrounds[0].tile_base_word + ch * 16 + byte / 2;
      if (word >= vram.element_count) { ResetVoxel(); return; }
      const uint8_t value = vram.data[word] >> ((byte & 1) * 8);
      const bool different = source->characters[ch * 32 + byte] != value;
      if (have_animation && ch*32 >= animation_offset && ch*32 < animation_offset+animation_bytes)
        animation_changed |= different;
      else changed |= different;
      source->characters[ch * 32 + byte] = value;
    }
  }
  /* The scene codec retains the native supplementary upload at byte $6000.
   * Its authored word range is $200-$2FF (the editor normalizes its binding).
   * Only selected donor characters are read, so unchanged rooms retain the
   * existing character-copy cost and animation cache behavior. */
  for (unsigned ch = 0; ch < sizeof(extra_used) / sizeof(*extra_used); ++ch) if (extra_used[ch]) {
    for (unsigned byte = 0; byte < 32; ++byte) {
      const size_t word = 0x3000u + ch * 16 + byte / 2;
      if (word >= vram.element_count) { ResetVoxel(); return; }
      const uint8_t value = vram.data[word] >> ((byte & 1) * 8);
      changed |= source->extra_characters[ch * 32 + byte] != value;
      source->extra_characters[ch * 32 + byte] = value;
    }
    source->have_extra_characters = true;
  }
  for (unsigned i = 0; i < 256; ++i) {
    const uint8_t value = palette.data[i / 2] >> ((i & 1) * 8);
    changed |= source->palette[i] != value;
    source->palette[i] = value;
  }
  source->have_palette = source->have_character_bank[0] = true;
  const unsigned width = bg->pages_wide * 32, height = bg->pages_high * 32;
  if (layout_changed) for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x)
      ActionRoomScene_LookupTile(source, 1, x, y, &s_voxel.tiles[y * width + x], NULL);
  /* Keep observed native door patches outside the viewport until their next
   * publication. Camera motion alone does not invalidate geometry. */
  if ((live.bgsc & 3u)!=3) {
    const size_t base=(size_t)(live.bgsc & 0xfcu)<<8;
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
      const uint16_t word=vram.data[base+(size_t)y*32+x];
      changed |= s_voxel.tiles[y*width+x]!=word;
      s_voxel.tiles[y*width+x]=word;
    }
  } else if ((live.camera_x & 1023) == (ppu.backgrounds[0].h_scroll & 1023) &&
      (live.camera_y & 1023) == (ppu.backgrounds[0].v_scroll & 1023)) {
    const unsigned last_x = (live.camera_x + 255u) / 8, last_y = (live.camera_y + 224u) / 8;
    for (unsigned y = (live.camera_y + 1u) / 8; y <= last_y && y < height; ++y)
      for (unsigned x = live.camera_x / 8; x <= last_x && x < width; ++x) {
        size_t address;
        if (!ActRaiserActionBg_RingAddress(live.tilemap_base, x, y, vram.element_count, &address)) continue;
        changed |= s_voxel.tiles[y * width + x] != vram.data[address];
        s_voxel.tiles[y * width + x] = vram.data[address];
      }
  }
  s_voxel.source_valid = s_voxel.terrain_valid = true;
  s_voxel.terrain_key = key;
  const bool known_animation = have_animation && ActionVoxel_AnimationMatches(reference,source->characters,
                                                                              &animation_offset,&animation_bytes);
  int castle_phase=-1;
  if(castle_animation) {
    if(known_animation)for(unsigned phase=0;phase<phase_count;++phase)
      if(!memcmp(source->characters+animation_offset,
          reference->characters+animation_offset+phase*animation_bytes,animation_bytes)) {
        castle_phase=(int)phase;break;
      }
    /* Every input outside the exact native animation window invalidates all
     * phases, including live door words, palette, bounds and authored edits.
     * Unknown/partial DMA frames never borrow a known phase's geometry. */
    if(changed || castle_phase<0)ResetVoxelCastlePhases();
    s_voxel.animation_safe=false;
    if(castle_phase>=0 && s_voxel.castle_phases[castle_phase]) {
      const ActionVoxelGeometry *cached=s_voxel.castle_phases[castle_phase];
      if(ActionVoxel_HasCutouts(cached) && !CaptureMasksSupported()) {
        ResetVoxel();return;
      }
      ActionVoxel_Retain(cached);
      ActionVoxel_Release(s_voxel.geometry);
      s_voxel.geometry=cached;
      return;
    }
    changed |= animation_changed || castle_phase>=0;
  } else if (changed || !known_animation) s_voxel.animation_safe = false;
  /* Authored faces own decoded pixels. Unlike a live captured face, those
   * pixels cannot borrow a canonical animation frame without freezing art. */
  if (authored_animation) s_voxel.animation_safe = false;
  if (!castle_animation && !authored_animation && known_animation &&
      !s_voxel.animation_safe && (changed || animation_changed)) {
    s_voxel.animation_safe = ActionVoxel_AnimationIsPresentationOnly(source,reference,terrain,s_voxel.tiles,width);
    changed |= s_voxel.animation_safe; /* Replace geometry compiled from an unknown frame. */
  }
  if (s_voxel.animation_safe) {
    /* Only the geometry compiler sees canonical art. Native texture capture
     * and emulation retain the current texture pixels and cadence. */
    memcpy(source->characters+animation_offset,reference->characters+animation_offset,animation_bytes);
  } else changed |= animation_changed;
  if (!changed) return;
  ActionVoxelGeometry *next = ActionVoxel_Create();
  if (!next || !ActionVoxel_BuildWithTiles(next, source, terrain, ActionVoxel_SettingsForRoom(group, room),
                                            s_voxel.tiles, width) ||
      (ActionVoxel_HasCutouts(next) && !CaptureMasksSupported())) {
    ActionVoxel_Release(next);
    next = NULL;
  }
  if(castle_phase>=0 && ActionVoxel_Active(next,group,room))
    s_voxel.castle_phases[castle_phase]=ActionVoxel_Retain(next);
  else if(castle_animation)ResetVoxelCastlePhases();
  /* Failure/unknown art falls back to ordinary Diorama for this frame; never
   * pair an earlier door's geometry with newly captured native pixels. */
  ActionVoxel_Release(s_voxel.geometry);
  s_voxel.geometry = next;
}

static uint16_t ResolveAuthenticActionCameraX(
    const uint8_t *wram, size_t wram_size, uint16_t live_camera_x) {
  if (!wram || wram_size < kActRaiserWramSize)
    return live_camera_x;
  uint16_t subject_x;
  const uint16_t player_handler =
      ReadWram16(wram, kActRaiserWram_PlayerHandler);
  if (ActRaiser_PlayerArrivalAnimationActive(player_handler)) {
    subject_x = ReadWram16(wram, kActRaiserWram_PlayerPositionX);
  } else {
    const uint16_t subject =
        ReadWram16(wram, kActRaiserWram_ActionCameraSubject);
    const uint16_t table_end = (uint16_t)(
        kActRaiserWram_ActionObjectTable +
        kActRaiserActionObjectCount * kActRaiserActionObjectStride);
    if (subject < kActRaiserWram_ActionObjectTable || subject >= table_end ||
        (subject - kActRaiserWram_ActionObjectTable) %
            kActRaiserActionObjectStride != 0) {
      /* Match the authentic activation gate: a transient/invalid subject
       * leaves the live game camera authoritative rather than snapping to a
       * different actor for one comparison frame. */
      return live_camera_x;
    } else {
      subject_x = ReadWram16(
          wram, (size_t)subject + kActRaiserActionObject_WorldX);
    }
  }
  return ActRaiser_AuthenticActionCameraX(
      subject_x, ReadWram16(wram, kActRaiserWram_Bg1Width));
}

void ActRaiserActionBg_BeginRoomSceneFrame(
    const uint8_t *wram, size_t wram_size) {
  SrPpuStateSnapshot ppu;
  s_observer.room_frame_valid = false;
  s_observer.room_frame_uses_previous = false;
  s_observer.room_frame_raster_matches_current = true;
  if (!QueryPpuState(&ppu)) return;
  const SrPpuAuthenticCameraRequest clear_request = {
    .struct_size = sizeof(clear_request),
    .flags = SR_PPU_AUTHENTIC_CAMERA_CLEAR,
    .lifetime_generation = ppu.lifetime_generation,
  };
  (void)s_runner_api->update_ppu_authentic_camera(
      s_runner, &clear_request);
  const bool compare_room_frame = RoomSceneCompareEnabled();
  const bool capture_authentic =
      (ppu.renderer_flags & SR_PPU_RENDERER_AUTHENTIC_SURFACE_BOUND) != 0u;
  if (!capture_authentic) {
    s_observer.authentic_camera_valid = false;
    s_observer.capture_live_camera_valid = false;
  }
  if ((!compare_room_frame && !capture_authentic) || !wram ||
      (ppu.flags & SR_PPU_STATE_FORCED_BLANK) != 0u || ppu.bg_mode != 1u ||
      wram_size <= kActRaiserWram_Bg1CameraY + 1u ||
      !SyncFrameIdentity(wram, wram_size))
    return;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  const uint16_t camera_x =
      ReadWram16(wram, kActRaiserWram_Bg1CameraX);
  const uint16_t authentic_camera_x =
      ResolveAuthenticActionCameraX(wram, wram_size, camera_x);
  const int background_delta =
      (int)authentic_camera_x - (int)camera_x;
  const int object_delta = -background_delta;
  if (!EnsureRoomScene(map_group, map_number)) return;

  const uint16_t raster_camera_x = s_observer.room_camera_valid
      ? s_observer.last_room_camera_x : camera_x;
  ActionRoomSceneFrameRequest request = {
    .camera_x = camera_x,
    .camera_y = ReadWram16(wram, kActRaiserWram_Bg1CameraY),
    .raster_camera_x = raster_camera_x,
    .game_frame = ReadWram16(wram, kActRaiserWram_GameFrame),
    .animation_phase = -1,
    .page_phase = -1,
    .have_raster_camera_x = true,
    .raster_entry_frame = s_observer.room_scene_entry_frame,
  };
  /* `$00:F5F0-$F619` replaces the Death Heim hub's BG pages during the
   * post-final-boss black frame. Progress makes the override eligible; the
   * live page bases are the script's authoritative phase signal. */
  if (map_group == kActRaiserMapGroup_DeathHeim &&
      map_number == kActRaiserDeathHeimMap_Hub &&
      wram[kActRaiserWram_DeathHeimProgress] >=
          kActRaiserDeathHeimProgress_FinalBossBeaten) {
    if ((ppu.background_tilemap_control[0] & 0xfcu) == 0x64u) {
      request.bgsc_override_mask |= 1u << 0;
      request.bgsc_override[0] = ppu.background_tilemap_control[0];
    }
    if ((ppu.background_tilemap_control[1] & 0xfcu) == 0x74u) {
      request.bgsc_override_mask |= 1u << 1;
      request.bgsc_override[1] = ppu.background_tilemap_control[1];
    }
  }

  bool authentic_reference_ready = false;
  if (capture_authentic) {
    ActionRoomSceneFrameRequest live_reference_request = request;
    live_reference_request.raster_camera_x =
        s_observer.capture_live_camera_valid
        ? s_observer.last_capture_live_camera_x : camera_x;
    ActionRoomSceneFrameRequest authentic_request = request;
    authentic_request.camera_x = authentic_camera_x;
    authentic_request.raster_camera_x = s_observer.authentic_camera_valid
        ? s_observer.last_authentic_camera_x : authentic_camera_x;
    ActionRoomSceneFrameState live_reference;
    if (ActionRoomScene_BuildFrameState(
            s_observer.room_scene, &live_reference_request,
            &live_reference) &&
        ActionRoomScene_BuildFrameState(
            s_observer.room_scene, &authentic_request,
            &s_observer.authentic_room_frame)) {
      authentic_reference_ready = true;
    }
  }

  /* Loading/fade frames can already carry the destination room identity while
   * another effect still owns channel 2. Match the complete raster DMA
   * contract—not only HDMAEN—before publishing an exact state. Until then the
   * authentic comparison frame is unavailable; no approximate camera state is
   * installed. This also pins the non-default R7 `$6800` and R9 `$7000` table
   * bases. */
  if (s_observer.room_scene->raster_effect != kActionRoomRaster_None &&
      !RasterDmaContractMatches(s_observer.room_scene->raster_effect))
    return;
  bool built_any = false;
  if (compare_room_frame && ActionRoomScene_BuildFrameState(
          s_observer.room_scene, &request, &s_observer.room_frame)) {
    s_observer.room_frame_current_camera_x = camera_x;
    s_observer.room_frame_raster_camera_x = raster_camera_x;
    s_observer.room_frame_prior_camera_x = s_observer.room_camera_valid
        ? s_observer.prior_room_camera_x : camera_x;
    s_observer.prior_room_camera_x = s_observer.last_room_camera_x;
    s_observer.last_room_camera_x = camera_x;
    s_observer.room_camera_valid = true;
    s_observer.room_frame_valid = true;
    s_observer.diagnostics.room_scene_frames_built++;
    built_any = true;
  }
  if (capture_authentic && authentic_reference_ready) {
    const SrPpuAuthenticCameraRequest camera_request = {
      .struct_size = sizeof(camera_request),
      .lifetime_generation = ppu.lifetime_generation,
      .layer_mask = SR_PPU_AUTHENTIC_CAMERA_ALL,
      .row_count = SR_PPU_NATIVE_HEIGHT,
      .bg1_hscroll = s_observer.authentic_room_frame.bg_hscroll[0],
      .bg2_hscroll = s_observer.authentic_room_frame.bg_hscroll[1],
      .object_offset_x = object_delta,
    };
    if (s_runner_api->update_ppu_authentic_camera(
            s_runner, &camera_request) == SR_RESULT_OK) {
      s_observer.last_capture_live_camera_x = camera_x;
      s_observer.capture_live_camera_valid = true;
      s_observer.last_authentic_camera_x = authentic_camera_x;
      s_observer.authentic_camera_valid = true;
      built_any = true;
    }
  }
  if (built_any) s_observer.room_scene_entry_frame = false;
}

void ActRaiserActionBg_ObserveRoomSceneFrameLine(
    const SrPpuStateSnapshot *ppu, unsigned output_y) {
  if (!s_observer.room_frame_valid || !ppu) return;
  ActRaiserActionRoomSceneFrameCompareResult comparison;
  const ActionRoomSceneFrameState *compared_frame =
      s_observer.room_frame_uses_previous
      ? &s_observer.previous_room_frame : &s_observer.room_frame;
  if (!ActRaiserActionBg_CompareRoomSceneFrameLine(
          compared_frame, ppu, output_y, &comparison))
    return;
  size_t current_raster_mismatches = 0;
  for (unsigned field = kActRaiserActionRoomSceneFrameField_Bg1HScroll;
       field <= kActRaiserActionRoomSceneFrameField_Mosaic; field++)
    current_raster_mismatches += comparison.mismatches_by_field[field];
  if (current_raster_mismatches)
    s_observer.room_frame_raster_matches_current = false;
  /* The raster callback is an action-update callback, not a presentation
   * callback. Hit-stop, fades, and other skipped action updates intentionally
   * leave the persistent HDMA table untouched. If the newly predicted table
   * disagrees but the last fully accepted table agrees, follow that retained
   * table for this scanout; it remains eligible for any later skipped frames. */
  if (comparison.mismatches && !s_observer.room_frame_uses_previous &&
      s_observer.previous_room_frame_valid) {
    ActRaiserActionRoomSceneFrameCompareResult held_comparison;
    if (ActRaiserActionBg_CompareRoomSceneFrameLine(
            &s_observer.previous_room_frame, ppu, output_y,
            &held_comparison) && !held_comparison.mismatches) {
      comparison = held_comparison;
      s_observer.room_frame_uses_previous = true;
      s_observer.diagnostics.room_scene_raster_hold_frames++;
      if (RoomSceneCompareVerbose()) {
        fprintf(stderr,
                "[action-room-scene] raster-hold map=%02X/%02X frame=%u "
                "row=%u previous-frame=%u "
                "camera={current:$%04X,raster:$%04X,prior:$%04X}\n",
                s_observer.map_group, s_observer.map_number,
                (unsigned)s_observer.room_frame.game_frame, output_y,
                (unsigned)s_observer.previous_room_frame.game_frame,
                s_observer.room_frame_current_camera_x,
                s_observer.room_frame_raster_camera_x,
                s_observer.room_frame_prior_camera_x);
      }
    }
  }
  /* Profile/page registers can legitimately change while the raster callback
   * still advances. Keep the last table whose five raster fields matched so a
   * later multi-frame gameplay pause can retain it indefinitely. */
  if (output_y + 1u == kActionRoomSceneFrameHeight &&
      s_observer.room_frame_raster_matches_current &&
      !s_observer.room_frame_uses_previous) {
    s_observer.previous_room_frame = s_observer.room_frame;
    s_observer.previous_room_frame_valid = true;
  }
  s_observer.diagnostics.room_scene_scanlines_compared++;
  s_observer.diagnostics.room_scene_registers_compared += comparison.compared;
  s_observer.diagnostics.room_scene_register_mismatches +=
      comparison.mismatches;
  for (unsigned field = 0;
       field < kActRaiserActionRoomSceneFrameField_Count; field++)
    s_observer.room_scene_register_mismatches_by_field[field] +=
        comparison.mismatches_by_field[field];
  if (!comparison.mismatches) return;
  const bool verbose = RoomSceneCompareVerbose();
  if ((s_observer.reported_room_frame_mismatch && !verbose) ||
      s_observer.room_scene_verbose_reports >= 128)
    return;
  static const char *const names[
      kActRaiserActionRoomSceneFrameField_Count] = {
    [kActRaiserActionRoomSceneFrameField_Bg1HScroll] = "BG1HOFS",
    [kActRaiserActionRoomSceneFrameField_Bg1VScroll] = "BG1VOFS",
    [kActRaiserActionRoomSceneFrameField_Bg2HScroll] = "BG2HOFS",
    [kActRaiserActionRoomSceneFrameField_Bg2VScroll] = "BG2VOFS",
    [kActRaiserActionRoomSceneFrameField_Mosaic] = "MOSAIC",
    [kActRaiserActionRoomSceneFrameField_MainScreen] = "TM",
    [kActRaiserActionRoomSceneFrameField_SubScreen] = "TS",
    [kActRaiserActionRoomSceneFrameField_MainWindow] = "TMW",
    [kActRaiserActionRoomSceneFrameField_SubWindow] = "TSW",
    [kActRaiserActionRoomSceneFrameField_Cgwsel] = "CGWSEL",
    [kActRaiserActionRoomSceneFrameField_Cgadsub] = "CGADSUB",
    [kActRaiserActionRoomSceneFrameField_Bgmode] = "BGMODE",
    [kActRaiserActionRoomSceneFrameField_Bg1Sc] = "BG1SC",
    [kActRaiserActionRoomSceneFrameField_Bg2Sc] = "BG2SC",
  };
  const char *field = comparison.first_field <
      kActRaiserActionRoomSceneFrameField_Count
      ? names[comparison.first_field] : "unknown";
  fprintf(stderr,
          "[action-room-scene] FRAME-MISMATCH map=%02X/%02X frame=%u "
          "row=%u field=%s immutable=$%04X live=$%04X "
          "camera={current:$%04X,raster:$%04X,prior:$%04X}\n",
          s_observer.map_group, s_observer.map_number,
          (unsigned)s_observer.room_frame.game_frame, output_y, field,
          comparison.first_immutable, comparison.first_live,
          s_observer.room_frame_current_camera_x,
          s_observer.room_frame_raster_camera_x,
          s_observer.room_frame_prior_camera_x);
  s_observer.reported_room_frame_mismatch = true;
  s_observer.room_scene_verbose_reports++;
}

bool ActRaiserActionBg_RoomSceneFrameObserverActive(void) {
  return s_observer.room_frame_valid;
}

/* Native gate patches must follow the capture into its synthetic margins,
 * too. Otherwise the geometry could describe an open gate over closed art. */
static const uint16_t *VoxelLiveTile(const ActRaiserActionBgProvider *provider, int x, int y) {
  if (!provider || provider->layer || !s_voxel.source_valid ||
      !ActRaiserActionBg_VoxelGeometry(s_observer.map_group,s_observer.map_number)) return NULL;
  const unsigned width=s_voxel.source->bg[0].pages_wide*32, height=s_voxel.source->bg[0].pages_high*32;
  return x>=0 && y>=0 && (unsigned)x<width && (unsigned)y<height ? s_voxel.tiles+(size_t)y*width+x : NULL;
}

static uint32_t ProviderLookup(void *context, int32_t tile_x,
                               int32_t tile_y, uint16_t *entry) {
  const ActRaiserActionBgProvider *provider = context;
  if (provider && provider->capture_native_tiles)
    tile_y = WrapWorldTile(tile_y, ActionBgWorld_TileHeight(provider->world));
  s_observer.diagnostics.provider_lookups++;
  const uint16_t *castle = VoxelLiveTile(provider,tile_x,tile_y);
  const ActionBgLookupResult result = castle ? (*entry=*castle,kActionBgLookup_Tile) : LookupWorldTile(
      provider ? provider->world : NULL,
      provider && provider->wrap_world_x, tile_x, tile_y, entry);
  if (result == kActionBgLookup_Tile) {
    s_observer.diagnostics.provider_tiles++;
    return SR_PPU_VIRTUAL_TILE_FOUND;
  }
  if (result == kActionBgLookup_OutsideWorld)
    s_observer.diagnostics.provider_outside_world++;
  return SR_PPU_VIRTUAL_TILE_TRANSPARENT;
}

static uint32_t ProviderLookupSpan(
    void *context, int32_t tile_x, int32_t tile_y, int32_t tile_step,
    uint32_t capacity, const uint16_t **entries, int64_t *entry_step) {
  const ActRaiserActionBgProvider *provider = context;
  if (!provider || !provider->world || !entries || !entry_step || !capacity ||
      (tile_step != 1 && tile_step != -1))
    return 0;

  size_t filled = 0;
  ptrdiff_t native_step = 0;
  const uint16_t *castle = VoxelLiveTile(provider,tile_x,tile_y);
  if (castle) {
    const unsigned width = s_voxel.source->bg[0].pages_wide*32;
    filled = tile_step > 0 ? width-(unsigned)tile_x : (unsigned)tile_x+1;
    if (filled>capacity) filled=capacity;
    *entries=castle; native_step=tile_step;
  } else if (!provider->wrap_world_x) {
    filled = ActionBgWorld_LookupSpan(
        provider->world, tile_x, tile_y, tile_step,
        capacity, entries, &native_step);
  } else {
    const unsigned width = ActionBgWorld_TileWidth(provider->world);
    if (!width) return 0;
    int64_t wrapped_coordinate = tile_x % (int64_t)width;
    if (wrapped_coordinate < 0) wrapped_coordinate += width;
    const int32_t wrapped = (int32_t)wrapped_coordinate;
    size_t chunk = tile_step > 0
        ? width - (unsigned)wrapped : (unsigned)wrapped + 1u;
    if (chunk > capacity) chunk = capacity;
    filled = ActionBgWorld_LookupSpan(
        provider->world, wrapped, tile_y, tile_step,
        chunk, entries, &native_step);
  }

  if (!filled) return 0;
  s_observer.diagnostics.provider_batches++;
  s_observer.diagnostics.provider_lookups += filled;
  if (*entries) s_observer.diagnostics.provider_tiles += filled;
  else s_observer.diagnostics.provider_outside_world += filled;
  *entry_step = native_step;
  return (uint32_t)filled;
}

static uint64_t HashTileBandWord(uint64_t hash, uint16_t value) {
  hash = DeterministicHash_Fnv1a64Byte(hash, (uint8_t)value);
  return DeterministicHash_Fnv1a64Byte(hash, (uint8_t)(value >> 8));
}

/* Hash only classification fields. Geometry (z/order/alpha) can change
 * without rebuilding the tile-indexed band cache. Explicit byte hashing also
 * avoids depending on host padding or endianness. */
static bool HashTileBandRules(const DioramaVirtualLayerOverride *layer,
                              uint64_t *out_hash) {
  if (!layer || !out_hash ||
      layer->cell_span_count > kDioramaVirtualCellSpanMax)
    return false;
  uint64_t hash = DETERMINISTIC_HASH_FNV1A64_OFFSET;
  for (size_t i = 0; i < sizeof(layer->metatile_set); i++)
    hash = DeterministicHash_Fnv1a64Byte(hash, layer->metatile_set[i]);
  for (size_t i = 0; i < sizeof(layer->metatile_bands); i++)
    hash = DeterministicHash_Fnv1a64Byte(hash, layer->metatile_bands[i]);
  hash = HashTileBandWord(hash, layer->cell_span_count);
  for (size_t i = 0; i < layer->cell_span_count; i++) {
    const DioramaVirtualCellSpan *span = &layer->cell_spans[i];
    hash = HashTileBandWord(hash, span->x0);
    hash = HashTileBandWord(hash, span->y0);
    hash = HashTileBandWord(hash, span->x1);
    hash = HashTileBandWord(hash, span->y1);
    hash = DeterministicHash_Fnv1a64Byte(hash, span->band);
  }
  *out_hash = hash;
  return true;
}

static bool CompileTileBandCache(ActRaiserActionBgProvider *provider) {
  if (!provider || !provider->world || !provider->virtual_room ||
      provider->layer >= kActionBgLayerCount)
    return false;
  const DioramaVirtualLayerOverride *layer =
      &provider->virtual_room->virtual_layers[provider->layer];
  if (!DioramaLayerOrder_VirtualLayerHasClassification(layer)) return false;

  uint64_t rules_hash = 0;
  if (!HashTileBandRules(layer, &rules_hash)) return false;
  const uint32_t world_serial = ActionBgWorld_Serial(provider->world);
  const unsigned tile_width = ActionBgWorld_TileWidth(provider->world);
  const unsigned tile_height = ActionBgWorld_TileHeight(provider->world);
  const size_t tile_count = ActionBgWorld_TileCount(provider->world);
  if (!world_serial || !tile_width || !tile_height ||
      tile_count != (size_t)tile_width * tile_height)
    return false;
  if (provider->tile_band_cache_valid &&
      provider->tile_band_world == provider->world &&
      provider->tile_band_world_serial == world_serial &&
      provider->tile_band_rules_hash == rules_hash &&
      provider->tile_band_width == tile_width &&
      provider->tile_band_height == tile_height &&
      provider->tile_band_count == tile_count) {
    s_observer.diagnostics.provider_tile_band_cache_hits++;
    return true;
  }

  if (tile_count > provider->tile_band_capacity) {
    uint8_t *resized = realloc(provider->tile_band_cache, tile_count);
    if (!resized) {
      provider->tile_band_cache_valid = false;
      return false;
    }
    provider->tile_band_cache = resized;
    provider->tile_band_capacity = tile_count;
  }

  /* Start with the authentic priority split, then apply increasingly specific
   * authored rules. Forward span application makes the newest (last) record
   * win exactly as DioramaLayerOrder_VirtualBand does. */
  for (unsigned tile_y = 0; tile_y < tile_height; tile_y++) {
    for (unsigned tile_x = 0; tile_x < tile_width; tile_x++) {
      uint16_t tile_word = 0;
      if (ActionBgWorld_Lookup(provider->world, (int)tile_x, (int)tile_y,
                               &tile_word) != kActionBgLookup_Tile) {
        provider->tile_band_cache_valid = false;
        return false;
      }
      provider->tile_band_cache[(size_t)tile_y * tile_width + tile_x] =
          (tile_word & 0x2000u) ? 2 : 1;
    }
  }

  for (unsigned cell_y = 0; cell_y < tile_height / 2u; cell_y++) {
    for (unsigned cell_x = 0; cell_x < tile_width / 2u; cell_x++) {
      uint8_t metatile = 0;
      if (!ActionBgWorld_LookupMetatile(
              provider->world, (int)(cell_x * 2u), (int)(cell_y * 2u),
              &metatile)) {
        provider->tile_band_cache_valid = false;
        return false;
      }
      if (!(layer->metatile_set[metatile >> 3] &
            (uint8_t)(1u << (metatile & 7u))))
        continue;
      const uint8_t band = layer->metatile_bands[metatile];
      if (band >= kDioramaVirtualBandCount) {
        provider->tile_band_cache_valid = false;
        return false;
      }
      const size_t top_left =
          (size_t)(cell_y * 2u) * tile_width + cell_x * 2u;
      provider->tile_band_cache[top_left] = band;
      provider->tile_band_cache[top_left + 1u] = band;
      provider->tile_band_cache[top_left + tile_width] = band;
      provider->tile_band_cache[top_left + tile_width + 1u] = band;
    }
  }

  for (size_t i = 0; i < layer->cell_span_count; i++) {
    const DioramaVirtualCellSpan *span = &layer->cell_spans[i];
    if (span->band >= kDioramaVirtualBandCount || span->x1 < span->x0 ||
        span->y1 < span->y0) {
      provider->tile_band_cache_valid = false;
      return false;
    }
    size_t tile_x0 = (size_t)span->x0 * 2u;
    size_t tile_y0 = (size_t)span->y0 * 2u;
    size_t tile_x1 = ((size_t)span->x1 + 1u) * 2u;
    size_t tile_y1 = ((size_t)span->y1 + 1u) * 2u;
    if (tile_x0 >= tile_width || tile_y0 >= tile_height) continue;
    if (tile_x1 > tile_width) tile_x1 = tile_width;
    if (tile_y1 > tile_height) tile_y1 = tile_height;
    for (size_t tile_y = tile_y0; tile_y < tile_y1; tile_y++)
      memset(provider->tile_band_cache + tile_y * tile_width + tile_x0,
             span->band, tile_x1 - tile_x0);
  }

  provider->tile_band_count = tile_count;
  provider->tile_band_world = provider->world;
  provider->tile_band_rules_hash = rules_hash;
  provider->tile_band_world_serial = world_serial;
  provider->tile_band_width = tile_width;
  provider->tile_band_height = tile_height;
  provider->tile_band_cache_valid = true;
  provider->reported_tile_band_cache_failure = false;
  s_observer.diagnostics.provider_tile_band_cache_builds++;
  return true;
}

static uint32_t ProviderBandLookup(void *context, int32_t tile_x,
                                   int32_t tile_y, uint16_t entry,
                                   uint8_t *band) {
  const ActRaiserActionBgProvider *provider = context;
  (void)entry;
  if (!provider || !band || !provider->tile_band_cache_valid)
    return false;
  if (provider->wrap_world_x)
    tile_x = WrapWorldTile(tile_x, provider->tile_band_width);
  if (provider->capture_native_tiles)
    tile_y = WrapWorldTile(tile_y, provider->tile_band_height);
  if ((unsigned)tile_x >= provider->tile_band_width ||
      (unsigned)tile_y >= provider->tile_band_height)
    return false;
  *band = provider->tile_band_cache[
      (size_t)(unsigned)tile_y * provider->tile_band_width +
      (unsigned)tile_x];
  return *band < kDioramaVirtualBandCount;
}

bool ActRaiserActionBg_PixelLayerHasEdits(unsigned bg) {
  return bg < kActionBgLayerCount && s_provider[bg].pixel_edits_active;
}

bool ActRaiserActionBg_CaptureTilesBound(unsigned bg) {
  return bg < kActionBgLayerCount && (s_capture_edit_mask & (1u << bg));
}

static const ActionVoxelGeometry *ProviderCutouts(const ActRaiserActionBgProvider *provider) {
  const ActionVoxelGeometry *voxel = provider->layer == 0 && provider->world
      ? ActRaiserActionBg_VoxelGeometry(s_observer.map_group, s_observer.map_number) : NULL;
  return ActionVoxel_HasCutouts(voxel) ? voxel : NULL;
}
static const DioramaTileStamp *ProviderStampAt(const ActRaiserActionBgProvider *provider,
    int cx, int cy, int source_y) {
  if (!provider->virtual_room ||
      !provider->virtual_room->stamp_layers[provider->layer].count) return NULL;
  ActionBgRowPolicy row = {.edge = kActionBgEdge_Clamp};
  /* Tile callbacks address canonical source rows, including authored rows
   * outside a resident page. Their horizontal continuation follows the same
   * content-family policy as the ordinary capture, never native page wrap
   * alone. Exact authored cells remain authoritative under every policy. */
  if (provider->capture_native_tiles)
    (void)ActionBgLayerPlan_ResolveRow(&provider->capture_plan,
        source_y - (int)provider->capture_plan.camera_y - 1, &row);
  return DioramaLayerOrder_ExtendedRowStampAt(
      &provider->virtual_room->stamp_layers[provider->layer], cx, cy,
      provider->capture_native_tiles ? (int)ActionBgWorld_TileWidth(provider->world) / 2 : 0,
      provider->capture_native_tiles ? (int)ActionBgWorld_TileHeight(provider->world) / 2 : 0,
      row.edge);
}
static uint32_t ProviderCaptureTile(void *context, int32_t tile_x,
                                    int32_t tile_y, SrPpuCaptureTile *tile) {
  const ActRaiserActionBgProvider *provider = context;
  const DioramaRoomOverride *room = provider->virtual_room;
  const bool bubbles=provider->bubbles.characters!=NULL;
  const ActionVoxelGeometry *cutouts = ProviderCutouts(provider);
  if ((!room || !provider->pixel_edits_active) && !bubbles && !cutouts) return 0;
  const unsigned bg = provider->layer;
  int cx = tile_x >= 0 ? tile_x / 2 : (tile_x - 1) / 2;
  int cy = tile_y >= 0 ? tile_y / 2 : (tile_y - 1) / 2;
  const DioramaTileStamp *stamp = ProviderStampAt(provider, cx, cy, tile_y * 8);
  int metatile = -1;
  /* Keep the legacy callback prefix safe on an older runner. Character bytes
   * are written only when ConfigureLavaBubbles verified the new capability. */
  memset(tile,0,SR_PPU_CAPTURE_TILE_MASKS_SIZE);
  if (stamp) {
    cx = stamp->x; cy = stamp->y;
    unsigned quadrant = ((tile_y & 1) * 2) + (tile_x & 1);
    tile->entry = stamp->words[quadrant];
    tile->band = ActionVoxel_OwnsBackgroundBands(ActionVoxel_Profile(
        ActRaiserActionBg_VoxelGeometry(s_observer.map_group,s_observer.map_number)),
        s_observer.map_number,bg)
        ? (tile->entry & 0x2000u ? 2u : 1u) : (stamp->bands >> (quadrant * 2)) & 3u;
    tile->flags = SR_PPU_CAPTURE_TILE_REPLACE |
        (stamp->blank ? SR_PPU_CAPTURE_TILE_BLANK : 0u);
    if (!stamp->blank) metatile = stamp->metatile;
  } else {
    if ((!room || !room->pixel_layers[bg].count) && !provider->pixel_band_cache_active && !bubbles && !cutouts) return 0;
    if (provider->wrap_world_x)
      tile_x = WrapWorldTile(tile_x, ActionBgWorld_TileWidth(provider->world));
    if (provider->capture_native_tiles)
      tile_y = WrapWorldTile(tile_y, ActionBgWorld_TileHeight(provider->world));
    uint8_t id;
    if (ActionBgWorld_Lookup(provider->world, tile_x, tile_y, &tile->entry) !=
            kActionBgLookup_Tile ||
        !ActionBgWorld_LookupMetatile(provider->world, tile_x, tile_y, &id)) return 0;
    const uint16_t *castle = VoxelLiveTile(provider,tile_x,tile_y);
    if (castle) tile->entry = *castle;
    cx = tile_x / 2;
    cy = tile_y / 2;
    metatile = id;
    tile->band = (tile->entry & 0x2000u) ? 2u : 1u;
    if (provider->pixel_band_cache_active)
      (void)ProviderBandLookup(context, tile_x, tile_y, tile->entry, &tile->band);
    /* Native pages need replacement pixels only to move a depth band. Pixel
     * masks alone must leave the live artwork in place. Neither is new scenery
     * allowed to override edge filling or extend the native vertical bounds. */
    if (provider->capture_native_tiles && provider->pixel_band_cache_active) {
      tile->flags = SR_PPU_CAPTURE_TILE_REPLACE;
      if (s_runner_api->capabilities & SR_RUNNER_CAP_PPU_CAPTURE_TILE_POLICY)
        tile->flags |= SR_PPU_CAPTURE_TILE_FOLLOW_POLICY;
    }
  }
  const bool bubble=!stamp && bubbles && ActionLavaBubbles_Character(&provider->bubbles,
      (unsigned)metatile,tile->entry,tile_x,tile_y,tile->character);
  if (bubble) tile->flags|=SR_PPU_CAPTURE_TILE_REPLACE|SR_PPU_CAPTURE_TILE_FOLLOW_POLICY|SR_PPU_CAPTURE_TILE_CHARACTER;
  const DioramaPixelEdit *edit = room && room->pixel_layers[bg].count
      ? DioramaLayerOrder_PixelEditAt(room, bg, cx, cy, metatile) : NULL;
  if (edit)
    for (unsigned row = 0; row < 8; ++row) {
      const unsigned mask_row = (tile_y & 1) * 8 + row;
      const unsigned shift = (tile_x & 1) ? 0 : 8;
      tile->black_rows[row] = (uint8_t)(edit->black[mask_row] >> shift);
      tile->transparent_rows[row] = (uint8_t)(edit->transparent[mask_row] >> shift);
    }
  const bool cutout = ActionVoxel_CutoutRows(cutouts, tile_x, tile_y, tile->transparent_rows);
  return tile->band < kDioramaVirtualBandCount &&
      (stamp || edit || provider->pixel_band_cache_active || bubble || cutout);
}

static bool EnvironmentTileEdit(void *context, unsigned bg, int x, int y,
                                ActionEnvironmentTileEdit *out) {
  const ActionEnvironmentScene *scene = context;
  if (bg >= 2 || !scene || !ActRaiserActionBg_HleEnabled() || !s_observer.map_valid ||
      s_observer.map_group != scene->group || s_observer.map_number != scene->room)
    return false;
  const ActRaiserActionBgProvider *p = &s_provider[bg];
  if (!p->world ||
      (!p->world_apron_available && !p->horizontal_bounds_available && !p->pixel_edits_active))
    return false;
  SrPpuCaptureTile tile;
  const bool edited = ProviderCaptureTile((void *)p, x, y, &tile) != 0;
  if (!edited) {
    uint16_t word;
    const uint16_t *castle = VoxelLiveTile(p, x, y);
    if (castle) word = *castle;
    else if (ActionBgWorld_Lookup(p->world, x, y, &word) != kActionBgLookup_Tile)
      return false;
    tile = (SrPpuCaptureTile){.entry = word, .band = (word & 0x2000) ? 2 : 1};
    if (p->pixel_band_cache_active) (void)ProviderBandLookup((void *)p, x, y, word, &tile.band);
  }
  *out = (ActionEnvironmentTileEdit){.entry = tile.entry,
                                     .band = tile.band,
                                     /* Both paths above resolve the actual world word. */
                                     .replace = 1,
                                     .blank = (tile.flags & SR_PPU_CAPTURE_TILE_BLANK) != 0};
  memcpy(out->black, tile.black_rows, 8);
  memcpy(out->transparent, tile.transparent_rows, 8);
  out->custom_character=(tile.flags&SR_PPU_CAPTURE_TILE_CHARACTER)!=0;
  if(out->custom_character)memcpy(out->character,tile.character,32);
  return true;
}
static bool EnvironmentRasterFrame(const ActionEnvironmentScene *scene, ActionRoomSceneFrameState *out) {
  if (!EnsureRoomScene(scene->group, scene->room) ||
      (s_observer.room_scene->raster_effect != kActionRoomRaster_None &&
       !RasterDmaContractMatches(s_observer.room_scene->raster_effect))) return false;
  const ActionRoomSceneFrameRequest request = {
    .camera_x = scene->camera_x[0], .camera_y = scene->camera_y[0],
    .raster_camera_x = RasterTableCamera(g_ram), .have_raster_camera_x = true,
    .game_frame = ReadWram16(g_ram, kActRaiserWram_GameFrame), .animation_phase = -1, .page_phase = -1};
  return ActionRoomScene_BuildFrameState(s_observer.room_scene, &request, out);
}
static bool EnvironmentRasterDepth(const ActionEnvironmentScene *scene, float *rows) {
  if (!EnsureRoomScene(scene->group, scene->room)) return false;
  const ActionRoomSceneFrameRequest request = {.camera_x=scene->camera_x[0],
      .camera_y=scene->camera_y[0],.game_frame=scene->clock,.animation_phase=-1,.page_phase=-1};
  return ActionRoomScene_RasterCameraSensitivity(s_observer.room_scene,&request,rows);
}

bool ActRaiserActionBg_BindEnvironmentScenery(ActionEnvironmentScene *scene) {
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram;
  if (!scene || !QueryPpuState(&ppu) || ppu.bg_mode != 1 ||
      !BorrowVram(ppu.lifetime_generation, &vram))
    return false;
  scene->vram = vram.data;
  for (unsigned i = 0; i < 2; ++i)
    scene->tile_base[i] = ppu.backgrounds[i].tile_base_word;
  scene->tile_edit = EnvironmentTileEdit;
  scene->tile_edit_context = scene;
  scene->capture_background_frame = EnvironmentRasterFrame;
  scene->capture_background_depth = EnvironmentRasterDepth;
  return true;
}

void ActRaiserActionBg_ConfigureLavaBubbles(const ActionSurfaceField *field, uint16_t clock) {
  const bool supported=field && s_observer.map_valid && s_runner_api &&
      (s_runner_api->capabilities&SR_RUNNER_CAP_PPU_CAPTURE_CHARACTER) &&
      EnsureRoomScene(s_observer.map_group,s_observer.map_number);
  ActionLavaBubbles_Prepare(&s_provider[0].bubbles,
      supported?s_observer.room_scene:NULL,field,clock);
}

bool ActRaiserActionBg_BindCaptureTiles(uint8_t capture_mask, uint8_t apron_mask) {
  s_capture_edit_mask = 0;
  SrPpuStateSnapshot state;
  if (!CaptureMasksSupported() || !QueryPpuState(&state)) return false;
  SrPpuCaptureTileRequest request = {
    .struct_size = sizeof(request), .lifetime_generation = state.lifetime_generation,
  };
  for (unsigned bg = 0; bg < kActionBgLayerCount; ++bg) {
    if (!(capture_mask & (1u << bg))) continue;
    ActRaiserActionBgProvider *provider = &s_provider[bg];
    const DioramaRoomOverride *room = provider->virtual_room;
    bool edits = ProviderCutouts(provider) || provider->bubbles.characters || (provider->pixel_edits_active && room &&
        (room->stamp_layers[bg].count || room->pixel_layers[bg].count ||
         provider->pixel_band_cache_active));
    if (!edits && !(apron_mask & (1u << bg))) continue;
    request.layer_mask |= 1u << bg;
    /* The published room edits/world are immutable through scanout. Raster
     * scroll, CHR and palette changes remain owned by the runtime. */
    if (edits && (s_runner_api->capabilities & SR_RUNNER_CAP_PPU_CAPTURE_TILE_CACHE))
      request.stable_layer_mask |= 1u << bg;
    request.bindings[bg] = (SrPpuCaptureTileBinding){
      .lookup = edits ? ProviderCaptureTile : NULL,
      .user_data = provider,
      .apron = (apron_mask & (1u << bg)) ? SR_PPU_OBJ_APRON : 0,
    };
  }
  if (s_runner_api->replace_ppu_capture_tiles(s_runner, &request) != SR_RESULT_OK)
    return false;
  for (unsigned bg = 0; bg < kActionBgLayerCount; bg++)
    if ((request.layer_mask & (1u << bg)) && request.bindings[bg].lookup)
      s_capture_edit_mask |= (uint8_t)(1u << bg);
  return true;
}

bool ActRaiserActionBg_PixelEditsActive(void) {
  return s_provider[0].pixel_edits_active || s_provider[1].pixel_edits_active;
}

bool ActRaiserActionBg_WorldApronAvailable(unsigned bg) {
  return bg < kActionBgLayerCount && s_provider[bg].world_apron_available;
}

bool ActRaiserActionBg_HorizontalSourceBounds(
    unsigned bg, uint16_t hscroll, int *x0, int *x1) {
  if (x0) *x0 = 0;
  if (x1) *x1 = 0;
  if (bg >= kActionBgLayerCount || !x0 || !x1 ||
      !s_provider[bg].horizontal_bounds_available) return false;
  const ActRaiserActionBgProvider *provider = &s_provider[bg];
  const int dh = ((hscroll - provider->hscroll_anchor + 512) & 1023) - 512;
  *x0 = provider->scenery_x0 - provider->camera_x - dh;
  *x1 = *x0 + provider->scenery_width;
  return true;
}

bool ActRaiserActionBg_StampAt(unsigned bg, int source_x, int sample_y,
                              uint16_t hscroll, uint16_t vscroll,
                              uint16_t *entry, uint8_t *band,
                              uint8_t *local_x, uint8_t *local_y,
                              bool *black, bool *blank) {
  if (bg >= kActionBgLayerCount || !entry || !band ||
      !local_x || !local_y || !black || !blank) return false;
  const ActRaiserActionBgProvider *provider = &s_provider[bg];
  if (!provider->pixel_edits_active || !provider->virtual_room ||
      !provider->virtual_room->stamp_layers[bg].count) return false;
  const int dh = ((hscroll - provider->hscroll_anchor + 512) & 1023) - 512;
  const int dv = ((vscroll - provider->vscroll_anchor + 512) & 1023) - 512;
  const int x = provider->camera_x + source_x + dh;
  const int y = provider->camera_y + sample_y + dv;
  /* Floor division is required for scenery extending left or above zero. */
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cy = y >= 0 ? y / 16 : (y - 15) / 16;
  const DioramaTileStamp *cell = ProviderStampAt(provider, cx, cy, y);
  if (!cell) return false;
  *local_x = (uint8_t)(x & 15);
  *local_y = (uint8_t)(y & 15);
  const unsigned quadrant = (*local_y / 8) * 2 + *local_x / 8;
  *entry = cell->words[quadrant];
  *band = (cell->bands >> (quadrant * 2)) & 3u;
  *blank = cell->blank;
  const DioramaPixelEdit *edit = DioramaLayerOrder_PixelEditAt(
      provider->virtual_room, bg, cell->x, cell->y, cell->blank ? -1 : cell->metatile);
  const unsigned bit = 1u << (15 - *local_x);
  const bool transparent = edit && (edit->transparent[*local_y] & bit);
  *blank |= transparent;
  *black = !transparent && edit && (edit->black[*local_y] & bit);
  return *band < kDioramaVirtualBandCount;
}

/* Coordinates already include the PPU's margin/mosaic policy. Reconstruct the
 * same finite world address used by the virtual tile provider; masks address
 * displayed metatile pixels, so character flips do not flip the authored mask. */
static bool PixelEditedAt(unsigned bg, int source_x, int sample_y,
                                    uint16_t hscroll, uint16_t vscroll,
                                    uint8_t *band, bool transparent) {
  if (bg >= kActionBgLayerCount || !band) return false;
  ActRaiserActionBgProvider *provider = &s_provider[bg];
  if (!provider->pixel_edits_active) return false;
  const int dh = ((hscroll - provider->hscroll_anchor + 512) & 1023) - 512;
  const int dv = ((vscroll - provider->vscroll_anchor + 512) & 1023) - 512;
  int x = provider->camera_x + source_x + dh;
  const int y = provider->camera_y + sample_y + dv;
  const int width = (int)ActionBgWorld_TileWidth(provider->world) * 8;
  if (provider->wrap_world_x) x = ((x % width) + width) % width;
  if (x < 0 || y < 0) return false;
  const int tx = x / 8, ty = y / 8;
  uint16_t entry;
  uint8_t id;
  if (ActionBgWorld_Lookup(provider->world, tx, ty, &entry) !=
          kActionBgLookup_Tile ||
      !ActionBgWorld_LookupMetatile(provider->world, tx, ty, &id))
    return false;
  if (provider->pixel_cell_x != x / 16 || provider->pixel_cell_y != y / 16) {
    provider->pixel_cell_x = x / 16;
    provider->pixel_cell_y = y / 16;
    provider->pixel_edit = DioramaLayerOrder_PixelEditAt(
        provider->virtual_room, bg, (unsigned)x / 16, (unsigned)y / 16, id);
  }
  const DioramaPixelEdit *edit = provider->pixel_edit;
  const unsigned bit = 1u << (15 - (x & 15));
  if (!edit || !(transparent ? edit->transparent[y & 15] & bit :
      (edit->black[y & 15] & bit) && !(edit->transparent[y & 15] & bit)))
    return false;
  *band = (entry & 0x2000u) ? 2 : 1;
  if (provider->pixel_band_cache_active)
    (void)ProviderBandLookup(provider, tx, ty, entry, band);
  return true;
}
bool ActRaiserActionBg_PixelBlackAt(unsigned bg, int source_x, int sample_y,
                                    uint16_t hscroll, uint16_t vscroll,
                                    uint8_t *band) {
  return PixelEditedAt(bg, source_x, sample_y, hscroll, vscroll, band, false);
}
bool ActRaiserActionBg_PixelTransparentAt(unsigned bg, int source_x, int sample_y,
                                         uint16_t hscroll, uint16_t vscroll,
                                         uint8_t *band) {
  return PixelEditedAt(bg, source_x, sample_y, hscroll, vscroll, band, true);
}

bool ActRaiserActionBg_NativeSceneryAt(unsigned bg, int source_x, int sample_y,
                                      uint16_t hscroll, uint16_t vscroll,
                                      uint16_t *entry, uint8_t *band,
                                      uint8_t *local_x, uint8_t *local_y,
                                      bool *black) {
  if (bg >= kActionBgLayerCount || !entry || !band || !local_x || !local_y || !black)
    return false;
  const ActRaiserActionBgProvider *provider = &s_provider[bg];
  if (!provider->pixel_edits_active && !provider->world_apron_available)
    return false;
  const int dh = ((hscroll - provider->hscroll_anchor + 512) & 1023) - 512;
  const int dv = ((vscroll - provider->vscroll_anchor + 512) & 1023) - 512;
  int x = provider->camera_x + source_x + dh;
  const int y = provider->camera_y + sample_y + dv;
  if (provider->wrap_world_x) {
    const int width = (int)ActionBgWorld_TileWidth(provider->world) * 8;
    x = ((x % width) + width) % width;
  }
  if (x < 0 || y < 0 ||
      ActionBgWorld_Lookup(provider->world, x / 8, y / 8, entry) != kActionBgLookup_Tile)
    return false;
  *local_x = (uint8_t)(x & 7);
  *local_y = (uint8_t)(y & 7);
  *band = (*entry & 0x2000u) ? 2 : 1;
  if (provider->pixel_band_cache_active)
    (void)ProviderBandLookup((void *)provider, x / 8, y / 8, *entry, band);
  *black = ActRaiserActionBg_PixelBlackAt(bg, source_x, sample_y, hscroll, vscroll, band);
  if (ActRaiserActionBg_PixelTransparentAt(bg, source_x, sample_y, hscroll, vscroll, band))
    return false;
  return true;
}

static void ReportComparison(
    const uint8_t *wram, unsigned layer, uint8_t map_group,
    uint8_t map_number, const ActRaiserActionBgLayerSnapshot *snapshot,
    uint32_t serial, const ActRaiserActionBgCompareResult *comparison,
    uint32_t *reported_outside_serial,
    uint32_t *reported_mismatch_serial) {
  if (comparison->outside_world &&
      reported_outside_serial[layer] != serial) {
    reported_outside_serial[layer] = serial;
    fprintf(stderr,
            "[action-bg-hle] finite-edge gf=%u map=%02X/%02X BG%u "
            "serial=%u count=%zu first=(%d,%d) camera=(%u,%u) "
            "world=%ux%u\n",
            ReadWram16(wram, kActRaiserWram_GameFrame),
            map_group, map_number, layer + 1, serial,
            comparison->outside_world,
            comparison->first_outside_tile_x,
            comparison->first_outside_tile_y,
            snapshot->camera_x, snapshot->camera_y,
            snapshot->decode.world_width, snapshot->decode.world_height);
  }
  if (comparison->mismatches &&
      reported_mismatch_serial[layer] != serial) {
    reported_mismatch_serial[layer] = serial;
    fprintf(stderr,
            "[action-bg-hle] MISMATCH gf=%u map=%02X/%02X BG%u "
            "serial=%u count=%zu/%zu first=(%d,%d) "
            "hle=$%04X native=$%04X\n",
            ReadWram16(wram, kActRaiserWram_GameFrame),
            map_group, map_number, layer + 1, serial,
            comparison->mismatches, comparison->compared,
            comparison->first_tile_x, comparison->first_tile_y,
            comparison->first_hle, comparison->first_native);
  }
}

uint8_t ActRaiserActionBg_BindPlan(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan) {
  return ActRaiserActionBg_BindPlanWithVirtualLayers(
      wram, wram_size, plan, NULL);
}

/* Native single-page backgrounds are outside the scrolling 64x64 ring
 * provider's domain. Capture edits can still use the live metatile page:
 * the PPU retains original pixels/scroll/color math and only authored tiles
 * or depth bands change. $0701 selects one of its two resident scene pages. */
static void PrepareNativeCaptureEdits(const uint8_t *wram, size_t wram_size,
    const SrPpuStateSnapshot *ppu, const ActionBgPlan *plan, const DioramaRoomOverride *room,
    SrPpuVirtualTilemapRequest *bindings) {
  if (!room ||
      wram[kActRaiserWram_MapGroup] != room->map_group ||
      wram[kActRaiserWram_CurrentMap] != room->map_number ||
      (ppu->flags & SR_PPU_STATE_FORCED_BLANK) || ppu->bg_mode != 1) return;
  const bool hub = room->map_group == kActRaiserMapGroup_DeathHeim &&
      room->map_number == kActRaiserDeathHeimMap_Hub;
  for (unsigned bg = 0; bg < kActionBgLayerCount; bg++) {
    if (!plan->layer[bg].valid || plan->layer[bg].source == kActionBgSource_WorldMap) continue;
    const bool classified = !ActionVoxel_OwnsBackgroundBands(ActionVoxel_Profile(
        ActRaiserActionBg_VoxelGeometry(room->map_group,room->map_number)),room->map_number,bg) &&
        DioramaLayerOrder_VirtualLayerHasClassification(&room->virtual_layers[bg]);
    if (!classified && !room->stamp_layers[bg].count && !room->pixel_layers[bg].count) continue;
    ActRaiserActionBgLayerSnapshot snapshot;
    if (!ActRaiserActionBg_CaptureLayer(wram, wram_size, bg,
        ppu->background_tilemap_control[bg], &snapshot) ||
        (snapshot.bgsc & 3) != 0 || snapshot.decode.world_height != 256) continue;
    if (hub) {
      if (snapshot.decode.world_width != 512) continue;
      const unsigned page = (snapshot.bgsc & 0xfc) == (bg ? 0x74 : 0x64);
      if ((snapshot.bgsc & 0xfc) != (bg ? 0x70 : 0x60) && !page) continue;
      snapshot.decode.world_width = 256;
      snapshot.decode.map_page += page * 256;
    } else if (snapshot.decode.world_width != 256) {
      continue;
    }
    ActionBgWorld *world = WorldForLayer(
        s_observer.provider_world, bg, room->map_group, room->map_number);
    if (!world || !ActionBgWorld_Update(world, &snapshot.decode)) continue;
    ActRaiserActionBgProvider *provider = &s_provider[bg];
    provider->world = world;
    provider->virtual_room = room;
    provider->capture_plan = plan->layer[bg];
    provider->layer = (uint8_t)bg;
    provider->wrap_world_x = true;
    provider->camera_x = snapshot.camera_x;
    provider->camera_y = snapshot.camera_y;
    /* A resident page's map origin is fixed; its live scroll may already
     * include HDMA motion (Bloodpool's last row is not its WRAM camera).
     * Anchor to the canonical camera so each row retains that displacement. */
    provider->hscroll_anchor = snapshot.camera_x & 0x3ff;
    provider->vscroll_anchor = snapshot.camera_y & 0x3ff;
    provider->pixel_cell_x = provider->pixel_cell_y = -1;
    provider->pixel_band_cache_active = classified && CompileTileBandCache(provider);
    provider->pixel_edits_active = true;
    provider->capture_native_tiles = true;
    /* Capture authoring uses the runner's coordinate binding, without its
     * INCLUDE_AUTHENTIC flag. The original VRAM page and live raster continue
     * to own ordinary scanout; only the isolated capture can replace tiles. */
    bindings->layer_mask |= 1u << bg;
    bindings->bindings[bg] = (SrPpuVirtualTilemapBinding){
        .lookup = ProviderLookup, .user_data = provider,
        .camera_x = provider->camera_x, .camera_y = provider->camera_y,
        .hscroll_anchor = provider->hscroll_anchor,
        .vscroll_anchor = provider->vscroll_anchor,
    };
  }
}

uint8_t ActRaiserActionBg_BindPlanWithVirtualLayers(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    const struct DioramaRoomOverride *virtual_room) {
  s_capture_edit_mask = 0;
  for (unsigned bg = 0; bg < kActionBgLayerCount; bg++) {
    s_provider[bg].pixel_edits_active = false;
    s_provider[bg].capture_native_tiles = false;
    s_provider[bg].world_apron_available = false;
    s_provider[bg].horizontal_bounds_available = false;
  }
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram;
  SrPpuVirtualTilemapRequest binding_request;
  if (!QueryPpuState(&ppu)) return 0;
  binding_request = (SrPpuVirtualTilemapRequest) {
    .struct_size = sizeof(binding_request),
    .lifetime_generation = ppu.lifetime_generation,
  };
  if (s_runner_api->replace_ppu_virtual_tilemaps(
          s_runner, &binding_request) != SR_RESULT_OK)
    return 0;
  if (!wram || !plan || !plan->valid ||
      !SyncFrameIdentity(wram, wram_size))
    return 0;
  PrepareNativeCaptureEdits(wram, wram_size, &ppu, plan, virtual_room, &binding_request);
  if (binding_request.layer_mask && s_runner_api->replace_ppu_virtual_tilemaps(
      s_runner, &binding_request) != SR_RESULT_OK) {
    for (unsigned bg = 0; bg < kActionBgLayerCount; bg++)
      s_provider[bg].pixel_edits_active = false;
    return 0;
  }
  if (!ActRaiserActionBg_HleEnabled()) return 0;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  if (!ActRaiser_IsActionMapGroup(map_group)) return 0;
  s_observer.diagnostics.provider_frames++;
  if ((ppu.flags & SR_PPU_STATE_FORCED_BLANK) != 0u) {
    if (!s_observer.forced_blank) ResetWorlds();
    s_observer.forced_blank = true;
    for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
      if (plan->layer[layer].source == kActionBgSource_WorldMap)
        RecordProviderFallback(kActRaiserActionBgFallback_ForcedBlank, layer,
                               map_group, map_number, NULL);
    return 0;
  }
  s_observer.forced_blank = false;
  if (ppu.bg_mode != 1u) {
    for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
      if (plan->layer[layer].source == kActionBgSource_WorldMap)
        RecordProviderFallback(kActRaiserActionBgFallback_WrongMode, layer,
                               map_group, map_number, NULL);
    return 0;
  }
  if (!BorrowVram(ppu.lifetime_generation, &vram)) return 0;

  uint8_t bound = 0;
  const uint8_t enabled = ppu.main_screen | ppu.sub_screen;
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    const ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    if (!layer_plan->valid ||
        layer_plan->source != kActionBgSource_WorldMap)
      continue;
    ActRaiserActionBgLayerSnapshot snapshot;
    if (!ActRaiserActionBg_CaptureLayer(
            wram, wram_size, layer,
            ppu.background_tilemap_control[layer], &snapshot) ||
        layer_plan->world_width != snapshot.decode.world_width ||
        layer_plan->world_height != snapshot.decode.world_height) {
      RecordProviderFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                             map_group, map_number, NULL);
      continue;
    }
    if (!(enabled & (1u << layer))) {
      RecordProviderFallback(kActRaiserActionBgFallback_LayerDisabled, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    if (!ActRaiserActionBg_WorldRingEligible(
            &snapshot, (size_t)vram.element_count)) {
      RecordProviderFallback(kActRaiserActionBgFallback_NativeTilemap, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    if ((snapshot.camera_x & 0x3ffu) !=
            (ppu.backgrounds[layer].h_scroll & 0x3ffu) ||
        (snapshot.camera_y & 0x3ffu) !=
            (ppu.backgrounds[layer].v_scroll & 0x3ffu)) {
      RecordProviderFallback(kActRaiserActionBgFallback_ScrollPhase, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    ActionBgWorld *world = WorldForLayer(
        s_observer.provider_world, layer, map_group, map_number);
    if (!world) continue;
    const uint32_t before = ActionBgWorld_Serial(world);
    bool room_scene_source = false;
    if (RoomSceneHleEnabled() && EnsureRoomScene(map_group, map_number)) {
      const uint8_t bg_layer = (uint8_t)(layer + 1u);
      room_scene_source =
          ActionRoomScene_TileWidth(s_observer.room_scene, bg_layer) * 8u ==
              snapshot.decode.world_width &&
          ActionRoomScene_TileHeight(s_observer.room_scene, bg_layer) * 8u ==
              snapshot.decode.world_height &&
          ActRaiserActionBg_UpdateWorldFromRoomScene(
              world, s_observer.room_scene, bg_layer);
    }
    if (RoomSceneHleEnabled() && !room_scene_source) {
      s_observer.diagnostics.room_scene_hle_fallbacks++;
      if (!s_observer.reported_room_scene_hle_fallback[layer]) {
        s_observer.reported_room_scene_hle_fallback[layer] = true;
        fprintf(stderr,
                "[action-room-scene] provider fallback map=%02X/%02X "
                "BG%u; retaining live WRAM source\n",
                map_group, map_number, layer + 1u);
      }
    }
    if (!room_scene_source &&
        !ActionBgWorld_Update(world, &snapshot.decode)) {
      RecordProviderFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    if (room_scene_source)
      s_observer.diagnostics.room_scene_hle_layers++;
    if (ActionBgWorld_Serial(world) != before)
      s_observer.diagnostics.layer_activations++;
    const uint32_t serial = ActionBgWorld_Serial(world);
    if (!room_scene_source)
      CompareRoomSceneWorld(layer, map_group, map_number, world);
    ActRaiserActionBgCompareResult comparison;
    if (!ActRaiserActionBg_CompareLayer(
            world, &snapshot, vram.data, (size_t)vram.element_count,
            layer_plan->wrap_world_x, &comparison)) {
      RecordProviderFallback(kActRaiserActionBgFallback_CompareFailure, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    s_observer.diagnostics.provider_preflight_layers++;
    s_observer.diagnostics.provider_preflight_tiles += comparison.compared;
    s_observer.diagnostics.provider_preflight_mismatches +=
        comparison.mismatches;
    s_observer.diagnostics.provider_preflight_outside_world +=
        comparison.outside_world;
    ReportComparison(
        wram, layer, map_group, map_number, &snapshot, serial, &comparison,
        s_observer.provider_reported_outside_serial,
        s_observer.provider_reported_mismatch_serial);
    if (comparison.outside_world) {
      RecordProviderFallback(kActRaiserActionBgFallback_AuthenticEdge, layer,
                             map_group, map_number, &snapshot);
      continue;
    }

    /* A resident ring can lag a newly visible 8px edge until the native 16px
     * streamer publishes it; a legitimate runtime patch could also disagree
     * with the immutable map. Keep either kind of live word authoritative in
     * the authentic viewport while the finite decoder continues to own only
     * synthetic margins. A coordinate outside the decoded world is different:
     * there is no safe finite source, so that still fails closed above. */
    const bool include_authentic = comparison.mismatches == 0;
    s_observer.diagnostics.provider_eligible_layers++;
    s_provider[layer].world = world;
    s_provider[layer].virtual_room = virtual_room;
    s_provider[layer].wrap_world_x = layer_plan->wrap_world_x;
    s_provider[layer].layer = (uint8_t)layer;
    const bool virtual_layer_classified = !ActionVoxel_OwnsBackgroundBands(ActionVoxel_Profile(
        ActRaiserActionBg_VoxelGeometry(map_group,map_number)),map_number,layer) && virtual_room &&
        DioramaLayerOrder_VirtualLayerHasClassification(
            &virtual_room->virtual_layers[layer]);
    const bool tile_band_cache_ready =
        virtual_layer_classified && CompileTileBandCache(&s_provider[layer]);
    if (virtual_layer_classified && !tile_band_cache_ready &&
        !s_provider[layer].reported_tile_band_cache_failure) {
      fprintf(stderr,
              "[action-bg-hle] map=%02X/%02X BG%u virtual-layer band "
              "cache unavailable; using authentic priority bands\n",
              map_group, map_number, layer + 1u);
      s_provider[layer].reported_tile_band_cache_failure = true;
    }
    s_provider[layer].pixel_band_cache_active = tile_band_cache_ready;
    s_provider[layer].camera_x = snapshot.camera_x;
    s_provider[layer].camera_y = snapshot.camera_y;
    s_provider[layer].hscroll_anchor = ppu.backgrounds[layer].h_scroll & 0x3ffu;
    s_provider[layer].vscroll_anchor = ppu.backgrounds[layer].v_scroll & 0x3ffu;
    s_provider[layer].pixel_cell_x = s_provider[layer].pixel_cell_y = -1;
    s_provider[layer].pixel_edits_active = include_authentic &&
        DioramaCameraPolicy_HasEdits(virtual_room, layer);
    s_provider[layer].horizontal_bounds_available = !layer_plan->wrap_world_x;
    ActRaiserActionBg_ResolveDioramaHorizontalExtent(
        s_provider[layer].pixel_edits_active ? virtual_room : NULL, layer,
        snapshot.decode.world_width, &s_provider[layer].scenery_x0,
        &s_provider[layer].scenery_width);
    /* The extra diorama capture can sample verified world terrain even when
     * the room has no editor overrides. Preserve every synthetic edge/band
     * and explicit extent; only ordinary live-world layers can continue.
     * A lagging authentic ring does not invalidate its world-backed margins. */
    s_provider[layer].world_apron_available =
        DioramaCameraPolicy_WorldApron(layer_plan);
    const SrPpuVirtualTilemapBinding binding = {
      .lookup = ProviderLookup,
      .lookup_span = ProviderLookupSpan,
      .band_lookup = tile_band_cache_ready ? ProviderBandLookup : NULL,
      .user_data = &s_provider[layer],
      .camera_x = snapshot.camera_x,
      .camera_y = snapshot.camera_y,
      .hscroll_anchor = ppu.backgrounds[layer].h_scroll & 0x3ffu,
      .vscroll_anchor = ppu.backgrounds[layer].v_scroll & 0x3ffu,
      .flags = include_authentic
          ? SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC : 0u,
    };
    binding_request.bindings[layer] = binding;
    binding_request.layer_mask |= 1u << layer;
    bound |= (uint8_t)(1u << layer);
    s_observer.diagnostics.provider_layers++;
  }
  if (bound != 0u && s_runner_api->replace_ppu_virtual_tilemaps(
          s_runner, &binding_request) != SR_RESULT_OK) {
    for (unsigned bg = 0; bg < kActionBgLayerCount; bg++) {
      s_provider[bg].pixel_edits_active = false;
      s_provider[bg].world_apron_available = false;
      s_provider[bg].horizontal_bounds_available = false;
    }
    binding_request.layer_mask = 0u;
    (void)s_runner_api->replace_ppu_virtual_tilemaps(
        s_runner, &binding_request);
    return 0;
  }
  return bound;
}

static void ObserveLayer(const uint8_t *wram, size_t wram_size,
                         const SrPpuStateSnapshot *ppu,
                         const SrBorrowedU16Span *vram, unsigned layer,
                         uint8_t map_group, uint8_t map_number,
                         bool wrap_world_x) {
  ActRaiserActionBgLayerSnapshot snapshot;
  if (!ActRaiserActionBg_CaptureLayer(
          wram, wram_size, layer,
          ppu->background_tilemap_control[layer], &snapshot)) {
    RecordFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                   map_group, map_number, NULL);
    return;
  }
  const uint8_t enabled = ppu->main_screen | ppu->sub_screen;
  if (!(enabled & (1u << layer))) {
    RecordFallback(kActRaiserActionBgFallback_LayerDisabled, layer,
                   map_group, map_number, &snapshot);
    return;
  }
  if (!ActRaiserActionBg_WorldRingEligible(&snapshot,
                                           (size_t)vram->element_count)) {
    RecordFallback(kActRaiserActionBgFallback_NativeTilemap, layer,
                   map_group, map_number, &snapshot);
    return;
  }

  /* The observer must never publish through the provider's world. The PPU
   * binding retains that object's address until scanout, so sharing it here
   * would replace an immutable room-scene publication with live WRAM after
   * preflight and force both sources to decode again on every frame. */
  ActionBgWorld *world = WorldForLayer(
      s_observer.comparison_world, layer, map_group, map_number);
  if (!world) return;
  const uint32_t before = ActionBgWorld_Serial(world);
  if (!ActionBgWorld_Update(world, &snapshot.decode)) {
    RecordFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                   map_group, map_number, &snapshot);
    return;
  }
  const uint32_t serial = ActionBgWorld_Serial(world);
  if (serial != before) s_observer.diagnostics.layer_activations++;
  CompareRoomSceneWorld(layer, map_group, map_number, world);

  ActRaiserActionBgCompareResult comparison;
  if (!ActRaiserActionBg_CompareLayer(
          world, &snapshot, vram->data, (size_t)vram->element_count,
          wrap_world_x, &comparison)) {
    RecordFallback(kActRaiserActionBgFallback_CompareFailure, layer,
                   map_group, map_number, &snapshot);
    return;
  }
  s_observer.diagnostics.layers_compared++;
  s_observer.diagnostics.tiles_compared += comparison.compared;
  s_observer.diagnostics.mismatches += comparison.mismatches;
  s_observer.diagnostics.outside_world += comparison.outside_world;
  ReportComparison(
      wram, layer, map_group, map_number, &snapshot, serial, &comparison,
      s_observer.comparison_reported_outside_serial,
      s_observer.comparison_reported_mismatch_serial);
}

void ActRaiserActionBg_ObserveFrame(const uint8_t *wram, size_t wram_size) {
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram;
  const bool compare_world = CompareEnabled();
  const bool compare_stage = RoomSceneStageCompareEnabled();
  if ((!compare_world && !compare_stage) || !wram ||
      !QueryPpuState(&ppu) ||
      (compare_world && !BorrowVram(ppu.lifetime_generation, &vram)) ||
      !SyncFrameIdentity(wram, wram_size))
    return;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  if (compare_world) s_observer.diagnostics.frames_observed++;

  if ((ppu.flags & SR_PPU_STATE_FORCED_BLANK) != 0u) {
    if (!s_observer.forced_blank) ResetWorlds();
    s_observer.forced_blank = true;
    if (compare_world)
      for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
        RecordFallback(kActRaiserActionBgFallback_ForcedBlank, layer,
                       map_group, map_number, NULL);
    return;
  }
  s_observer.forced_blank = false;
  if (ppu.bg_mode != 1u) {
    if (compare_world)
      for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
        RecordFallback(kActRaiserActionBgFallback_WrongMode, layer,
                       map_group, map_number, NULL);
    return;
  }

  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
    CompareRoomSceneStageOnce(
        wram, wram_size, layer, map_group, map_number);
  if (!compare_world) return;

  /* Keep the compare-only observer on the same topology as the production
   * provider. Otherwise Marahna 0501 would falsely report a finite edge after
   * camera X=512 even though its native ring publishes BG2 modulo 512. */
  ActionBgPlan plan;
  ActionBgPresentationPolicy presentation;
  const bool have_plan = ActRaiserActionBg_BuildPlan(
      wram, wram_size, &ppu, true, &plan, &presentation);
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
    ObserveLayer(wram, wram_size, &ppu, &vram, layer, map_group, map_number,
                 have_plan && plan.layer[layer].wrap_world_x);
}

const ActRaiserActionBgDiagnostics *ActRaiserActionBg_GetDiagnostics(void) {
  return &s_observer.diagnostics;
}

void ActRaiserActionBg_Shutdown(void) {
  if (s_observer.enabled > 0 && s_observer.diagnostics.frames_observed) {
    fprintf(stderr,
            "[action-bg-hle] summary frames=%" PRIu64
            " activations=%" PRIu64 " layers=%" PRIu64
            " tiles=%" PRIu64 " mismatches=%" PRIu64
            " outside=%" PRIu64
            " fallbacks={blank:%" PRIu64 ",mode:%" PRIu64
            ",disabled:%" PRIu64 ",native:%" PRIu64
            ",invalid:%" PRIu64 ",alloc:%" PRIu64
            ",phase:%" PRIu64 ",edge:%" PRIu64
            ",compare:%" PRIu64 "}\n",
            s_observer.diagnostics.frames_observed,
            s_observer.diagnostics.layer_activations,
            s_observer.diagnostics.layers_compared,
            s_observer.diagnostics.tiles_compared,
            s_observer.diagnostics.mismatches,
            s_observer.diagnostics.outside_world,
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_ForcedBlank],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_WrongMode],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_LayerDisabled],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_NativeTilemap],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_InvalidSource],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_Allocation],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_ScrollPhase],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_AuthenticEdge],
            s_observer.diagnostics
                .fallbacks[kActRaiserActionBgFallback_CompareFailure]);
  }
  if (s_observer.hle_enabled > 0 &&
      s_observer.diagnostics.provider_frames) {
    fprintf(stderr,
            "[action-bg-hle] provider-summary frames=%" PRIu64
            " preflight={layers:%" PRIu64 ",tiles:%" PRIu64
            ",mismatches:%" PRIu64 ",outside:%" PRIu64 "}"
            " eligible=%" PRIu64 " layers=%" PRIu64
            " lookups=%" PRIu64 " batches=%" PRIu64
            " tiles=%" PRIu64 " outside=%" PRIu64
            " band-cache={builds:%" PRIu64 ",hits:%" PRIu64 "}\n",
            s_observer.diagnostics.provider_frames,
            s_observer.diagnostics.provider_preflight_layers,
            s_observer.diagnostics.provider_preflight_tiles,
            s_observer.diagnostics.provider_preflight_mismatches,
            s_observer.diagnostics.provider_preflight_outside_world,
            s_observer.diagnostics.provider_eligible_layers,
            s_observer.diagnostics.provider_layers,
            s_observer.diagnostics.provider_lookups,
            s_observer.diagnostics.provider_batches,
            s_observer.diagnostics.provider_tiles,
            s_observer.diagnostics.provider_outside_world,
            s_observer.diagnostics.provider_tile_band_cache_builds,
            s_observer.diagnostics.provider_tile_band_cache_hits);
  }
  if (s_observer.room_scene_compare_enabled > 0) {
    fprintf(stderr,
            "[action-room-scene] summary loads=%" PRIu64
            " load-failures=%" PRIu64 " layers=%" PRIu64
            " tiles=%" PRIu64 " mismatches=%" PRIu64
            " frames=%" PRIu64 " raster-holds=%" PRIu64
            " scanlines=%" PRIu64
            " registers=%" PRIu64 " register-mismatches=%" PRIu64 "\n",
            s_observer.diagnostics.room_scene_loads,
            s_observer.diagnostics.room_scene_load_failures,
            s_observer.diagnostics.room_scene_layers_compared,
            s_observer.diagnostics.room_scene_tiles_compared,
            s_observer.diagnostics.room_scene_mismatches,
            s_observer.diagnostics.room_scene_frames_built,
            s_observer.diagnostics.room_scene_raster_hold_frames,
            s_observer.diagnostics.room_scene_scanlines_compared,
            s_observer.diagnostics.room_scene_registers_compared,
            s_observer.diagnostics.room_scene_register_mismatches);
    fprintf(stderr,
            "[action-room-scene] mismatch-fields="
            "{BG1HOFS:%" PRIu64 ",BG1VOFS:%" PRIu64
            ",BG2HOFS:%" PRIu64 ",BG2VOFS:%" PRIu64
            ",MOSAIC:%" PRIu64 ",TM:%" PRIu64 ",TS:%" PRIu64
            ",TMW:%" PRIu64 ",TSW:%" PRIu64
            ",CGWSEL:%" PRIu64 ",CGADSUB:%" PRIu64
            ",BGMODE:%" PRIu64 ",BG1SC:%" PRIu64
            ",BG2SC:%" PRIu64 "}\n",
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bg1HScroll],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bg1VScroll],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bg2HScroll],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bg2VScroll],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Mosaic],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_MainScreen],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_SubScreen],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_MainWindow],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_SubWindow],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Cgwsel],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Cgadsub],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bgmode],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bg1Sc],
            s_observer.room_scene_register_mismatches_by_field[
                kActRaiserActionRoomSceneFrameField_Bg2Sc]);
  }
  if (s_observer.room_scene_stage_compare_enabled > 0) {
    fprintf(stderr,
            "[action-room-stage] summary layers=%" PRIu64
            " bytes=%" PRIu64 " mismatches=%" PRIu64 "\n",
            s_observer.diagnostics.room_scene_stage_layers_compared,
            s_observer.diagnostics.room_scene_stage_bytes_compared,
            s_observer.diagnostics.room_scene_stage_mismatches);
  }
  if (s_observer.room_scene_hle_enabled > 0) {
    fprintf(stderr,
            "[action-room-scene] provider-summary layers=%" PRIu64
            " live-fallbacks=%" PRIu64 "\n",
            s_observer.diagnostics.room_scene_hle_layers,
            s_observer.diagnostics.room_scene_hle_fallbacks);
  }
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActionBgWorld_Destroy(s_observer.provider_world[layer]);
    ActionBgWorld_Destroy(s_observer.comparison_world[layer]);
    free(s_provider[layer].tile_band_cache);
  }
  for(unsigned terrain=0;terrain<kDioramaTerrainProfileCount;++terrain)for(unsigned room=0;room<2;++room) {
    free(s_trunk_rooms[terrain][room]);s_trunk_rooms[terrain][room]=NULL;
  }
  ResetVoxel();
  free(s_voxel.source); free(s_voxel.tiles);
  memset(&s_voxel, 0, sizeof(s_voxel));
  free(s_observer.room_scene);
  memset(&s_observer, 0, sizeof(s_observer));
  s_observer.enabled = -1;
  s_observer.hle_enabled = -1;
  s_observer.room_scene_hle_enabled = -1;
  s_observer.room_scene_compare_enabled = -1;
  s_observer.room_scene_stage_compare_enabled = -1;
  s_observer.room_scene_compare_verbose = -1;
  memset(s_provider, 0, sizeof(s_provider));
}
