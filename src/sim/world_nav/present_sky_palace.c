#include "sim/world_nav/present_sky_palace.h"
#include "sim/world_nav/present_world_nav.h"
#include "present/presentation_surface.h"
#include "sim/world_nav/sim_world_navigation_palace.h"

static ArRenderTexture s_sky_palace_foreground_texture;
static bool s_sky_palace_foreground_valid;
static uint32_t s_sky_palace_foreground_pixels[
    kSimWorldNavigationPalaceMaxWidth * kSimWorldNavigationPalaceMaxHeight];

/* Consume borrowed capture pixels only during the slot's upload lifetime.
 * Retained presentations use the published texture, never the producer's
 * mutable mask. Invalid foreground is a selected-scene failure, not native
 * fallback permission. */
void PresentSkyPalace_Upload(ArRenderDevice *device, const FrameSlot *slot) {
  s_sky_palace_foreground_valid = false;
  if (!ArRenderDevice_IsReady(device)) return;
  if (slot->sim.view != kSimView_SkyPalace || slot->diorama_active ||
      slot->snes_width <= 0 || slot->snes_height <= 0 ||
      slot->snes_width > kSimWorldNavigationPalaceMaxWidth ||
      slot->snes_height > kSimWorldNavigationPalaceMaxHeight) return;
  const SrPpuSurfaceView *native = PresentationSurface_Bound(&slot->ppu_surfaces.main);
  const SrPpuSurfaceView *mask = PresentationSurface_Bound(
      &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0]);
  const uint8_t *pixels = PresentationSurface_Region(native,
      native ? native->origin_x - slot->ws_extra : -1,
      native ? native->origin_y - slot->ws_extra_top : -1,
      slot->snes_width, slot->snes_height);
  const uint8_t *winners = PresentationSurface_Region(mask,
      mask ? mask->origin_x - slot->ws_extra : -1,
      mask ? mask->origin_y - slot->ws_extra_top : -1,
      slot->snes_width, slot->snes_height);
  const int pitch = kSimWorldNavigationPalaceMaxWidth * (int)sizeof(uint32_t);
  if (!pixels || !winners || !SimWorldNavigationPalace_ComposeForeground(
          s_sky_palace_foreground_pixels, pitch,
          pixels, (int)native->pitch_bytes, winners, (int)mask->pitch_bytes,
          slot->snes_width, slot->snes_height)) return;
  if (!ArRenderTexture_IsValid(s_sky_palace_foreground_texture)) {
    const ArRenderTextureDesc desc = {
      .width = kSimWorldNavigationPalaceMaxWidth,
      .height = kSimWorldNavigationPalaceMaxHeight,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(
            device, &desc, &s_sky_palace_foreground_texture)) return;
  }
  const ArRenderRectI rect = {0, 0, slot->snes_width, slot->snes_height};
  s_sky_palace_foreground_valid = ArRenderDevice_UpdateTexture(
      device, s_sky_palace_foreground_texture, &rect,
      s_sky_palace_foreground_pixels, pitch);
}

bool PresentSkyPalace_ForegroundReady(void) { return s_sky_palace_foreground_valid; }

void PresentSkyPalace_Reset(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_sky_palace_foreground_texture);
  s_sky_palace_foreground_texture = ArRenderTexture_Invalid();
  s_sky_palace_foreground_valid = false;
}

PresentationOutcome PresentSkyPalace_Draw(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    const ArRenderRectF *source,
    const ArRenderRectF *destination) {
  if (!slot || slot->sim.view != kSimView_SkyPalace ||
      !s_sky_palace_foreground_valid)
    return kPresentationOutcome_CoreFailure;
  const PresentationOutcome outcome = PresentWorldNavigationBackdrop(slot, viewport);
  if (!PresentationOutcome_IsUsable(outcome)) return outcome;
  if (!ArRenderDevice_DrawTexture(device, s_sky_palace_foreground_texture, source, destination))
    return kPresentationOutcome_CoreFailure;
  return outcome;
}
