#include "diorama/diorama_capture.h"

#include "actraiser/actraiser_rtl.h"
#include "app/settings.h"
#include "diorama/diorama.h"
#include "diorama/diorama_planes.h"
#include "present/present.h"

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
    mask |= SourcePlanes(SR_PPU_OVERLAY_BG2);
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
  frame->diorama_plane_content_mask = content;
  frame->diorama_plane_additive_mask = additive;
}
