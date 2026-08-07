#include "sim_world_navigation_capture.h"

#include <string.h>

#include "sim3d.h"
#include "snes/ppu.h"

uint32_t g_sim_world_navigation_palace_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_ui_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];

static bool CaptureLayer(Ppu *ppu,
                         SimWorldNavigationCompositionLayer *layer,
                         uint32_t *pixels) {
  PpuObjRangeBounds bounds;
  if (!layer || !layer->visible || !pixels ||
      !PpuGetObjRangeBounds(ppu, layer->oam_first, layer->oam_count,
                            3, &bounds))
    return false;
  const int width = bounds.x1 - bounds.x0;
  const int height = bounds.y1 - bounds.y0;
  if (bounds.x0 < 0 || bounds.y0 < 0 ||
      bounds.x1 > kSimWorldNavigationCompositionWidth ||
      bounds.y1 > kSimWorldNavigationCompositionHeight ||
      width <= 0 || height <= 0)
    return false;
  if (!PpuRasterizeObjRange(
          ppu, layer->oam_first, layer->oam_count, 3, &bounds,
          pixels, width, height, kSimWorldNavigationCompositionPitch))
    return false;
  layer->screen_x = bounds.x0;
  layer->screen_y = bounds.y0;
  layer->width = (uint16_t)width;
  layer->height = (uint16_t)height;
  return true;
}

/* The scene module is pure and cannot include ppu.h, so it declares its own OAM
 * word count. This file sees both and is where the two are kept honest. */
_Static_assert(kSimWorldNavigationOamWords == kPpuOamWords,
               "sim_world_navigation_scene.h's mirrored OAM word count must "
               "match ppu.h");

bool SimWorldNavigationCapture_Capture(SimFrameData *frame, Ppu *ppu) {
  if (!frame || frame->view != kSimView_WorldNavigation ||
      !frame->world_navigation_scene.valid)
    return false;

  frame->world_navigation_brightness =
      ppu ? (uint8_t)PPU_brightness(ppu) : 0;
  SimWorldNavigationComposition composition;
  /* Partial INIDISP brightness is a supported presentation state. The host
   * world is faded as one complete composition, while these PPU-rasterized
   * Palace/UI pixels already contain the same master-brightness adjustment.
   * Forced blank remains authentic fallback; both paths are fully black. */
  if (!ppu || PPU_mode(ppu) != 7 || PPU_forcedBlank(ppu) ||
      !SimWorldNavigationScene_ClassifyOam(ppu->oam, &composition)) {
    frame->world_navigation_scene.composition =
        (SimWorldNavigationComposition){0};
    frame->view = kSimView_AuthenticFallback;
    return false;
  }

  /* Unlike town separated capture, navigation has no reason to inherit the
   * most recently captured town's backdrop. Snapshot the live Mode-7 PPU
   * colour at full intensity: presentation applies master brightness once to
   * the complete host world after its effects have been composed. */
  frame->separated_backdrop_argb =
      ActRaiser_BackdropArgbFullBrightness(ppu);

  if (!composition.empty_animation &&
      (!CaptureLayer(ppu, &composition.palace,
                     g_sim_world_navigation_palace_pixels) ||
       !CaptureLayer(ppu, &composition.ui,
                     g_sim_world_navigation_ui_pixels))) {
    frame->world_navigation_scene.composition =
        (SimWorldNavigationComposition){0};
    frame->view = kSimView_AuthenticFallback;
    return false;
  }
  frame->world_navigation_scene.composition = composition;
  return true;
}
