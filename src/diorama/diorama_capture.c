#include "diorama/diorama_capture.h"

#include "actraiser/actraiser_rtl.h"
#include "app/settings.h"
#include "diorama/diorama.h"
#include "diorama/diorama_planes.h"
#include "present/present.h"
#include "snes_bgr555.h"

static const uint8_t kPrimarySources[] = {
  SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG2, SR_PPU_OVERLAY_BG3, SR_PPU_OVERLAY_OBJ,
};

static uint32_t PlaneBit(unsigned plane) { return 1u << plane; }

static uint32_t SourcePlanes(unsigned source) {
  uint32_t mask = PlaneBit(source);
  size_t band_count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&band_count);
  for (size_t i = 0; i < band_count; i++)
    if (bands[i].source == source)
      mask |= PlaneBit(bands[i].plane);
  return mask;
}

static uint32_t RequestedPlanes(void) {
  _Static_assert(kDioramaPlane_Count <= 32, "diorama masks need one bit per plane");
  uint32_t mask = 0;
  if (g_settings.diorama_layer_backdrop && g_settings.diorama_skybox != kDioramaSky_Only)
    mask |= PlaneBit(kDioramaPlane_Backdrop);
  if (g_settings.diorama_layer_bg1)
    mask |= SourcePlanes(SR_PPU_OVERLAY_BG1);
  if (g_settings.diorama_layer_bg2)
    mask |= SourcePlanes(SR_PPU_OVERLAY_BG2);
  else if (g_settings.diorama_skybox != kDioramaSky_Off)
    mask |= PlaneBit(SR_PPU_OVERLAY_BG2);
  if (g_settings.diorama_layer_obj)
    mask |= SourcePlanes(SR_PPU_OVERLAY_OBJ);
  if (g_settings.diorama_layer_bg3 && !g_settings.diorama_hud_flat)
    mask |= PlaneBit(SR_PPU_OVERLAY_BG3);
  return mask;
}

void DioramaCapture_CaptureFrame(FrameSlot *frame, const SrPpuFrameSnapshot *ppu) {
  frame->diorama_active = g_diorama_frame_active;
  frame->diorama_plane_request_mask = 0;
  frame->diorama_plane_content_mask = 0;
  frame->diorama_plane_additive_mask = 0;
  if (!frame->diorama_active) return;
  frame->diorama_plane_request_mask = RequestedPlanes();
  if (!ppu) return;

  /* Backdrop uses the residual composited frame. BG4 has no diorama plane;
   * every other primary and split must have positively captured content. */
  uint32_t content = PlaneBit(kDioramaPlane_Backdrop), additive = 0;
  for (size_t i = 0; i < sizeof(kPrimarySources) / sizeof(kPrimarySources[0]); i++) {
    const unsigned source = kPrimarySources[i];
    if (ppu->overlays[source].content_band_mask & 1u)
      content |= PlaneBit(source);
    if (ppu->overlays[source].flags & SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN)
      additive |= SourcePlanes(source);
  }
  size_t band_count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&band_count);
  for (size_t i = 0; i < band_count; i++) {
    const DioramaPriorityBand *band = &bands[i];
    if (ppu->overlays[band->source].content_band_mask & (1u << band->band))
      content |= PlaneBit(band->plane);
  }
  /* The hub pass writes this virtual band after scanout, so its live capture
   * result supplements the PPU's own content bits. */
  if (ActRaiser_DioramaDeathHeimHubFacesPromoted())
    content |= PlaneBit(kDioramaPlane_Bg2Far);
  content |= ActRaiser_DioramaPixelContentMask();
  frame->diorama_plane_content_mask = content;
  frame->diorama_plane_additive_mask = additive;
}

static bool PixelWindowInside(const SrPpuStateSnapshot *ppu, unsigned bg,
                              int screen_x) {
  const unsigned flags = (ppu->window_select >> (bg * 4)) & 15u;
  const int x = screen_x < 0 ? 0 : screen_x > 255 ? 255 : screen_x;
  bool first = x >= ppu->window1_left && x <= ppu->window1_right;
  bool second = x >= ppu->window2_left && x <= ppu->window2_right;
  if (flags & 1u) first = !first;
  if (flags & 4u) second = !second;
  if (!(flags & 2u)) return (flags & 8u) && second;
  if (!(flags & 8u)) return first;
  switch ((ppu->window_logic >> (bg * 2)) & 3u) {
    case 0: return first || second;
    case 1: return first && second;
    case 2: return first != second;
    default: return first == second;
  }
}

bool DioramaCapture_PixelLayerVisible(const SrPpuStateSnapshot *ppu,
                                       unsigned bg, int screen_x) {
  if (!ppu || bg >= 2 || (ppu->flags & SR_PPU_STATE_FORCED_BLANK))
    return false;
  const unsigned bit = 1u << bg;
  if (!((ppu->main_screen | ppu->sub_screen) & bit)) return false;
  const unsigned windowed = (ppu->main_screen & bit)
      ? ppu->main_windowed : ppu->sub_windowed;
  return !(windowed & bit) || !PixelWindowInside(ppu, bg, screen_x);
}

uint32_t DioramaCapture_PaintBlackRow(unsigned bg, const uint8_t *bands,
                                     uint32_t *rows[3], size_t width) {
  return DioramaCapture_PaintEditedRow(bg, bands, NULL, 0, rows, width);
}

uint32_t DioramaCapture_StampColor(const SrPpuStateSnapshot *ppu, unsigned bg,
                                  const uint16_t *vram, const uint16_t *cgram,
                                  uint16_t entry, unsigned x, unsigned y,
                                  uint32_t capture_flags) {
  if (!ppu || bg >= 2 || !vram || !cgram ||
      ppu->backgrounds[bg].bits_per_pixel != 4) return 0;
  x &= 7;
  y &= 7;
  if (entry & 0x4000u) x = 7 - x;
  if (entry & 0x8000u) y = 7 - y;
  const unsigned address = ppu->backgrounds[bg].tile_base_word +
      (entry & 1023u) * 16 + y;
  const uint16_t low = vram[address & 0x7fffu];
  const uint16_t high = vram[(address + 8) & 0x7fffu];
  const unsigned bit = 7 - x;
  const unsigned value = ((low >> bit) & 1u) | ((low >> (bit + 7)) & 2u) |
      ((high >> bit) & 1u) * 4 | ((high >> (bit + 7)) & 2u) * 4;
  if (!value) return 0;
  const uint16_t bgr = cgram[((entry >> 10) & 7u) * 16 + value];
  uint32_t color = 0;
  for (unsigned component = 0; component < 3; component++) {
    int channel = (bgr >> (component * 5)) & 31u;
    if (capture_flags & SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT) {
      channel -= (ppu->fixed_color >> (component * 5)) & 31u;
      if (channel < 0) channel = 0;
    }
    /* Captures use ARGB words; SNES BGR555's red occupies the low five bits. */
    color |= (uint32_t)ExpandColor5((uint32_t)channel, ppu->brightness)
        << (16 - component * 8);
  }
  return color | ((capture_flags & SR_PPU_OVERLAY_MARK_BG_HALF_ADD)
      ? 0x80000000u : 0xff000000u);
}

uint32_t DioramaCapture_PaintEditedRow(unsigned bg, const uint8_t *bands,
                                      const uint32_t *colors, uint32_t backing,
                                      uint32_t *rows[3], size_t width) {
  if (bg >= 2 || !bands || !rows) return 0;
  const unsigned planes[3] = {
    bg ? kDioramaPlane_Bg2Far : kDioramaPlane_Bg1Far,
    bg, bg ? kDioramaPlane_Bg2Hi : kDioramaPlane_Bg1Hi,
  };
  uint32_t content = 0;
  for (size_t x = 0; x < width; x++) {
    const unsigned code = bands[x];
    if (code >= 4 && code <= 6 && colors) {
      for (unsigned band = 0; band < 3; band++)
        if (rows[band]) rows[band][x] = band == 1 ? backing : 0;
      if (backing && rows[1]) content |= 1u << planes[1];
      if (rows[code - 4] && colors[x]) {
        rows[code - 4][x] = colors[x];
        content |= 1u << planes[code - 4];
      }
    } else if (code >= 1 && code <= 3 && rows[code - 1]) {
      rows[code - 1][x] = 0xff000000u;
      content |= 1u << planes[code - 1];
    }
  }
  return content;
}
