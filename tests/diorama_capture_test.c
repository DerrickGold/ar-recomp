#include "diorama/diorama_capture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "actraiser/actraiser_rtl.h"
#include "app/settings.h"
#include "present/present.h"

Settings g_settings;
bool g_diorama_frame_active;
static FrameSlot slot;
static SrPpuFrameSnapshot ppu;
static bool faces_promoted;
static int promotion_queries;
bool ActRaiser_DioramaDeathHeimHubFacesPromoted(void) {
  promotion_queries++;
  return faces_promoted;
}
static uint32_t Bit(int plane) { return 1u << plane; }
static void Capture(const SrPpuFrameSnapshot *snapshot) {
  slot.timestamp_ns = 123;
  DioramaCapture_CaptureFrame(&slot, snapshot);
  assert(slot.timestamp_ns == 123);
}

int main(void) {
  /* Independent ABI-to-plane contract. -1 means no diorama surface. */
  const int expected[5][4] = {
    {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far, -1},
    {SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far, -1},
    {SR_PPU_OVERLAY_BG3, -1, -1, -1},
    {-1, -1, -1, -1},
    {SR_PPU_OVERLAY_OBJ, kDioramaPlane_Obj1, kDioramaPlane_Obj2, kDioramaPlane_Obj3},
  };
  size_t count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&count);
  assert(count == 7);
  uint32_t seen = 0;
  for (size_t i = 0; i < count; i++) {
    assert(bands[i].source < 5 && bands[i].band > 0 && bands[i].band < 4);
    assert(expected[bands[i].source][bands[i].band] == bands[i].plane);
    assert(!(seen & Bit(bands[i].plane)));
    seen |= Bit(bands[i].plane);
  }
  slot.diorama_plane_request_mask = slot.diorama_plane_content_mask = UINT32_MAX;
  slot.diorama_plane_additive_mask = UINT32_MAX;
  Capture(&ppu);
  assert(!slot.diorama_active && !slot.diorama_plane_request_mask);
  assert(!slot.diorama_plane_content_mask && !slot.diorama_plane_additive_mask);
  assert(!promotion_queries);
  g_diorama_frame_active = true;
  g_settings.diorama_layer_backdrop = true;
  Capture(NULL);
  assert(slot.diorama_plane_request_mask == Bit(kDioramaPlane_Backdrop));
  assert(!slot.diorama_plane_content_mask && !slot.diorama_plane_additive_mask);
  assert(!promotion_queries);

  for (unsigned source = 0; source < 5; source++) {
    uint32_t family = 0;
    for (unsigned band = 0; band < 8; band++) {
      memset(&ppu, 0, sizeof(ppu));
      ppu.overlays[source].content_band_mask = 1u << band;
      Capture(&ppu);
      const uint32_t plane = band < 4 && expected[source][band] >= 0
          ? Bit(expected[source][band]) : 0;
      assert(slot.diorama_plane_content_mask == (Bit(kDioramaPlane_Backdrop) | plane));
      family |= plane;
    }
    ppu.overlays[source].flags = SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN;
    Capture(&ppu);
    assert(slot.diorama_plane_additive_mask == family);
  }
  memset(&ppu, 0, sizeof(ppu));
  faces_promoted = true;
  Capture(&ppu);
  assert(slot.diorama_plane_content_mask ==
         (Bit(kDioramaPlane_Backdrop) | Bit(kDioramaPlane_Bg2Far)));
  assert(!slot.diorama_plane_additive_mask);
  faces_promoted = false;

  g_settings.diorama_skybox = kDioramaSky_Only;
  Capture(NULL);
  assert(slot.diorama_plane_request_mask == Bit(SR_PPU_OVERLAY_BG2));
  g_settings.diorama_skybox = kDioramaSky_Both;
  Capture(NULL);
  assert(slot.diorama_plane_request_mask ==
         (Bit(kDioramaPlane_Backdrop) | Bit(SR_PPU_OVERLAY_BG2)));
  g_settings.diorama_layer_bg1 = g_settings.diorama_layer_bg2 = true;
  g_settings.diorama_layer_bg3 = g_settings.diorama_layer_obj = true;
  Capture(NULL);
  const uint32_t all = ((1u << kDioramaPlane_Count) - 1u) & ~Bit(SR_PPU_OVERLAY_BG4);
  assert(slot.diorama_plane_request_mask == all);
  g_settings.diorama_hud_flat = true;
  Capture(NULL);
  assert(slot.diorama_plane_request_mask == (all & ~Bit(SR_PPU_OVERLAY_BG3)));
  puts("Diorama capture: shared band mapping, source masks and missing-view gates passed");
  return 0;
}
