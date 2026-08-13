#include "actraiser_action_bg.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "snes/ppu.h"

enum {
  kActionBgLayerCount = 2,
  kActionBgRingTiles = 64,
  kActionBgRingWords = kActionBgRingTiles * kActionBgRingTiles,
};

typedef struct ActRaiserActionBgObserver {
  ActionBgWorld *world[kActionBgLayerCount];
  ActRaiserActionBgDiagnostics diagnostics;
  uint32_t reported_mismatch_serial[kActionBgLayerCount];
  uint32_t reported_outside_serial[kActionBgLayerCount];
  uint16_t last_game_frame;
  uint8_t map_group;
  uint8_t map_number;
  bool reported_fallback[kActRaiserActionBgFallback_Count];
  bool frame_valid;
  bool map_valid;
  bool forced_blank;
  int enabled;
  int hle_enabled;
} ActRaiserActionBgObserver;

typedef struct ActRaiserActionBgProvider {
  const ActionBgWorld *world;
  bool wrap_world_x;
} ActRaiserActionBgProvider;

static ActRaiserActionBgObserver s_observer = {
  .enabled = -1,
  .hle_enabled = -1,
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

static void ResetWorlds(void) {
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActionBgWorld_Reset(s_observer.world[layer]);
    s_observer.reported_mismatch_serial[layer] = 0;
    s_observer.reported_outside_serial[layer] = 0;
  }
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

static ActionBgWorld *WorldForLayer(unsigned layer, uint8_t map_group,
                                    uint8_t map_number) {
  if (s_observer.world[layer]) return s_observer.world[layer];
  s_observer.world[layer] = ActionBgWorld_Create();
  if (!s_observer.world[layer])
    RecordFallback(kActRaiserActionBgFallback_Allocation, layer,
                   map_group, map_number, NULL);
  return s_observer.world[layer];
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

static void ReportComparison(
    const uint8_t *wram, unsigned layer, uint8_t map_group,
    uint8_t map_number, const ActRaiserActionBgLayerSnapshot *snapshot,
    uint32_t serial, const ActRaiserActionBgCompareResult *comparison) {
  if (comparison->outside_world &&
      s_observer.reported_outside_serial[layer] != serial) {
    s_observer.reported_outside_serial[layer] = serial;
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
      s_observer.reported_mismatch_serial[layer] != serial) {
    s_observer.reported_mismatch_serial[layer] = serial;
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
    ActionBgWorld *world = WorldForLayer(layer, map_group, map_number);
    if (!world) continue;
    const uint32_t before = ActionBgWorld_Serial(world);
    if (!ActionBgWorld_Update(world, &snapshot.decode)) {
      RecordProviderFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                             map_group, map_number, &snapshot);
      continue;
    }
    if (ActionBgWorld_Serial(world) != before)
      s_observer.diagnostics.layer_activations++;
    const uint32_t serial = ActionBgWorld_Serial(world);
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
    ReportComparison(wram, layer, map_group, map_number, &snapshot, serial,
                     &comparison);
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
    s_provider[layer].wrap_world_x = layer_plan->wrap_world_x;
    const PpuVirtualTilemapBinding binding = {
      .lookup = ProviderLookup,
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

  ActionBgWorld *world = WorldForLayer(layer, map_group, map_number);
  if (!world) return;
  const uint32_t before = ActionBgWorld_Serial(world);
  if (!ActionBgWorld_Update(world, &snapshot.decode)) {
    RecordFallback(kActRaiserActionBgFallback_InvalidSource, layer,
                   map_group, map_number, &snapshot);
    return;
  }
  const uint32_t serial = ActionBgWorld_Serial(world);
  if (serial != before) s_observer.diagnostics.layer_activations++;

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
  ReportComparison(wram, layer, map_group, map_number, &snapshot, serial,
                   &comparison);
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
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
    ActionBgWorld_Destroy(s_observer.world[layer]);
  memset(&s_observer, 0, sizeof(s_observer));
  s_observer.enabled = -1;
  s_observer.hle_enabled = -1;
  memset(s_provider, 0, sizeof(s_provider));
}
