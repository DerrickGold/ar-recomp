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
  uint16_t last_game_frame;
  uint8_t map_group;
  uint8_t map_number;
  bool reported_fallback[kActRaiserActionBgFallback_Count];
  bool frame_valid;
  bool map_valid;
  bool forced_blank;
  int enabled;
} ActRaiserActionBgObserver;

static ActRaiserActionBgObserver s_observer = { .enabled = -1 };

_Static_assert(kActionBgLayerCount == 2,
               "ActRaiser action HLE currently owns BG1/BG2 only");
_Static_assert(kActRaiserBgLayerStateStride == 4,
               "background capture offsets assume a four-byte layer stride");

static uint16_t ReadWram16(const uint8_t *wram, size_t address) {
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
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

bool ActRaiserActionBg_CompareLayer(
    const ActionBgWorld *world,
    const ActRaiserActionBgLayerSnapshot *snapshot,
    const uint16_t *vram, size_t vram_words,
    ActRaiserActionBgCompareResult *result) {
  if (result) {
    memset(result, 0, sizeof(*result));
    result->first_tile_x = -1;
    result->first_tile_y = -1;
  }
  if (!world || !snapshot || !vram || !result ||
      !ActRaiserActionBg_WorldRingEligible(snapshot, vram_words))
    return false;

  ActRaiserActionBgCompareResult built = {
    .first_tile_x = -1,
    .first_tile_y = -1,
  };
  const int first_x = snapshot->camera_x >> 3;
  const int first_y = snapshot->camera_y >> 3;
  const int last_x = (snapshot->camera_x +
                      kActRaiserAuthenticWidth - 1) >> 3;
  const int last_y = (snapshot->camera_y +
                      kActRaiserAuthenticHeight - 1) >> 3;
  for (int tile_y = first_y; tile_y <= last_y; tile_y++) {
    for (int tile_x = first_x; tile_x <= last_x; tile_x++) {
      uint16_t hle = 0;
      const ActionBgLookupResult lookup =
          ActionBgWorld_Lookup(world, tile_x, tile_y, &hle);
      if (lookup == kActionBgLookup_OutsideWorld) {
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

static const char *FallbackName(ActRaiserActionBgFallbackReason reason) {
  static const char *const names[kActRaiserActionBgFallback_Count] = {
    [kActRaiserActionBgFallback_ForcedBlank] = "forced-blank",
    [kActRaiserActionBgFallback_WrongMode] = "non-mode1",
    [kActRaiserActionBgFallback_LayerDisabled] = "layer-disabled",
    [kActRaiserActionBgFallback_NativeTilemap] = "native-tilemap",
    [kActRaiserActionBgFallback_InvalidSource] = "invalid-world-source",
    [kActRaiserActionBgFallback_Allocation] = "allocation-failure",
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
              "[action-bg-hle] differential observer enabled; rendering "
              "remains native\n");
  }
  return s_observer.enabled != 0;
}

static void ResetWorlds(void) {
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++) {
    ActionBgWorld_Reset(s_observer.world[layer]);
    s_observer.reported_mismatch_serial[layer] = 0;
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

static ActionBgWorld *WorldForLayer(unsigned layer, uint8_t map_group,
                                    uint8_t map_number) {
  if (s_observer.world[layer]) return s_observer.world[layer];
  s_observer.world[layer] = ActionBgWorld_Create();
  if (!s_observer.world[layer])
    RecordFallback(kActRaiserActionBgFallback_Allocation, layer,
                   map_group, map_number, NULL);
  return s_observer.world[layer];
}

static void ObserveLayer(const uint8_t *wram, size_t wram_size,
                         const Ppu *ppu, unsigned layer,
                         uint8_t map_group, uint8_t map_number) {
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
          sizeof(ppu->vram) / sizeof(ppu->vram[0]), &comparison)) {
    RecordFallback(kActRaiserActionBgFallback_CompareFailure, layer,
                   map_group, map_number, &snapshot);
    return;
  }
  s_observer.diagnostics.layers_compared++;
  s_observer.diagnostics.tiles_compared += comparison.compared;
  s_observer.diagnostics.mismatches += comparison.mismatches;
  s_observer.diagnostics.outside_world += comparison.outside_world;
  if (comparison.mismatches &&
      s_observer.reported_mismatch_serial[layer] != serial) {
    s_observer.reported_mismatch_serial[layer] = serial;
    fprintf(stderr,
            "[action-bg-hle] MISMATCH gf=%u map=%02X/%02X BG%u "
            "serial=%u count=%zu/%zu first=(%d,%d) hle=$%04X native=$%04X\n",
            ReadWram16(wram, kActRaiserWram_GameFrame),
            map_group, map_number, layer + 1, serial,
            comparison.mismatches, comparison.compared,
            comparison.first_tile_x, comparison.first_tile_y,
            comparison.first_hle, comparison.first_native);
  }
}

void ActRaiserActionBg_ObserveFrame(const uint8_t *wram, size_t wram_size,
                                    const struct Ppu *ppu) {
  if (!CompareEnabled() || !wram || !ppu ||
      wram_size < kActRaiserWram_GameFrame + 2)
    return;
  const uint8_t map_group = wram[kActRaiserWram_MapGroup];
  const uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  if (!ActRaiser_IsActionMapGroup(map_group)) {
    if (s_observer.map_valid) ActRaiserActionBg_Reset();
    return;
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
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
    ObserveLayer(wram, wram_size, ppu, layer, map_group, map_number);
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
                .fallbacks[kActRaiserActionBgFallback_CompareFailure]);
  }
  for (unsigned layer = 0; layer < kActionBgLayerCount; layer++)
    ActionBgWorld_Destroy(s_observer.world[layer]);
  memset(&s_observer, 0, sizeof(s_observer));
  s_observer.enabled = -1;
}
