#include "sim_world_navigation_capture.h"

#include <string.h>

uint32_t g_sim_world_navigation_palace_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_ui_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];

static bool CaptureLayer(const SnesRunnerApi *api, SrRunnerHandle *runner,
                         uint64_t lifetime_generation,
                         SimWorldNavigationCompositionLayer *layer,
                         uint32_t *pixels) {
  SrPpuObjRasterRequest request = {
      .struct_size = sizeof(request),
      .lifetime_generation = lifetime_generation,
      .first_sprite = layer ? layer->oam_first : 0u,
      .sprite_count = layer ? layer->oam_count : 0u,
      .priority = 3u,
      .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
      .pixels = pixels,
      .pixel_byte_size =
          (uint64_t)kSimWorldNavigationCompositionWidth *
          kSimWorldNavigationCompositionHeight * sizeof(uint32_t),
      .pitch_bytes = kSimWorldNavigationCompositionPitch,
  };
  SrPpuObjRasterResult result = {sizeof(result), 0u};
  if (!layer || !layer->visible || !pixels ||
      api->rasterize_ppu_obj_range(runner, &request, &result) != SR_RESULT_OK)
    return false;
  if (result.x0 < 0 || result.y0 < 0 ||
      result.x1 > kSimWorldNavigationCompositionWidth ||
      result.y1 > kSimWorldNavigationCompositionHeight ||
      result.width == 0u || result.height == 0u ||
      result.width > UINT16_MAX || result.height > UINT16_MAX)
    return false;
  layer->screen_x = (int16_t)result.x0;
  layer->screen_y = (int16_t)result.y0;
  layer->width = (uint16_t)result.width;
  layer->height = (uint16_t)result.height;
  return true;
}

/* The scene module stays pure and declares only the public SNES OAM extent.
 * Keep its classifier contract pinned to the runner ABI here. */
_Static_assert(kSimWorldNavigationOamWords == SR_PPU_OAM_WORD_COUNT,
               "sim_world_navigation_scene.h's mirrored OAM word count must "
               "match the runner ABI");

static uint8_t ExpandColor5FullBrightness(uint16_t value) {
  return (uint8_t)((value << 3) | (value >> 2));
}

static uint32_t BackdropArgbFullBrightness(uint16_t color) {
  return UINT32_C(0xff000000) |
      (uint32_t)ExpandColor5FullBrightness(color & 0x1fu) << 16 |
      (uint32_t)ExpandColor5FullBrightness((color >> 5) & 0x1fu) << 8 |
      ExpandColor5FullBrightness((color >> 10) & 0x1fu);
}

bool SimWorldNavigationCapture_Capture(SimFrameData *frame,
                                       SrRunnerHandle *runner) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrPpuStateSnapshot ppu = {SR_PPU_STATE_SNAPSHOT_V2_SIZE, 0u};
  SrBorrowedU16Span oam = {sizeof(oam), 0u, NULL, 0u, 0u};
  SrBorrowedU16Span cgram = {sizeof(cgram), 0u, NULL, 0u, 0u};
  if (!frame || frame->view != kSimView_WorldNavigation ||
      !frame->world_navigation_scene.valid)
    return false;

  frame->world_navigation_brightness = 0u;
  SimWorldNavigationComposition composition;
  /* Partial INIDISP brightness is a supported presentation state. The host
   * world is faded as one complete composition, while these PPU-rasterized
   * Palace/UI pixels already contain the same master-brightness adjustment.
   * Forced blank remains authentic fallback; both paths are fully black. */
  if (!api || !runner ||
      api->struct_size < SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE ||
      (api->capabilities &
       (SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_OBJ_RASTER)) !=
          (SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_BORROWED_U16_SPANS |
           SR_RUNNER_CAP_PPU_OBJ_RASTER) ||
      api->query_ppu_state(runner, &ppu) != SR_RESULT_OK) {
    frame->world_navigation_scene.composition =
        (SimWorldNavigationComposition){0};
    frame->view = kSimView_AuthenticFallback;
    return false;
  }
  frame->world_navigation_brightness = ppu.brightness;
  if (api->borrow_u16_memory(runner, SR_MEMORY_OAM, &oam) != SR_RESULT_OK ||
      api->borrow_u16_memory(runner, SR_MEMORY_CGRAM, &cgram) != SR_RESULT_OK ||
      oam.element_count < SR_PPU_OAM_WORD_COUNT ||
      cgram.element_count < SR_PPU_CGRAM_WORD_COUNT ||
      oam.lifetime_generation != ppu.lifetime_generation ||
      cgram.lifetime_generation != ppu.lifetime_generation ||
      ppu.bg_mode != 7u || (ppu.flags & SR_PPU_STATE_FORCED_BLANK) != 0u ||
      !SimWorldNavigationScene_ClassifyOam(oam.data, &composition)) {
    frame->world_navigation_scene.composition =
        (SimWorldNavigationComposition){0};
    frame->view = kSimView_AuthenticFallback;
    return false;
  }

  /* Unlike town separated capture, navigation has no reason to inherit the
   * most recently captured town's backdrop. Snapshot the live Mode-7 PPU
   * colour at full intensity: presentation applies master brightness once to
   * the complete host world after its effects have been composed. */
  frame->separated_backdrop_argb = BackdropArgbFullBrightness(cgram.data[0]);

  if (!composition.empty_animation &&
      (!CaptureLayer(api, runner, ppu.lifetime_generation,
                     &composition.palace,
                     g_sim_world_navigation_palace_pixels) ||
       !CaptureLayer(api, runner, ppu.lifetime_generation, &composition.ui,
                     g_sim_world_navigation_ui_pixels))) {
    frame->world_navigation_scene.composition =
        (SimWorldNavigationComposition){0};
    frame->view = kSimView_AuthenticFallback;
    return false;
  }
  frame->world_navigation_scene.composition = composition;
  return true;
}
