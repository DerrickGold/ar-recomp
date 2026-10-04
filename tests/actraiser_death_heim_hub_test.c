/* Native eye ownership, authored face bands, independent water and the hidden
 * A/B page swap are tested against the actual game capture pass. */
#include "support/test_assert.h"
#include "actraiser/actraiser_death_heim_hub.h"
#include "actraiser/actraiser_rtl.h"
#include "actraiser/actraiser_action_bg.h"
#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "diorama/diorama.h"
#include "host/host_frame_surfaces.h"
#include "present/display_geometry.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kSnesWramSize];
Settings g_settings;
bool g_diorama_frame_active;
uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
static ActRaiserDisplayGeometry geometry = {.extra_top = 32};
const ActRaiserDisplayGeometry *const g_actraiser_display_geometry = &geometry;
static SrPpuFrameTransactionContext frame;
static DioramaRoomOverride room;
static bool have_room, native_edits, capture_ready, stamped;
static uint8_t tile_band;
static uint32_t planes[kDioramaPlane_Count][
    SR_PPU_SURFACE_MAX_WIDTH * SR_PPU_SURFACE_MAX_HEIGHT];
static uint32_t *eye_mask;
static int capture_width;

const SrPpuFrameTransactionContext *ActRaiser_PpuFrame(void) { return &frame; }
const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void) {
  return have_room ? &room : NULL;
}
bool ActRaiser_ConfigurePpuObjCapture(const SrPpuObjCaptureRequest *request) {
  assert(request->range_first == 12 && request->range_count == 4);
  assert(request->flags == SR_PPU_OBJ_CAPTURE_WINNERS);
  assert(request->range_pitch_bytes == (capture_width + 2 * SR_PPU_OBJ_APRON) * sizeof(uint32_t));
  assert(request->range_pixel_byte_size >= request->range_pitch_bytes * kActRaiserAuthenticHeight);
  eye_mask = (uint32_t *)request->range_pixels;
  memset(eye_mask, 0, request->range_pixel_byte_size);
  return capture_ready;
}
ActRaiserSpriteOwnership ActRaiserSpriteOwnership_Presented(uint8_t group, uint8_t map) {
  return (ActRaiserSpriteOwnership){.valid = true, .group = group, .map = map};
}
bool ActRaiserSpriteOwnership_Range(const ActRaiserSpriteOwnership *ownership,
    ActRaiserSpriteRole role, uint8_t *first, uint8_t *count) {
  assert(ownership->valid && role == kActRaiserSprite_StatueEyes);
  *first = 12;
  *count = 4;
  return true;
}
bool ActRaiserActionBg_CaptureTilesBound(unsigned bg) {
  assert(bg == 1);
  return native_edits;
}
bool ActRaiserActionBg_StampAt(unsigned bg, int x, int y,
    uint16_t hscroll, uint16_t vscroll, uint16_t *entry, uint8_t *band,
    uint8_t *local_x, uint8_t *local_y, bool *black, bool *blank) {
  if (!stamped) return false;
  *blank = false;
  return ActRaiserActionBg_NativeSceneryAt(bg, x, y, hscroll, vscroll,
      entry, band, local_x, local_y, black);
}
bool ActRaiserActionBg_NativeSceneryAt(unsigned bg, int x, int y,
    uint16_t hscroll, uint16_t vscroll, uint16_t *entry, uint8_t *band,
    uint8_t *local_x, uint8_t *local_y, bool *black) {
  assert(bg == 1 && hscroll == 0 && vscroll == 0);
  *entry = 0;
  *band = tile_band;
  *local_x = x & 15;
  *local_y = y & 15;
  *black = false;
  return true;
}
static size_t At(int x, int y) {
  return (size_t)(y + geometry.extra_top) *
      (capture_width + 2 * SR_PPU_OBJ_APRON) + x +
      (capture_width - kActRaiserAuthenticWidth) / 2 + SR_PPU_OBJ_APRON;
}
static size_t EyeAt(int x, int y) {
  return At(x, y) - geometry.extra_top * (capture_width + 2 * SR_PPU_OBJ_APRON);
}
static void Reset(int extra) {
  geometry.render_extra = extra;
  capture_width = kActRaiserAuthenticWidth + extra * 2;
  memset(planes, 0, sizeof(planes));
  memset(&frame, 0, sizeof(frame));
  for (int p = 0; p < kDioramaPlane_Count; p++)
    g_diorama_layer_pixels[p] = (uint8_t *)planes[p];
  g_ram[kActRaiserWram_MapGroup] = 7;
  g_ram[kActRaiserWram_CurrentMap] = 1;
  g_ram[kActRaiserWram_DeathHeimProgress] = 7;
  frame.state.background_tilemap_control[0] = 0x60;
  frame.state.background_tilemap_control[1] = 0x70;
  g_diorama_frame_active = capture_ready = true;
  g_settings.diorama_skybox = kDioramaSky_Only;
  native_edits = have_room = stamped = false;
  eye_mask = NULL;
  ActRaiser_DioramaDeathHeimEyesPrepare();
}

static void CheckEyePromotion(int extra) {
  Reset(extra);
  const size_t face = At(15, 48), water = At(15, 144), other = At(30, 48);
  const uint32_t colour = 0xff8899aau;
  planes[SR_PPU_OVERLAY_BG2][face] = 0xff223344u;
  planes[SR_PPU_OVERLAY_BG2][water] = 0xff445566u;
  planes[kDioramaPlane_Obj2][face] = colour;
  planes[kDioramaPlane_Obj2][other] = colour; /* Same colour, different owner. */
  eye_mask[EyeAt(15, 48)] = colour;
  ActRaiser_DioramaDeathHeimHubStatuesFinish(capture_width);
  assert(ActRaiser_DioramaDeathHeimHubFacesPromoted());
  assert(planes[kDioramaPlane_Bg2Far][face] == colour);
  assert(!planes[SR_PPU_OVERLAY_BG2][face] && !planes[kDioramaPlane_Obj2][face]);
  assert(planes[kDioramaPlane_Obj2][other] == colour);
  assert(planes[SR_PPU_OVERLAY_BG2][water] == 0xff445566u);
  assert(!planes[kDioramaPlane_Bg2Far][water]);
}

int main(void) {
  CheckEyePromotion(0);
  CheckEyePromotion(120);
  Reset(0);
  const size_t face = At(15, 48), other = At(30, 48);
  const uint32_t colour = 0xff8899aau;

  /* Per-tile authoring controls where the eyes live, without re-promoting
   * ordinary scenery above the water boundary into the far band. */
  for (unsigned stamp = 0; stamp < 2; stamp++)
  for (tile_band = 0; tile_band <= 2; tile_band++) {
    Reset(0);
    stamped = stamp != 0;
    have_room = native_edits = true;
    room = (DioramaRoomOverride){.used = true, .map_group = 7, .map_number = 1};
    assert(DioramaLayerOrder_ParseLine(&room, "bg2-virtual = cells:0,0-15,8 band:0", NULL));
    planes[kDioramaPlane_Bg2Far][other] = 0xff223344u;
    planes[SR_PPU_OVERLAY_BG2][face] = 0xff223344u;
    planes[kDioramaPlane_Obj2][face] = colour;
    eye_mask[EyeAt(15, 48)] = colour;
    ActRaiser_DioramaDeathHeimHubStatuesFinish(kActRaiserAuthenticWidth);
    assert(planes[SR_PPU_OVERLAY_BG2][face] == 0xff223344u);
    assert(planes[kDioramaPlane_Bg2Far][face] == (tile_band ? 0 : colour));
    assert(planes[kDioramaPlane_Obj2][face] == (tile_band ? colour : 0));
    DioramaLayerOrder_ClearRoom(&room);
  }

  /* Progress and music may advance while faces remain visible. Only the two
   * page changes select B; after that neither stale eye masks nor A's room
   * overrides may move completion scenery into the face band. */
  for (unsigned pages = 1; pages <= 3; pages++) {
    Reset(0);
    g_ram[kActRaiserWram_SongSelection] = 3;
    if (pages & 1u) frame.state.background_tilemap_control[0] = 0x64;
    if (pages & 2u) frame.state.background_tilemap_control[1] = 0x74;
    planes[SR_PPU_OVERLAY_BG2][face] = colour;
    ActRaiser_DioramaDeathHeimHubStatuesFinish(kActRaiserAuthenticWidth);
    assert(ActRaiser_DioramaDeathHeimHubFacesPromoted() == (pages != 3));
    assert(planes[kDioramaPlane_Bg2Far][face] == (pages == 3 ? 0 : colour));
  }
  ActRaiser_DioramaDeathHeimEyesPrepare();
  puts("Death Heim faces: owned eyes, authored depth, water and under-black A/B switch passed");
  return 0;
}
