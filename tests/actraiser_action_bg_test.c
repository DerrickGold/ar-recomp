/* ROM-free tests for the ActRaiser-specific half of SPEC-bg-hle BH2: capture
 * the two low-WRAM layer records and compare an authentic viewport against the
 * exact 64x64 native VRAM ring layout. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_action_bg.h"
#include "actraiser_game.h"
#include "snes/ppu.h"

static int failures;
#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); failures++; \
} } while (0)

enum {
  kMapStart = 0x2000,
  kTableStart = 0x1000,
  kVramWords = 0x8000,
};

/* This ROM-free adapter target does not link the scanline renderer. The real
 * PPU seam is exercised end-to-end by ppu_render_pipeline_test; these bounded
 * stubs let this target pin ActRaiser's plan/decoder-to-binding decisions. */
void PpuClearVirtualTilemaps(Ppu *ppu) {
  if (ppu) memset(ppu->virtualTilemap, 0, sizeof(ppu->virtualTilemap));
}

bool PpuSetVirtualTilemap(Ppu *ppu, uint8_t layer,
                          const PpuVirtualTilemapBinding *binding) {
  if (!ppu || layer >= 2) return false;
  if (!binding) {
    memset(&ppu->virtualTilemap[layer], 0,
           sizeof(ppu->virtualTilemap[layer]));
    return true;
  }
  if (!binding->lookup || binding->hscroll_anchor > 0x3ff ||
      binding->vscroll_anchor > 0x3ff)
    return false;
  ppu->virtualTilemap[layer] = *binding;
  return true;
}

static void Write16(uint8_t *bytes, size_t address, uint16_t value) {
  bytes[address] = (uint8_t)value;
  bytes[address + 1] = (uint8_t)(value >> 8);
}

static void SetLayerState(uint8_t *wram, unsigned layer) {
  const size_t offset = layer * kActRaiserBgLayerStateStride;
  Write16(wram, kActRaiserWram_Bg1CameraX + offset,
          (uint16_t)(13 + layer * 16));
  Write16(wram, kActRaiserWram_Bg1CameraY + offset,
          (uint16_t)(7 + layer * 16));
  Write16(wram, kActRaiserWram_Bg1Width + offset, 512);
  Write16(wram, kActRaiserWram_Bg1Height + offset, 512);
  Write16(wram, kActRaiserWram_BgMapPage + offset,
          (uint16_t)(kMapStart + layer * 0x400));
  Write16(wram, kActRaiserWram_BgTilemapBase + offset,
          (uint16_t)(0x6000 + layer * 0x1000));
  Write16(wram, kActRaiserWram_BgMetatileTable + offset,
          (uint16_t)(kTableStart + layer * 0x800));
  Write16(wram, kActRaiserWram_BgWordMask + offset, 0xFFFF);
  wram[kActRaiserWram_BgAttributes + offset] = (uint8_t)(layer * 0x20);
}

static void FillLayerSource(uint8_t *wram, unsigned layer) {
  const size_t map = kMapStart + layer * 0x400;
  const size_t table = kTableStart + layer * 0x800;
  for (unsigned page = 0; page < 4; page++) {
    for (unsigned metatile = 0; metatile < 256; metatile++)
      wram[map + page * 256 + metatile] =
          (uint8_t)(page * 37 + metatile);
  }
  for (unsigned id = 0; id < 256; id++) {
    for (unsigned quadrant = 0; quadrant < 4; quadrant++) {
      const uint16_t word = (uint16_t)(((id * 4 + quadrant) & 0x3FF) |
          ((quadrant & 1u) ? 0x4000 : 0) |
          ((quadrant & 2u) ? 0x8000 : 0));
      Write16(wram, table + id * 8 + quadrant * 2, word);
    }
  }
}

static uint8_t *BuildWram(void) {
  uint8_t *wram = calloc(1, kActRaiserWramSize);
  CHECK(wram != NULL);
  if (!wram) return NULL;
  for (unsigned layer = 0; layer < 2; layer++) {
    SetLayerState(wram, layer);
    FillLayerSource(wram, layer);
  }
  return wram;
}

static void TestCapture(void) {
  uint8_t *wram = BuildWram();
  uint8_t short_wram[kActRaiserWram_BgAttributes +
                     kActRaiserBgLayerStateStride] = { 0 };
  ActRaiserActionBgLayerSnapshot bg1, bg2;
  CHECK(ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 0, 0x63, &bg1));
  CHECK(ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 1, 0x73, &bg2));
  CHECK(bg1.camera_x == 13 && bg1.camera_y == 7);
  CHECK(bg2.camera_x == 29 && bg2.camera_y == 23);
  CHECK(bg1.decode.world_width == 512 && bg1.decode.world_height == 512);
  CHECK(bg2.decode.map_page == kMapStart + 0x400);
  CHECK(bg1.decode.metatile_table == kTableStart);
  CHECK(bg2.decode.metatile_table == kTableStart + 0x800);
  CHECK(bg1.tilemap_base == 0x6000 && bg2.tilemap_base == 0x7000);
  CHECK(bg1.bgsc == 0x63 && bg2.bgsc == 0x73);
  CHECK(!ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 2, 0, &bg1));
  CHECK(!ActRaiserActionBg_CaptureLayer(NULL, 0, 0, 0, &bg1));
  CHECK(!ActRaiserActionBg_CaptureLayer(
      short_wram, sizeof(short_wram), 1, 0, &bg1));
  CHECK(!ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 0, 0, NULL));
  free(wram);
}

static void PopulateNativeRing(const ActionBgWorld *world,
                               const ActRaiserActionBgLayerSnapshot *snapshot,
                               uint16_t *vram) {
  for (unsigned tile_y = 0; tile_y < ActionBgWorld_TileHeight(world); tile_y++) {
    for (unsigned tile_x = 0; tile_x < ActionBgWorld_TileWidth(world); tile_x++) {
      uint16_t entry = 0;
      size_t address = 0;
      CHECK(ActionBgWorld_Lookup(world, (int)tile_x, (int)tile_y, &entry) ==
            kActionBgLookup_Tile);
      CHECK(ActRaiserActionBg_RingAddress(snapshot->tilemap_base,
                                          (int)tile_x, (int)tile_y,
                                          kVramWords, &address));
      vram[address] = entry;
    }
  }
}

static void TestRingAndComparison(void) {
  uint8_t *wram = BuildWram();
  uint16_t *vram = calloc(kVramWords, sizeof(*vram));
  ActionBgWorld *world = ActionBgWorld_Create();
  ActRaiserActionBgLayerSnapshot snapshot;
  CHECK(ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 0, 0x63, &snapshot));
  CHECK(ActRaiserActionBg_WorldRingEligible(&snapshot, kVramWords));
  CHECK(ActionBgWorld_Update(world, &snapshot.decode));
  PopulateNativeRing(world, &snapshot, vram);

  ActRaiserActionBgCompareResult result;
  CHECK(ActRaiserActionBg_CompareLayer(
      world, &snapshot, vram, kVramWords, &result));
  /* Camera phases 13/7 expose 33 by 29 unique tile cells. */
  CHECK(result.compared == 33u * 29u);
  CHECK(result.mismatches == 0 && result.outside_world == 0);
  CHECK(result.first_tile_x == -1 && result.first_tile_y == -1);
  CHECK(result.first_outside_tile_x == -1 &&
        result.first_outside_tile_y == -1);

  const int changed_x = snapshot.camera_x >> 3;
  const int changed_y = snapshot.camera_y >> 3;
  size_t changed_address = 0;
  CHECK(ActRaiserActionBg_RingAddress(snapshot.tilemap_base,
                                      changed_x, changed_y,
                                      kVramWords, &changed_address));
  vram[changed_address] ^= 1;
  CHECK(ActRaiserActionBg_CompareLayer(
      world, &snapshot, vram, kVramWords, &result));
  CHECK(result.mismatches == 1);
  CHECK(result.first_tile_x == changed_x && result.first_tile_y == changed_y);
  CHECK(result.first_hle != result.first_native);
  vram[changed_address] ^= 1;

  /* A finite world edge is reported separately, never compared to wrapped
   * native cells and never promoted to provider failure. */
  snapshot.camera_x = 500;
  CHECK(ActRaiserActionBg_CompareLayer(
      world, &snapshot, vram, kVramWords, &result));
  CHECK(result.compared == 2u * 29u);
  CHECK(result.outside_world == 31u * 29u);
  CHECK(result.mismatches == 0);
  CHECK(result.first_outside_tile_x == 64 &&
        result.first_outside_tile_y == 0);

  snapshot.bgsc = 0x62;
  CHECK(!ActRaiserActionBg_WorldRingEligible(&snapshot, kVramWords));
  snapshot.bgsc = 0x73;
  CHECK(!ActRaiserActionBg_WorldRingEligible(&snapshot, kVramWords));
  snapshot.bgsc = 0x63;
  snapshot.tilemap_base = 0x7C00;
  CHECK(!ActRaiserActionBg_WorldRingEligible(&snapshot, kVramWords));
  size_t address = 0;
  CHECK(!ActRaiserActionBg_RingAddress(0x6000, -1, 0,
                                       kVramWords, &address));
  CHECK(!ActRaiserActionBg_RingAddress(0x7FFF, 63, 63,
                                       kVramWords, &address));

  ActionBgWorld_Destroy(world);
  free(vram);
  free(wram);
}

static void TestFramePlanCapture(void) {
  uint8_t *wram = BuildWram();
  Ppu *ppu = calloc(1, sizeof(*ppu));
  CHECK(ppu != NULL);
  if (!wram || !ppu) {
    free(ppu);
    free(wram);
    return;
  }
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 1;
  Write16(wram, kActRaiserWram_Bg2Width, 256);
  ppu->bgXsc[0] = 0x63;
  ppu->bgXsc[1] = 0x70;

  ActionBgPlan plan;
  ActionBgPresentationPolicy policy;
  CHECK(ActRaiserActionBg_BuildPlan(
      wram, kActRaiserWramSize, ppu, true, &plan, &policy));
  CHECK(plan.valid);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(policy.mirror_layers == kActRaiserBgLayerMask_Bg2);
  CHECK(policy.repeat_band_layer == kActRaiserPpuLayer_Bg2);
  CHECK(policy.repeat_band_y0 == 136 && policy.repeat_band_y1 == 224);

  wram[kActRaiserWram_CurrentMap] = 9;
  memset(&plan, 0xA5, sizeof(plan));
  CHECK(!ActRaiserActionBg_BuildPlan(
      wram, kActRaiserWramSize, ppu, true, &plan, &policy));
  CHECK(!plan.valid && policy.repeat_band_layer == -1);
  free(ppu);
  free(wram);
}

static void TestFramePlanBinding(void) {
  uint8_t *wram = BuildWram();
  Ppu *ppu = calloc(1, sizeof(*ppu));
  CHECK(ppu != NULL);
  if (!wram || !ppu) {
    free(ppu);
    free(wram);
    return;
  }
  CHECK(setenv("AR_ACTION_BG_HLE", "1", 1) == 0);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  wram[kActRaiserWram_CurrentMap] = 1;
  Write16(wram, kActRaiserWram_GameFrame, 100);
  /* Narrow BG2 is presentation-owned, so only finite BG1 may bind. */
  Write16(wram, kActRaiserWram_Bg2Width, 256);
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = kActRaiserBgLayerMask_Bg1 |
                          kActRaiserBgLayerMask_Bg2;
  ppu->bgXsc[0] = 0x63;
  ppu->bgXsc[1] = 0x73;
  ppu->hScroll[0] = 0x413;
  ppu->vScroll[0] = 0x807;

  ActionBgPlan plan;
  ActionBgPresentationPolicy policy;
  CHECK(ActRaiserActionBg_BuildPlan(
      wram, kActRaiserWramSize, ppu, true, &plan, &policy));
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) ==
      kActRaiserBgLayerMask_Bg1);
  const PpuVirtualTilemapBinding *binding = &ppu->virtualTilemap[0];
  CHECK(binding->lookup != NULL && binding->context != NULL);
  CHECK(binding->camera_x == 13 && binding->camera_y == 7);
  CHECK(binding->hscroll_anchor == 0x13);
  CHECK(binding->vscroll_anchor == 7);
  CHECK(ppu->virtualTilemap[1].lookup == NULL);

  ActionBgWorld *reference = ActionBgWorld_Create();
  ActRaiserActionBgLayerSnapshot snapshot;
  uint16_t expected = 0, actual = 0;
  CHECK(reference != NULL);
  CHECK(ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 0, ppu->bgXsc[0], &snapshot));
  CHECK(ActionBgWorld_Update(reference, &snapshot.decode));
  CHECK(ActionBgWorld_Lookup(reference, 0, 0, &expected) ==
        kActionBgLookup_Tile);
  CHECK(binding->lookup(binding->context, 0, 0, &actual));
  CHECK(actual == expected);
  CHECK(!binding->lookup(binding->context, 64, 0, &actual));
  ActionBgWorld_Destroy(reference);

  /* Every rejected frame clears the previous binding rather than mixing a
   * stale HLE layer with a new native plan. */
  plan.layer[0].source = kActionBgSource_NativeTilemap;
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) == 0);
  CHECK(ppu->virtualTilemap[0].lookup == NULL);
  plan.layer[0].source = kActionBgSource_WorldMap;
  ppu->bgmode = 7;
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) == 0);
  CHECK(ppu->virtualTilemap[0].lookup == NULL);

  ActRaiserActionBg_Shutdown();
  CHECK(unsetenv("AR_ACTION_BG_HLE") == 0);
  free(ppu);
  free(wram);
}

int main(void) {
  TestCapture();
  TestRingAndComparison();
  TestFramePlanCapture();
  TestFramePlanBinding();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser_action_bg: OK\n");
  return 0;
}
