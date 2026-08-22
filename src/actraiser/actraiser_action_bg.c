#include "actraiser_action_bg.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "action/action_room_scene.h"
#include "diorama/diorama_layer_order.h"
#include "snes/dma.h"
#include "snes/ppu.h"

enum {
  kActionBgLayerCount = 2,
  kActionBgRingTiles = 64,
  kActionBgRingWords = kActionBgRingTiles * kActionBgRingTiles,
};

typedef struct ActRaiserActionBgObserver {
  ActionBgWorld *provider_world[kActionBgLayerCount];
  ActionBgWorld *comparison_world[kActionBgLayerCount];
  ActRaiserActionBgDiagnostics diagnostics;
  uint32_t provider_reported_mismatch_serial[kActionBgLayerCount];
  uint32_t provider_reported_outside_serial[kActionBgLayerCount];
  uint32_t comparison_reported_mismatch_serial[kActionBgLayerCount];
  uint32_t comparison_reported_outside_serial[kActionBgLayerCount];
  uint32_t room_scene_compared_serial[kActionBgLayerCount];
  const uint8_t *rom;
  size_t rom_size;
  ActionRoomScene *room_scene;
  ActionRoomSceneFrameState room_frame;
  ActionRoomSceneFrameState previous_room_frame;
  uint16_t last_game_frame;
  uint16_t last_room_camera_x;
  uint16_t prior_room_camera_x;
  uint16_t room_frame_current_camera_x;
  uint16_t room_frame_raster_camera_x;
  uint16_t room_frame_prior_camera_x;
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
  bool room_camera_valid;
  bool reported_room_frame_mismatch;
  bool reported_room_scene_hle_fallback[kActionBgLayerCount];
  bool forced_blank;
  int enabled;
  int hle_enabled;
  int room_scene_hle_enabled;
  int room_scene_compare_enabled;
  int room_scene_compare_verbose;
  unsigned room_scene_verbose_reports;
} ActRaiserActionBgObserver;

typedef struct ActRaiserActionBgProvider {
  const ActionBgWorld *world;
  const DioramaRoomOverride *virtual_room;
  bool wrap_world_x;
  uint8_t layer;
} ActRaiserActionBgProvider;

static ActRaiserActionBgObserver s_observer = {
  .enabled = -1,
  .hle_enabled = -1,
  .room_scene_hle_enabled = -1,
  .room_scene_compare_enabled = -1,
  .room_scene_compare_verbose = -1,
};
static ActRaiserActionBgProvider s_provider[kActionBgLayerCount];

_Static_assert(kActionBgLayerCount == 2,
               "ActRaiser action HLE currently owns BG1/BG2 only");
_Static_assert(kActRaiserBgLayerStateStride == 4,
               "background capture offsets assume a four-byte layer stride");

static uint16_t ReadWram16(const uint8_t *wram, size_t address) {
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

void ActRaiserActionBg_ResolveVerticalMargins(
    int camera_y, int world_height, int budget,
    int *top, int *bottom) {
  if (budget < 0) budget = 0;
  int available_top = camera_y > 0 ? camera_y : 0;
  int available_bottom =
      world_height - kActRaiserActionCameraViewportHeight - camera_y;
  if (available_bottom < 0) available_bottom = 0;
  if (available_top > budget) available_top = budget;
  if (available_bottom > budget) available_bottom = budget;
  if (top) *top = available_top;
  if (bottom) *bottom = available_bottom;
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
    const uint8_t *wram, size_t wram_size, const struct Ppu *ppu,
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
    .death_heim_ending_state = wram[kActRaiserWram_DeathHeimEndingState],
    .decorative_padding_enabled = decorative_padding_enabled,
  };
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActRaiserActionBgLayerSnapshot snapshot;
    if (!ActRaiserActionBg_CaptureLayer(
            wram, wram_size, layer, ppu->bgXsc[layer], &snapshot))
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

static uint16_t PpuExtentCap(ActionBgExtentMode mode, uint16_t fixed) {
  return mode == kActionBgExtent_Fixed
      ? fixed : kPpuWidescreenExtentAvailable;
}

static bool HorizontalExtentsEqual(const ActionBgHorizontalExtent *a,
                                   const ActionBgHorizontalExtent *b) {
  return a->mode == b->mode && a->left == b->left && a->right == b->right;
}

bool ActRaiserActionBg_ApplyPlanExtents(const ActionBgPlan *plan, Ppu *ppu) {
  if (!ppu || !ActionBgPlan_Validate(plan)) return false;
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
      PpuSetWidescreenLayerExtent(
          ppu, (uint8_t)layer,
          PpuExtentCap(horizontal->mode, horizontal->left),
          PpuExtentCap(horizontal->mode, horizontal->right),
          PpuExtentCap(vertical->mode, vertical->top),
          PpuExtentCap(vertical->mode, vertical->bottom));
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
        PpuSetWidescreenLayerExtentBand(
            ppu, (uint8_t)layer, (uint8_t)y0, (uint8_t)y1,
            PpuExtentCap(band->horizontal_extent.mode,
                         band->horizontal_extent.left),
            PpuExtentCap(band->horizontal_extent.mode,
                         band->horizontal_extent.right));
      }
    }
  }
  return true;
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

bool ActRaiserActionBg_InitRoomScenes(const uint8_t *rom, size_t rom_size) {
  s_observer.rom = rom;
  s_observer.rom_size = rom_size;
  s_observer.room_scene_valid = false;
  s_observer.room_scene_attempted = false;
  s_observer.room_frame_valid = false;
  s_observer.previous_room_frame_valid = false;
  s_observer.room_frame_uses_previous = false;
  s_observer.room_camera_valid = false;
  s_observer.reported_room_frame_mismatch = false;
  memset(s_observer.reported_room_scene_hle_fallback, 0,
         sizeof(s_observer.reported_room_scene_hle_fallback));
  s_observer.room_scene_verbose_reports = 0;
  memset(s_observer.room_scene_compared_serial, 0,
         sizeof(s_observer.room_scene_compared_serial));
  if (!rom || !rom_size) return false;
  if (!s_observer.room_scene)
    s_observer.room_scene = calloc(1, sizeof(*s_observer.room_scene));
  return s_observer.room_scene != NULL;
}

static void ResetWorlds(void) {
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActionBgWorld_Reset(s_observer.provider_world[layer]);
    ActionBgWorld_Reset(s_observer.comparison_world[layer]);
    s_observer.provider_reported_mismatch_serial[layer] = 0;
    s_observer.provider_reported_outside_serial[layer] = 0;
    s_observer.comparison_reported_mismatch_serial[layer] = 0;
    s_observer.comparison_reported_outside_serial[layer] = 0;
    s_observer.room_scene_compared_serial[layer] = 0;
  }
  s_observer.room_scene_valid = false;
  s_observer.room_scene_attempted = false;
  s_observer.room_frame_valid = false;
  s_observer.previous_room_frame_valid = false;
  s_observer.room_frame_uses_previous = false;
  s_observer.room_camera_valid = false;
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
                            map_group, map_number)) {
    s_observer.diagnostics.room_scene_load_failures++;
    fprintf(stderr,
            "[action-room-scene] load failed map=%02X/%02X\n",
            map_group, map_number);
    return false;
  }
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
}

bool ActRaiserActionBg_CompareRoomSceneFrameLine(
    const ActionRoomSceneFrameState *state, const Ppu *ppu,
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
      state->bg_hscroll[0][output_y], ppu->hScroll[0] & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg1VScroll,
      state->bg_vscroll[0][output_y], ppu->vScroll[0] & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg2HScroll,
      state->bg_hscroll[1][output_y], ppu->hScroll[1] & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Bg2VScroll,
      state->bg_vscroll[1][output_y], ppu->vScroll[1] & 0x03ffu);
  CompareRoomFrameValue(
      &built, kActRaiserActionRoomSceneFrameField_Mosaic,
      state->mosaic[output_y], ppu->mosaic);

  if (output_y == 0) {
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_MainScreen,
        state->screen_enabled[0], ppu->screenEnabled[0]);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_SubScreen,
        state->screen_enabled[1], ppu->screenEnabled[1]);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_MainWindow,
        state->screen_windowed[0], ppu->screenWindowed[0]);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_SubWindow,
        state->screen_windowed[1], ppu->screenWindowed[1]);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Cgwsel,
        state->cgwsel, ppu->cgwsel);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Cgadsub,
        state->cgadsub, ppu->cgadsub);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Bgmode,
        state->bgmode, ppu->bgmode);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Bg1Sc,
        state->bgsc[0], ppu->bgXsc[0]);
    CompareRoomFrameValue(
        &built, kActRaiserActionRoomSceneFrameField_Bg2Sc,
        state->bgsc[1], ppu->bgXsc[1]);
  }
  *result = built;
  return true;
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
  s_observer.map_group = map_group;
  s_observer.map_number = map_number;
  s_observer.map_valid = true;
  s_observer.last_game_frame = game_frame;
  s_observer.frame_valid = true;
  return true;
}

void ActRaiserActionBg_BeginRoomSceneFrame(
    const uint8_t *wram, size_t wram_size, const Ppu *ppu,
    const Dma *dma) {
  s_observer.previous_room_frame_valid = s_observer.room_frame_valid;
  if (s_observer.previous_room_frame_valid)
    s_observer.previous_room_frame = s_observer.room_frame;
  s_observer.room_frame_valid = false;
  s_observer.room_frame_uses_previous = false;
  if (!RoomSceneCompareEnabled() || !wram || !ppu ||
      (ppu->inidisp & 0x80u) || (ppu->bgmode & 7u) != 1u ||
      wram_size <= kActRaiserWram_Bg1CameraY + 1u ||
      !SyncFrameIdentity(wram, wram_size))
    return;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  if (!EnsureRoomScene(map_group, map_number)) return;
  /* Loading/fade frames can already carry the destination room identity while
   * another effect still owns channel 2. Match the complete raster DMA
   * contract—not only HDMAEN—before treating the frame as stable. This also
   * pins the non-default R7 `$6800` and R9 `$7000` table bases. */
  if (s_observer.room_scene->raster_preset != kActionRoomRaster_None) {
    if (!dma) return;
    uint16_t table = 0x6000;
    uint8_t target = 0x0f;
    uint8_t mode = 2;
    switch (s_observer.room_scene->raster_preset) {
      case kActionRoomRaster_R3: target = 0x10; break;
      case kActionRoomRaster_R4: target = 0x06; mode = 0; break;
      case kActionRoomRaster_R7: table = 0x6800; break;
      case kActionRoomRaster_R9: table = 0x7000; break;
      default: break;
    }
    const DmaChannel *channel = &dma->channel[2];
    if (!channel->hdmaActive || channel->indirect ||
        channel->mode != mode || channel->bAdr != target ||
        channel->aBank != 0x7e || channel->aAdr != table)
      return;
    if (s_observer.room_scene->raster_preset == kActionRoomRaster_R10) {
      channel = &dma->channel[3];
      if (!channel->hdmaActive || channel->indirect ||
          channel->mode != 2 || channel->bAdr != 0x0d ||
          channel->aBank != 0x7e || channel->aAdr != 0x6800)
        return;
    }
  }
  const uint16_t camera_x =
      ReadWram16(wram, kActRaiserWram_Bg1CameraX);
  const uint16_t raster_camera_x = s_observer.room_camera_valid
      ? s_observer.last_room_camera_x : camera_x;
  const ActionRoomSceneFrameRequest request = {
    .camera_x = camera_x,
    .camera_y = ReadWram16(wram, kActRaiserWram_Bg1CameraY),
    .raster_camera_x = raster_camera_x,
    .game_frame = ReadWram16(wram, kActRaiserWram_GameFrame),
    .animation_phase = -1,
    .page_phase = -1,
    .have_raster_camera_x = true,
  };
  if (!ActionRoomScene_BuildFrameState(
          s_observer.room_scene, &request, &s_observer.room_frame))
    return;
  s_observer.room_frame_current_camera_x = camera_x;
  s_observer.room_frame_raster_camera_x = raster_camera_x;
  s_observer.room_frame_prior_camera_x = s_observer.room_camera_valid
      ? s_observer.prior_room_camera_x : camera_x;
  s_observer.prior_room_camera_x = s_observer.last_room_camera_x;
  s_observer.last_room_camera_x = camera_x;
  s_observer.room_camera_valid = true;
  s_observer.room_frame_valid = true;
  s_observer.diagnostics.room_scene_frames_built++;
}

void ActRaiserActionBg_ObserveRoomSceneFrameLine(
    const Ppu *ppu, unsigned output_y) {
  if (!s_observer.room_frame_valid || !ppu) return;
  ActRaiserActionRoomSceneFrameCompareResult comparison;
  const ActionRoomSceneFrameState *compared_frame =
      s_observer.room_frame_uses_previous
      ? &s_observer.previous_room_frame : &s_observer.room_frame;
  if (!ActRaiserActionBg_CompareRoomSceneFrameLine(
          compared_frame, ppu, output_y, &comparison))
    return;
  /* The raster callback is an action-update callback, not a presentation
   * callback. Hit-stop and similar skipped action updates intentionally leave
   * the persistent HDMA table untouched for one displayed frame. If the new
   * deterministic table disagrees but the complete preceding table agrees,
   * follow that retained table for the rest of this scanout. */
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
  s_observer.diagnostics.room_scene_scanlines_compared++;
  s_observer.diagnostics.room_scene_registers_compared += comparison.compared;
  s_observer.diagnostics.room_scene_register_mismatches +=
      comparison.mismatches;
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

static bool ProviderLookup(const void *context, int32_t tile_x,
                           int32_t tile_y, uint16_t *entry) {
  const ActRaiserActionBgProvider *provider = context;
  s_observer.diagnostics.provider_lookups++;
  const ActionBgLookupResult result = LookupWorldTile(
      provider ? provider->world : NULL,
      provider && provider->wrap_world_x, tile_x, tile_y, entry);
  if (result == kActionBgLookup_Tile) {
    s_observer.diagnostics.provider_tiles++;
    return true;
  }
  if (result == kActionBgLookup_OutsideWorld)
    s_observer.diagnostics.provider_outside_world++;
  return false;
}

static bool ProviderBandLookup(const void *context, int32_t tile_x,
                               int32_t tile_y, uint16_t entry,
                               uint8_t *band) {
  const ActRaiserActionBgProvider *provider = context;
  if (!provider || !provider->world || !provider->virtual_room || !band ||
      provider->layer >= kActionBgLayerCount ||
      !DioramaLayerOrder_VirtualLayerIsAuthored(
          &provider->virtual_room->virtual_layers[provider->layer]))
    return false;
  if (provider->wrap_world_x)
    tile_x = WrapWorldTile(tile_x, ActionBgWorld_TileWidth(provider->world));
  uint8_t metatile = 0;
  if (!ActionBgWorld_LookupMetatile(
          provider->world, tile_x, tile_y, &metatile))
    return false;
  *band = (uint8_t)DioramaLayerOrder_VirtualBand(
      provider->virtual_room, provider->layer, tile_x >> 1, tile_y >> 1,
      metatile, entry);
  return *band < kDioramaVirtualBandCount;
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
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    struct Ppu *ppu) {
  return ActRaiserActionBg_BindPlanWithVirtualLayers(
      wram, wram_size, plan, NULL, ppu);
}

uint8_t ActRaiserActionBg_BindPlanWithVirtualLayers(
    const uint8_t *wram, size_t wram_size, const ActionBgPlan *plan,
    const struct DioramaRoomOverride *virtual_room, struct Ppu *ppu) {
  PpuClearVirtualTilemaps(ppu);
  if (!ActRaiserActionBg_HleEnabled() ||
      !wram || !ppu || !plan || !plan->valid ||
      !SyncFrameIdentity(wram, wram_size))
    return 0;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  if (!ActRaiser_IsActionMapGroup(map_group)) return 0;
  s_observer.diagnostics.provider_frames++;
  if (ppu->inidisp & 0x80) {
    if (!s_observer.forced_blank) ResetWorlds();
    s_observer.forced_blank = true;
    for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
      if (plan->layer[layer].source == kActionBgSource_WorldMap)
        RecordProviderFallback(kActRaiserActionBgFallback_ForcedBlank, layer,
                               map_group, map_number, NULL);
    return 0;
  }
  s_observer.forced_blank = false;
  if ((ppu->bgmode & 7u) != 1u) {
    for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
      if (plan->layer[layer].source == kActionBgSource_WorldMap)
        RecordProviderFallback(kActRaiserActionBgFallback_WrongMode, layer,
                               map_group, map_number, NULL);
    return 0;
  }

  uint8_t bound = 0;
  const uint8_t enabled = ppu->screenEnabled[0] | ppu->screenEnabled[1];
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    const ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    if (!layer_plan->valid ||
        layer_plan->source != kActionBgSource_WorldMap)
      continue;
    ActRaiserActionBgLayerSnapshot snapshot;
    if (!ActRaiserActionBg_CaptureLayer(
            wram, wram_size, layer, ppu->bgXsc[layer], &snapshot) ||
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
            &snapshot, sizeof(ppu->vram) / sizeof(ppu->vram[0]))) {
      RecordProviderFallback(kActRaiserActionBgFallback_NativeTilemap, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    if ((snapshot.camera_x & 0x3ffu) !=
            (ppu->hScroll[layer] & 0x3ffu) ||
        (snapshot.camera_y & 0x3ffu) !=
            (ppu->vScroll[layer] & 0x3ffu)) {
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
            world, &snapshot, ppu->vram,
            sizeof(ppu->vram) / sizeof(ppu->vram[0]),
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
    const PpuVirtualTilemapBinding binding = {
      .lookup = ProviderLookup,
      .band_lookup = ProviderBandLookup,
      .context = &s_provider[layer],
      .camera_x = snapshot.camera_x,
      .camera_y = snapshot.camera_y,
      .hscroll_anchor = (uint16_t)(ppu->hScroll[layer] & 0x3ff),
      .vscroll_anchor = (uint16_t)(ppu->vScroll[layer] & 0x3ff),
      .flags = include_authentic
          ? kPpuVirtualTilemapFlag_IncludeAuthentic : 0,
    };
    if (!PpuSetVirtualTilemap(ppu, (uint8_t)layer, &binding)) {
      RecordProviderFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    bound |= (uint8_t)(1u << layer);
    s_observer.diagnostics.provider_layers++;
  }
  return bound;
}

static void ObserveLayer(const uint8_t *wram, size_t wram_size,
                         const Ppu *ppu, unsigned layer,
                         uint8_t map_group, uint8_t map_number,
                         bool wrap_world_x) {
  ActRaiserActionBgLayerSnapshot snapshot;
  if (!ActRaiserActionBg_CaptureLayer(
          wram, wram_size, layer, ppu->bgXsc[layer], &snapshot)) {
    RecordFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                   map_group, map_number, NULL);
    return;
  }
  const uint8_t enabled = ppu->screenEnabled[0] | ppu->screenEnabled[1];
  if (!(enabled & (1u << layer))) {
    RecordFallback(kActRaiserActionBgFallback_LayerDisabled, layer,
                   map_group, map_number, &snapshot);
    return;
  }
  if (!ActRaiserActionBg_WorldRingEligible(&snapshot,
                                           sizeof(ppu->vram) /
                                               sizeof(ppu->vram[0]))) {
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
          world, &snapshot, ppu->vram,
          sizeof(ppu->vram) / sizeof(ppu->vram[0]),
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

void ActRaiserActionBg_ObserveFrame(const uint8_t *wram, size_t wram_size,
                                    const struct Ppu *ppu) {
  if (!CompareEnabled() || !wram || !ppu ||
      !SyncFrameIdentity(wram, wram_size))
    return;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  s_observer.diagnostics.frames_observed++;

  if (ppu->inidisp & 0x80) {
    if (!s_observer.forced_blank) ResetWorlds();
    s_observer.forced_blank = true;
    for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
      RecordFallback(kActRaiserActionBgFallback_ForcedBlank, layer,
                     map_group, map_number, NULL);
    return;
  }
  s_observer.forced_blank = false;
  if ((ppu->bgmode & 7u) != 1u) {
    for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
      RecordFallback(kActRaiserActionBgFallback_WrongMode, layer,
                     map_group, map_number, NULL);
    return;
  }

  /* Keep the compare-only observer on the same topology as the production
   * provider. Otherwise Marahna 0501 would falsely report a finite edge after
   * camera X=512 even though its native ring publishes BG2 modulo 512. */
  ActionBgPlan plan;
  ActionBgPresentationPolicy presentation;
  const bool have_plan = ActRaiserActionBg_BuildPlan(
      wram, wram_size, ppu, true, &plan, &presentation);
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
    ObserveLayer(wram, wram_size, ppu, layer, map_group, map_number,
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
            " lookups=%" PRIu64
            " tiles=%" PRIu64 " outside=%" PRIu64 "\n",
            s_observer.diagnostics.provider_frames,
            s_observer.diagnostics.provider_preflight_layers,
            s_observer.diagnostics.provider_preflight_tiles,
            s_observer.diagnostics.provider_preflight_mismatches,
            s_observer.diagnostics.provider_preflight_outside_world,
            s_observer.diagnostics.provider_eligible_layers,
            s_observer.diagnostics.provider_layers,
            s_observer.diagnostics.provider_lookups,
            s_observer.diagnostics.provider_tiles,
            s_observer.diagnostics.provider_outside_world);
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
  }
  free(s_observer.room_scene);
  memset(&s_observer, 0, sizeof(s_observer));
  s_observer.enabled = -1;
  s_observer.hle_enabled = -1;
  s_observer.room_scene_hle_enabled = -1;
  s_observer.room_scene_compare_enabled = -1;
  s_observer.room_scene_compare_verbose = -1;
  memset(s_provider, 0, sizeof(s_provider));
}
