#include "scene_inspector.h"

#include <stdio.h>
#include <string.h>

#include "runner_internal.h"
#include "snes/ppu.h"

uint8 g_ram[0x20000];
Snes *g_snes;
static Ppu s_ppu;
static int s_failures;

static SrResult QueryPpuState(SrRunnerHandle *runner,
                              SrPpuStateSnapshot *state) {
  (void)runner;
  if (!state || state->struct_size < SR_PPU_STATE_SNAPSHOT_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  memset(state, 0, sizeof(*state));
  state->struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE;
  state->lifetime_generation = 1u;
  state->display_control = s_ppu.inidisp;
  state->bg_mode_control = s_ppu.bgmode;
  state->mosaic_control = s_ppu.mosaic;
  state->bg_mode = PPU_mode(&s_ppu);
  state->brightness = PPU_brightness(&s_ppu);
  state->main_screen = s_ppu.screenEnabled[0];
  state->sub_screen = s_ppu.screenEnabled[1];
  state->main_windowed = s_ppu.screenWindowed[0];
  state->sub_windowed = s_ppu.screenWindowed[1];
  state->object_size_select = PPU_objSize(&s_ppu);
  state->margin_left = s_ppu.extraLeftCur;
  state->margin_right = s_ppu.extraRightCur;
  state->object_tile_base_1_word = PPU_objTileAdr1(&s_ppu);
  state->object_tile_base_2_word = PPU_objTileAdr2(&s_ppu);
  state->mode7_select = s_ppu.m7sel;
  memcpy(state->mode7_matrix, s_ppu.m7matrix,
         sizeof(state->mode7_matrix));
  for (unsigned layer = 0u; layer < 4u; ++layer) {
    SrPpuBackgroundState *background = &state->backgrounds[layer];
    background->h_scroll = s_ppu.hScroll[layer];
    background->v_scroll = s_ppu.vScroll[layer];
    background->tilemap_base_word = PPU_bgTilemapAdr(&s_ppu, layer);
    background->tile_base_word = PPU_bgTileAdr(&s_ppu, layer);
    background->tilemap_width_tiles =
        PPU_bgTilemapWider(&s_ppu, layer) ? 64u : 32u;
    background->tilemap_height_tiles =
        PPU_bgTilemapHigher(&s_ppu, layer) ? 64u : 32u;
  }
  return SR_RESULT_OK;
}

static SrResult QueryPpuFrame(SrRunnerHandle *runner,
                              SrPpuFrameSnapshot *frame) {
  (void)runner;
  if (!frame || frame->struct_size < SR_PPU_FRAME_SNAPSHOT_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  memset(frame, 0, sizeof(*frame));
  frame->struct_size = SR_PPU_FRAME_SNAPSHOT_V2_SIZE;
  frame->lifetime_generation = 1u;
  frame->margin_budget = s_ppu.extraLeftRight;
  frame->hud_split_height = s_ppu.wsHudSplitHeight;
  frame->hud_left_end = s_ppu.wsHudLeftEnd;
  frame->hud_right_start = s_ppu.wsHudRightStart;
  frame->hud_left_only_y = s_ppu.wsHudLeftOnlyY;
  return SR_RESULT_OK;
}

static SrResult BorrowU16(SrRunnerHandle *runner, SrMemoryRegion region,
                          SrBorrowedU16Span *span) {
  (void)runner;
  if (!span || span->struct_size < SR_BORROWED_U16_SPAN_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  span->region = region;
  span->lifetime_generation = 1u;
  if (region == SR_MEMORY_VRAM) {
    span->data = s_ppu.vram;
    span->element_count = SR_PPU_VRAM_WORD_COUNT;
  } else if (region == SR_MEMORY_OAM) {
    span->data = s_ppu.oam;
    span->element_count = SR_PPU_OAM_WORD_COUNT;
  } else {
    return SR_RESULT_UNSUPPORTED;
  }
  return SR_RESULT_OK;
}

static SrResult BorrowBytes(SrRunnerHandle *runner, SrMemoryRegion region,
                            SrBorrowedSpan *span) {
  (void)runner;
  if (!span || span->struct_size < SR_BORROWED_SPAN_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  if (region != SR_MEMORY_HIGH_OAM) return SR_RESULT_UNSUPPORTED;
  span->region = region;
  span->data = s_ppu.highOam;
  span->byte_size = sizeof(s_ppu.highOam);
  span->lifetime_generation = 1u;
  return SR_RESULT_OK;
}

static uint32_t BorrowU16Valid(SrRunnerHandle *runner,
                               const SrBorrowedU16Span *span) {
  (void)runner;
  return span && span->lifetime_generation == 1u;
}

static uint32_t BorrowValid(SrRunnerHandle *runner,
                            const SrBorrowedSpan *span) {
  (void)runner;
  return span && span->lifetime_generation == 1u;
}

static SrResult ResolveBackgroundCoordinate(
    SrRunnerHandle *runner,
    const SrPpuBackgroundCoordinateRequest *request,
    SrPpuBackgroundCoordinateResult *result) {
  (void)runner;
  if (!request || !result || request->layer >= 4u ||
      request->lifetime_generation != 1u)
    return SR_RESULT_INVALID_ARGUMENT;
  PpuWidescreenLayerPolicy policy = {
    kPpuWidescreenBandFill_RawWrap,
    kPpuWidescreenMotion_FillRelative,
    false,
  };
  int source_x = 0;
  bool mapped = request->screen_x >= -s_ppu.extraLeftCur &&
      request->screen_x < kPpuXPixels + s_ppu.extraRightCur &&
      PpuMapWidescreenLayerX(
          &s_ppu, (uint8_t)request->layer, request->screen_y,
          request->screen_x, &source_x, &policy);
  memset(result, 0, sizeof(*result));
  result->struct_size = SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE;
  result->lifetime_generation = 1u;
  result->flags = (mapped ? SR_PPU_BACKGROUND_COORDINATE_MAPPED : 0u) |
      (policy.band_override
          ? SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE : 0u);
  result->source_x = source_x;
  result->sample_y = request->screen_y + 1;
  result->fill = (SrPpuBackgroundFill)policy.fill;
  result->motion = (SrPpuBackgroundMotion)policy.motion;
  return SR_RESULT_OK;
}

static const SnesRunnerApi s_api = {
  .abi_version = SR_RUNNER_ABI_VERSION,
  .struct_size = sizeof(SnesRunnerApi),
  .capabilities = SR_RUNNER_CAP_PPU_STATE |
      SR_RUNNER_CAP_PPU_FRAME_STATE |
      SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
      SR_RUNNER_CAP_BORROWED_U16_SPANS |
      SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE,
  .borrow_memory = BorrowBytes,
  .borrow_is_valid = BorrowValid,
  .query_ppu_state = QueryPpuState,
  .borrow_u16_memory = BorrowU16,
  .borrow_u16_is_valid = BorrowU16Valid,
  .query_ppu_frame_state = QueryPpuFrame,
  .resolve_ppu_background_coordinate = ResolveBackgroundCoordinate,
};

const SnesRunnerApi *sr_runner_get_api(uint32_t version) {
  return version == SR_RUNNER_ABI_VERSION ? &s_api : NULL;
}

SrRunnerHandle *RtlGameRunner(void) {
  return (SrRunnerHandle *)(void *)g_snes;
}

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void ResetPpu(void) {
  memset(&s_ppu, 0, sizeof(s_ppu));
  memset(g_ram, 0, sizeof(g_ram));
  g_snes = (Snes *)(void *)&s_ppu;
  s_ppu.bgmode = 1;
  s_ppu.inidisp = 15;
  s_ppu.extraLeftRight = 43;
  s_ppu.extraLeftCur = 43;
  s_ppu.extraRightCur = 43;
  g_ram[0x18] = 1;
  g_ram[0x19] = 2;
  SceneInspector_SetSimFrameData(NULL);
  SceneInspector_Clear();
}

static void TestBgAndMirrorMapping(void) {
  ResetPpu();
  s_ppu.screenEnabled[0] = 1;
  s_ppu.bgXsc[0] = 0;
  s_ppu.bgTileAdr = 1; /* BG1 character base = VRAM word $1000. */
  s_ppu.vram[0] = 5 | (2 << 10) | (1 << 13);
  int char_address = 0x1000 + 5 * 16;
  s_ppu.vram[char_address + 1] = 1 << 3; /* output x=4, scanline 1 */

  CHECK(SceneInspector_Select(4, 0));
  CHECK(strstr(SceneInspector_PanelText(), "BG1 T$005") != NULL);
  CHECK(strstr(SceneInspector_PanelText(), "PIX1 CENTER") != NULL);

  s_ppu.wsLayerMirror = 1;
  CHECK(SceneInspector_Select(-4, 0));
  CHECK(strstr(SceneInspector_PanelText(), "PIX1 MIRROR") != NULL);
}

static void TestObjectIdentity(void) {
  ResetPpu();
  s_ppu.screenEnabled[0] = 0x10;
  int slot = 3;
  int index = slot * 2;
  s_ppu.oam[index] = 20 | (30 << 8);
  s_ppu.oam[index + 1] = 7 | (5 << 9) | (2 << 12);
  int char_address = 7 * 16;
  s_ppu.vram[char_address + 2] = 1 << 5; /* local pixel 2,2 */

  CHECK(SceneInspector_Select(22, 32));
  CHECK(strstr(SceneInspector_PanelText(), "OBJ#3 8X8") != NULL);
  CHECK(strstr(SceneInspector_PanelText(), "BASE$07 SUB$07") != NULL);
  int x0, y0, x1, y1;
  CHECK(SceneInspector_GetHighlight(&x0, &y0, &x1, &y1));
  CHECK(x0 == 20 && y0 == 30 && x1 == 28 && y1 == 38);

  SimFrameData sim = {0};
  sim.town = 1;
  sim.view = kSimView_Enhanced;
  sim.metadata_valid = true;
  sim.build_serial = 42;
  sim.requested_features = kSimFeature_All;
  sim.separated_valid = true;
  sim.separated_status = kSim3DCapture_Ready;
  sim.separated_hash = UINT64_C(0x123456789abcdef0);
  sim.object_count = 1;
  sim.objects[0] = (SimRenderObject){
    .record_address = 0x0A00,
    .composition = 0xE71B,
    .oam_first = slot,
    .oam_count = 1,
    .priority = 2,
    .tier = kSimRecordTier_World,
    .foot_x = 120,
    .foot_y = 104,
  };
  /* The panel must report the classifier's own verdict for this Napper
   * ground-pluck composition, not a second inspector-local guess. */
  SimObjectClassification classification = Sim3D_ClassifyObject(
      sim.objects[0].tier, 0x13, 11, sim.objects[0].record_address,
      sim.objects[0].composition);
  sim.objects[0].height_class = classification.height_class;
  sim.objects[0].virtual_height = classification.virtual_height;
  sim.objects[0].traits = classification.traits;
  SceneInspector_SetSimFrameData(&sim);
  CHECK(SceneInspector_Select(22, 32));
  CHECK(strstr(SceneInspector_PanelText(),
               "SIM3D enhanced META OK SERIAL 42") != NULL);
  CHECK(strstr(SceneInspector_PanelText(),
               "FLAT READY STATUS ready MISMATCH 0 "
               "HASH 123456789ABCDEF0") != NULL);
  CHECK(strstr(SceneInspector_PanelText(),
               "SIMW REC$0A00 CMP$E71B OAM3+1 P2 FOOT120,104 "
               "semi_grounded H8 ATLAS PENDING") != NULL);
}

static void TestPlaneFiltering(void) {
  ResetPpu();
  s_ppu.screenEnabled[0] = 1;
  s_ppu.bgTileAdr = 1;
  s_ppu.vram[0] = 5 | (1 << 13);
  int char_address = 0x1000 + 5 * 16;
  s_ppu.vram[char_address + 1] = 1 << 3;

  CHECK(SceneInspector_SelectFiltered(
      4, 0, kSceneInspectorBg3, false));
  CHECK(strstr(SceneInspector_PanelText(), "BG1") == NULL);
  CHECK(strstr(SceneInspector_PanelText(), "NO VISIBLE") != NULL);
}

int main(void) {
  TestBgAndMirrorMapping();
  TestObjectIdentity();
  TestPlaneFiltering();
  SceneInspector_Clear();
  CHECK(!SceneInspector_HasSelection());
  if (s_failures) {
    fprintf(stderr, "scene inspector tests: %d failure(s)\n", s_failures);
    return 1;
  }
  printf("scene inspector tests passed\n");
  return 0;
}
