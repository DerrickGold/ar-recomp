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
      binding->vscroll_anchor > 0x3ff ||
      (binding->flags & ~kPpuVirtualTilemapFlag_IncludeAuthentic))
    return false;
  ppu->virtualTilemap[layer] = *binding;
  return true;
}

void PpuSetWidescreenLayerExtent(Ppu *ppu, uint8_t layer,
                                 uint16_t left, uint16_t right,
                                 uint16_t top, uint16_t bottom) {
  if (!ppu || layer >= 4) return;
  ppu->wsLayerExtentLeftDefault[layer] = left;
  ppu->wsLayerExtentRightDefault[layer] = right;
  ppu->wsLayerExtentTop[layer] = top;
  ppu->wsLayerExtentBottom[layer] = bottom;
  for (int y = 0; y < kPpuYPixels; y++) {
    ppu->wsLayerExtentLeft[layer][y] = left;
    ppu->wsLayerExtentRight[layer][y] = right;
  }
}

void PpuSetWidescreenLayerExtentBand(Ppu *ppu, uint8_t layer,
                                     uint8_t y0, uint8_t y1,
                                     uint16_t left, uint16_t right) {
  if (!ppu || layer >= 4 || y0 >= y1 || y1 > kPpuYPixels) return;
  for (int y = y0; y < y1; y++) {
    ppu->wsLayerExtentLeft[layer][y] = left;
    ppu->wsLayerExtentRight[layer][y] = right;
  }
}

static void ResetExtentStub(Ppu *ppu) {
  memset(ppu->wsLayerExtentLeftDefault, 0xff,
         sizeof(ppu->wsLayerExtentLeftDefault));
  memset(ppu->wsLayerExtentRightDefault, 0xff,
         sizeof(ppu->wsLayerExtentRightDefault));
  memset(ppu->wsLayerExtentTop, 0xff, sizeof(ppu->wsLayerExtentTop));
  memset(ppu->wsLayerExtentBottom, 0xff,
         sizeof(ppu->wsLayerExtentBottom));
  memset(ppu->wsLayerExtentLeft, 0xff, sizeof(ppu->wsLayerExtentLeft));
  memset(ppu->wsLayerExtentRight, 0xff, sizeof(ppu->wsLayerExtentRight));
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

static void TestVerticalMargins(void) {
  int top = -1, bottom = -1;
  ActRaiserActionBg_ResolveVerticalMargins(232, 512, 64, &top, &bottom);
  CHECK(top == 64 && bottom == 55);

  /* The reported 2026-08-10 jump moves the camera from 232 to 184. A 64-row
   * symmetric budget keeps world row 436 visible in both captures; the old
   * top-only capture ended at row 408 after the jump. */
  ActRaiserActionBg_ResolveVerticalMargins(184, 512, 64, &top, &bottom);
  CHECK(top == 64 && bottom == 64);
  CHECK(436 >= 184 - top && 436 < 184 + 224 + bottom);
  CHECK(436 >= 232 - 64 && 436 < 232 + 224 + 55);

  ActRaiserActionBg_ResolveVerticalMargins(0, 512, 64, &top, &bottom);
  CHECK(top == 0 && bottom == 64);
  ActRaiserActionBg_ResolveVerticalMargins(287, 512, 64, &top, &bottom);
  CHECK(top == 64 && bottom == 0);
  ActRaiserActionBg_ResolveVerticalMargins(100, 200, 64, &top, &bottom);
  CHECK(top == 64 && bottom == 0);
  ActRaiserActionBg_ResolveVerticalMargins(100, 512, -1, &top, &bottom);
  CHECK(top == 0 && bottom == 0);
  ActRaiserActionBg_ResolveVerticalMargins(8, 512, 4, NULL, &bottom);
  CHECK(bottom == 4);
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
  /* Authentic x=0..255 and PPU scanlines 1..224 expose 33 by 28 cells at
   * camera phases 13/7. */
  CHECK(result.compared == 33u * 28u);
  CHECK(result.mismatches == 0 && result.outside_world == 0);
  CHECK(result.first_tile_x == -1 && result.first_tile_y == -1);
  CHECK(result.first_outside_tile_x == -1 &&
        result.first_outside_tile_y == -1);

  const int changed_x = snapshot.camera_x >> 3;
  const int changed_y = (snapshot.camera_y + 1) >> 3;
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
  CHECK(result.compared == 2u * 28u);
  CHECK(result.outside_world == 31u * 28u);
  CHECK(result.mismatches == 0);
  CHECK(result.first_outside_tile_x == 64 &&
        result.first_outside_tile_y == 1);

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
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].horizontal_extent.left == 76 &&
        plan.layer[1].horizontal_extent.right == 100);
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

static void TestPlanExtentProjection(void) {
  Ppu *ppu = calloc(1, sizeof(*ppu));
  CHECK(ppu != NULL);
  if (!ppu) return;
  ResetExtentStub(ppu);

  ActionBgPlan plan;
  ActionBgPlan_InitNative(&plan);
  ActionBgLayerPlan *bg2 = &plan.layer[kActRaiserPpuLayer_Bg2];
  bg2->default_edge = kActionBgEdge_Mirror;
  bg2->horizontal_extent = (ActionBgHorizontalExtent) {
    .mode = kActionBgExtent_Fixed,
    .left = 48,
    .right = 64,
  };
  bg2->vertical_extent = (ActionBgVerticalExtent) {
    .mode = kActionBgExtent_Fixed,
    .top = 12,
    .bottom = 7,
  };
  bg2->bands[0] = (ActionBgBand) {
    .y0 = 136,
    .y1 = 224,
    .edge = kActionBgEdge_Repeat,
    .horizontal_extent = {
      .mode = kActionBgExtent_Available,
    },
  };
  bg2->band_count = 1;
  CHECK(ActionBgPlan_Validate(&plan));

  CHECK(ActRaiserActionBg_ApplyPlanExtents(&plan, ppu));
  CHECK(ppu->wsLayerExtentLeftDefault[kActRaiserPpuLayer_Bg1] ==
        kPpuWidescreenExtentAvailable);
  CHECK(ppu->wsLayerExtentLeftDefault[kActRaiserPpuLayer_Bg2] == 48);
  CHECK(ppu->wsLayerExtentRightDefault[kActRaiserPpuLayer_Bg2] == 64);
  CHECK(ppu->wsLayerExtentTop[kActRaiserPpuLayer_Bg2] == 12);
  CHECK(ppu->wsLayerExtentBottom[kActRaiserPpuLayer_Bg2] == 7);
  CHECK(ppu->wsLayerExtentLeft[kActRaiserPpuLayer_Bg2][100] == 48);
  CHECK(ppu->wsLayerExtentRight[kActRaiserPpuLayer_Bg2][100] == 64);
  CHECK(ppu->wsLayerExtentLeft[kActRaiserPpuLayer_Bg2][136] ==
        kPpuWidescreenExtentAvailable);
  CHECK(ppu->wsLayerExtentRight[kActRaiserPpuLayer_Bg2][223] ==
        kPpuWidescreenExtentAvailable);

  /* The per-frame reset restores available everywhere, and rejection is
   * atomic: an invalid plan cannot partially restage caps. */
  ResetExtentStub(ppu);
  CHECK(ppu->wsLayerExtentLeft[kActRaiserPpuLayer_Bg2][100] ==
        kPpuWidescreenExtentAvailable);
  ActionBgPlan invalid = plan;
  invalid.layer[1].bands[0].y1 = 225;
  CHECK(!ActRaiserActionBg_ApplyPlanExtents(&invalid, ppu));
  CHECK(ppu->wsLayerExtentLeft[kActRaiserPpuLayer_Bg2][100] ==
        kPpuWidescreenExtentAvailable);
  CHECK(!ActRaiserActionBg_ApplyPlanExtents(&plan, NULL));

  free(ppu);
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
  /* BH7 production default: an absent variable must exercise the provider. */
  CHECK(unsetenv("AR_ACTION_BG_HLE") == 0);
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
  ppu->hScroll[0] = 0x40D;
  ppu->vScroll[0] = 0x807;

  ActionBgPlan plan;
  ActionBgPresentationPolicy policy;
  CHECK(ActRaiserActionBg_BuildPlan(
      wram, kActRaiserWramSize, ppu, true, &plan, &policy));
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);

  ActionBgWorld *reference = ActionBgWorld_Create();
  ActRaiserActionBgLayerSnapshot snapshot;
  CHECK(reference != NULL);
  CHECK(ActRaiserActionBg_CaptureLayer(
      wram, kActRaiserWramSize, 0, ppu->bgXsc[0], &snapshot));
  CHECK(ActionBgWorld_Update(reference, &snapshot.decode));
  PopulateNativeRing(reference, &snapshot, ppu->vram);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) ==
      kActRaiserBgLayerMask_Bg1);
  const PpuVirtualTilemapBinding *binding = &ppu->virtualTilemap[0];
  CHECK(binding->lookup != NULL && binding->context != NULL);
  CHECK(binding->camera_x == 13 && binding->camera_y == 7);
  CHECK(binding->hscroll_anchor == 13);
  CHECK(binding->vscroll_anchor == 7);
  CHECK(binding->flags == kPpuVirtualTilemapFlag_IncludeAuthentic);
  CHECK(ppu->virtualTilemap[1].lookup == NULL);

  uint16_t expected = 0, actual = 0;
  CHECK(ActionBgWorld_Lookup(reference, 0, 0, &expected) ==
        kActionBgLookup_Tile);
  CHECK(binding->lookup(binding->context, 0, 0, &actual));
  CHECK(actual == expected);
  CHECK(!binding->lookup(binding->context, 64, 0, &actual));

  const ActRaiserActionBgDiagnostics *diagnostics =
      ActRaiserActionBg_GetDiagnostics();
  CHECK(diagnostics->layer_activations == 1);

  /* A paused redraw owns the same logical game frame. Rebinding must remain
   * valid without rebuilding the immutable world; a geometry/resize callback
   * may clear the frame-scoped PPU seam first, so pin that sequence too. */
  PpuClearVirtualTilemaps(ppu);
  CHECK(ppu->virtualTilemap[0].lookup == NULL);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) ==
      kActRaiserBgLayerMask_Bg1);
  CHECK(ppu->virtualTilemap[0].lookup != NULL);
  CHECK(diagnostics->layer_activations == 1);

  /* Savestate/restart invalidation discards provider-owned caches. The next
   * render may still carry the same game-frame value and must rebuild cleanly
   * rather than mistaking it for a stale paused redraw. */
  PpuClearVirtualTilemaps(ppu);
  ActRaiserActionBg_Reset();
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) ==
      kActRaiserBgLayerMask_Bg1);
  CHECK(ppu->virtualTilemap[0].lookup != NULL);
  CHECK(diagnostics->layer_activations == 2);

  CHECK(diagnostics->provider_preflight_layers == 3);
  CHECK(diagnostics->provider_preflight_tiles == 3u * 33u * 28u);
  CHECK(diagnostics->provider_preflight_mismatches == 0);
  CHECK(diagnostics->provider_preflight_outside_world == 0);
  CHECK(diagnostics->provider_eligible_layers == 3);

  /* Any authentic contradiction rejects the whole layer for that frame. */
  const int changed_x = snapshot.camera_x >> 3;
  const int changed_y = (snapshot.camera_y + 1) >> 3;
  size_t changed_address = 0;
  CHECK(ActRaiserActionBg_RingAddress(snapshot.tilemap_base,
                                      changed_x, changed_y,
                                      kVramWords, &changed_address));
  ppu->vram[changed_address] ^= 1;
  Write16(wram, kActRaiserWram_GameFrame, 101);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) == 0);
  CHECK(ppu->virtualTilemap[0].lookup == NULL);
  CHECK(diagnostics->provider_preflight_mismatches == 1);
  CHECK(diagnostics->fallbacks[kActRaiserActionBgFallback_CompareFailure] == 1);
  ppu->vram[changed_address] ^= 1;

  /* The finite decoder may report a valid outside-world coordinate, but the
   * full authentic handoff requires every displayed cell to exist. */
  Write16(wram, kActRaiserWram_Bg1CameraX, 500);
  ppu->hScroll[0] = 500;
  Write16(wram, kActRaiserWram_GameFrame, 102);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) == 0);
  CHECK(diagnostics->provider_preflight_outside_world == 31u * 28u);
  CHECK(diagnostics->fallbacks[kActRaiserActionBgFallback_AuthenticEdge] == 1);

  /* Camera/PPU phase disagreement means the comparison would address a
   * different native cell than scanout, so it fails before preflight. */
  Write16(wram, kActRaiserWram_Bg1CameraX, 13);
  ppu->hScroll[0] = 14;
  Write16(wram, kActRaiserWram_GameFrame, 103);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) == 0);
  CHECK(diagnostics->fallbacks[kActRaiserActionBgFallback_ScrollPhase] == 1);
  ppu->hScroll[0] = 13;
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

  /* The native A/B remains exact and frame-scoped: after resetting the cached
   * environment decision, explicit 0 must clear/decline every binding. */
  CHECK(setenv("AR_ACTION_BG_HLE", "0", 1) == 0);
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = kActRaiserBgLayerMask_Bg1 |
                          kActRaiserBgLayerMask_Bg2;
  plan.layer[0].source = kActionBgSource_WorldMap;
  Write16(wram, kActRaiserWram_Bg1CameraX, 13);
  ppu->hScroll[0] = 13;
  Write16(wram, kActRaiserWram_GameFrame, 104);
  CHECK(ActRaiserActionBg_BindPlan(
      wram, kActRaiserWramSize, &plan, ppu) == 0);
  CHECK(ppu->virtualTilemap[0].lookup == NULL);
  CHECK(ActRaiserActionBg_GetDiagnostics()->provider_frames == 0);
  ActRaiserActionBg_Shutdown();
  CHECK(unsetenv("AR_ACTION_BG_HLE") == 0);
  free(ppu);
  free(wram);
}

int main(void) {
  TestCapture();
  TestVerticalMargins();
  TestRingAndComparison();
  TestFramePlanCapture();
  TestPlanExtentProjection();
  TestFramePlanBinding();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser_action_bg: OK\n");
  return 0;
}
