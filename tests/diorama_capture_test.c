#include "diorama/diorama_capture.h"

#include "support/test_assert.h"
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
static uint32_t pixel_content;
uint32_t ActRaiser_DioramaPixelContentMask(void) { return pixel_content; }
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

static void TestPixelCaptureRowsAndWindows(void) {
  uint32_t pixels[3][7] = {{0}};
  uint32_t *rows[3] = {pixels[0] + 1, pixels[1] + 1, pixels[2] + 1};
  const uint8_t codes[5] = {0, 1, 2, 3, 0};
  const uint32_t mask = DioramaCapture_PaintBlackRow(0, codes, rows, 5);
  assert(mask == (Bit(kDioramaPlane_Bg1Far) | Bit(SR_PPU_OVERLAY_BG1) |
                  Bit(kDioramaPlane_Bg1Hi)));
  for (unsigned band = 0; band < 3; band++) for (unsigned x = 0; x < 7; x++)
    assert(pixels[band][x] == (x == band + 2 ? 0xff000000u : 0));
  rows[1] = NULL;
  assert(!(DioramaCapture_PaintBlackRow(1, codes, rows, 5) & Bit(SR_PPU_OVERLAY_BG2)));
  assert(!DioramaCapture_PaintBlackRow(2, codes, rows, 5));
  SrPpuStateSnapshot state = {.main_screen = 1, .main_windowed = 1,
      .window_select = 2, .window1_left = 10, .window1_right = 20};
  assert(DioramaCapture_PixelLayerVisible(&state, 0, 9));
  assert(!DioramaCapture_PixelLayerVisible(&state, 0, 10));
  assert(!DioramaCapture_PixelLayerVisible(&state, 0, 20));
  assert(DioramaCapture_PixelLayerVisible(&state, 0, 21));
  assert(!DioramaCapture_PixelLayerVisible(&state, 1, 9));
  state.window_select |= 1; /* inverted window */
  assert(!DioramaCapture_PixelLayerVisible(&state, 0, 9));
  assert(DioramaCapture_PixelLayerVisible(&state, 0, 10));
  state.main_screen = 0;
  state.sub_screen = 1;
  assert(DioramaCapture_PixelLayerVisible(&state, 0, 9));
  state.sub_windowed = 1;
  assert(!DioramaCapture_PixelLayerVisible(&state, 0, 9));
  state.flags = SR_PPU_STATE_FORCED_BLANK;
  assert(!DioramaCapture_PixelLayerVisible(&state, 0, 10));
}

static void TestStampedTilePixels(void) {
  static uint16_t vram[0x8000], cgram[256];
  SrPpuStateSnapshot state = {.brightness = 15};
  state.backgrounds[0].bits_per_pixel = 4;
  state.backgrounds[0].tile_base_word = 0x7ff0;
  /* Character one wraps VRAM; one bit in each plane gives distinct pixels. */
  vram[2] = 0x4080;
  vram[10] = 0x1020;
  cgram[17] = 31;          /* red */
  cgram[18] = 31u << 5;   /* green */
  cgram[20] = 31u << 10;  /* blue */
  cgram[24] = 0x7fff;     /* white */
  const uint16_t entry = 1 | (1u << 10);
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 0, 2, 0)
      == 0xffff0000u);
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 1, 2, 0)
      == 0xff00ff00u);
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 2, 2, 0)
      == 0xff0000ffu);
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 3, 2, 0)
      == 0xffffffffu);
  assert(!DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 4, 2, 0));
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry | 0xc000u, 7, 5, 0)
      == 0xffff0000u);
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 0, 2,
      SR_PPU_OVERLAY_MARK_BG_HALF_ADD) == 0x80ff0000u);
  state.fixed_color = 31;
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 0, 2,
      SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT) == 0xff000000u);
  state.brightness = 0;
  assert(DioramaCapture_StampColor(&state, 0, vram, cgram, entry, 0, 2, 0)
      == 0xff000000u);

  uint32_t pixels[3][6], *rows[3] = {pixels[0], pixels[1], pixels[2]};
  for (unsigned band = 0; band < 3; band++)
    for (unsigned x = 0; x < 6; x++) pixels[band][x] = 0x12345678;
  const uint8_t codes[6] = {0, 4, 5, 6, 4, 7};
  const uint32_t colors[6] = {0, 0xffff0000, 0xff00ff00, 0xff0000ff, 0, 0};
  assert(DioramaCapture_PaintEditedRow(0, codes, colors, 0, rows, 6) ==
      (Bit(kDioramaPlane_Bg1Far) | Bit(SR_PPU_OVERLAY_BG1) | Bit(kDioramaPlane_Bg1Hi)));
  for (unsigned band = 0; band < 3; band++) {
    assert(pixels[band][0] == 0x12345678 && pixels[band][5] == 0x12345678);
    for (unsigned x = 1; x < 5; x++)
      assert(pixels[band][x] == (x == band + 1 ? colors[x] : 0));
  }
  DioramaCapture_PaintEditedRow(0, codes, colors, 0xff102030, rows, 6);
  assert(pixels[1][4] == 0xff102030 && !pixels[0][4] && !pixels[2][4]);
}

int main(void) {
  TestStampedTilePixels();
  TestPixelCaptureRowsAndWindows();
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

  pixel_content = Bit(kDioramaPlane_Bg1Hi);
  Capture(&ppu);
  assert(slot.diorama_plane_content_mask ==
         (Bit(kDioramaPlane_Backdrop) | pixel_content));
  pixel_content = 0;

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
