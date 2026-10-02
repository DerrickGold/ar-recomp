#include "host/frame_queue.h"
#include "diorama/diorama_planes.h"
#include "support/test_assert.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SrPpuSurfaceView View(uint32_t *pixels) {
  return (SrPpuSurfaceView){.data = (const uint8_t *)pixels,
      .byte_size = 16, .pitch_bytes = 8, .width_pixels = 2, .height_pixels = 2,
      .flags = SR_PPU_SURFACE_BOUND, .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32};
}

static void TestPixels(HostFramePacket *p) {
  uint32_t pixels[] = {1, 2, 3, 4};
  FrameSlot *f = &p->frame;
  *f = (FrameSlot){.diorama_active = true};
  assert(HostFramePacket_Supports(f));
  f->diorama_active = false;
  assert(!HostFramePacket_Supports(f));
  f->diorama_active = true; f->scene_inspector_enabled = true;
  assert(!HostFramePacket_Supports(f));
  f->scene_inspector_enabled = false; f->bg_mode = 7;
  assert(!HostFramePacket_Supports(f));
  f->bg_mode = 1; f->m7_active = true;
  assert(!HostFramePacket_Supports(f));
  f->m7_active = false; f->sim.view = kSimView_Enhanced;
  assert(!HostFramePacket_Supports(f));
  *f = (FrameSlot){.diorama_active = true,
      .diorama_plane_request_mask = (1u << kDioramaPlane_Bg1Hi),
      .diorama_plane_content_mask = (1u << kDioramaPlane_Bg1Hi)};
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0] = View(pixels);
  f->hud_obj_surface = f->diorama_skybox_surface = View(pixels);
  f->ppu_surfaces.main = f->ppu_surfaces.authentic = f->ppu_surfaces.mode7 = View(pixels);
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == 5 * sizeof(pixels));
  memset(pixels, 0, sizeof(pixels));
  const uint32_t *copy = (const uint32_t *)f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1].data;
  assert(copy != pixels && copy[0] == 1 && copy[3] == 4);
  assert(!f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0].data);
  assert(!f->ppu_surfaces.main.data && !f->ppu_surfaces.authentic.data && !f->ppu_surfaces.mode7.data);
  assert(f->hud_obj_surface.data && f->diorama_skybox_surface.data);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1].byte_size--;
  assert(!HostFramePacket_OwnPixels(p));
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1] = View(pixels);
  const size_t capacity = p->capacity;
  p->capacity = 1;
  assert(!HostFramePacket_OwnPixels(p));
  p->capacity = capacity;
}

static void TestBackgroundPacket(HostFramePacket *p) {
  SrPpuBgPacket *packet = calloc(1, sizeof(*packet));
  assert(packet);
  packet->words[0] = 640;
  packet->words[1] = 352;
  packet->words[2] = 3;
  packet->words[SR_PPU_BG_PACKET_WORDS - 1] = 0x12345678u;
  p->frame = (FrameSlot){.diorama_active = true, .background_packet = packet};
  assert(!p->background_storage);
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == sizeof(*packet));
  assert(p->frame.background_packet != packet);
  memset(packet, 0, sizeof(*packet));
  assert(p->frame.background_packet->words[2] == 3);
  assert(p->frame.background_packet->words[SR_PPU_BG_PACKET_WORDS - 1] == 0x12345678u);
  /* Fully owned captures carry dimensions and compact packet bytes, never
   * stale CPU pixels. Unowned HUD captures remain independently copied. */
  SrPpuBgPacket_Begin(packet, 2, 2);
  packet->words[2] = packet->owned_sources = 7;
  uint32_t pixels[] = {4, 3, 2, 1};
  p->frame = (FrameSlot){.diorama_active = true, .background_packet = packet,
      .diorama_plane_request_mask = 3, .diorama_plane_content_mask = 3};
  p->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0] = View(pixels);
  p->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0] = View(pixels);
  p->frame.hud_obj_surface = p->frame.diorama_skybox_surface = View(pixels);
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == SrPpuBgPacket_Size(packet) + sizeof(pixels));
  assert(!p->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0].data);
  assert(!p->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0].data);
  assert(!p->frame.diorama_skybox_surface.data && p->frame.diorama_skybox_surface.width_pixels == 2);
  assert(p->frame.hud_obj_surface.data && p->frame.hud_obj_surface.data != (const uint8_t *)pixels);
  free(packet);
  /* A recycled slot with CPU-only output must not retain the previous export. */
  p->frame = (FrameSlot){.diorama_active = true};
  assert(HostFramePacket_OwnPixels(p) && p->copied_bytes == 0);
}

enum { kFrames = 100000 };
static int Writer(void *context) {
  HostFrameQueue *q = context;
  for (int i = 0; i < kFrames; ++i) {
    HostFramePacket *p;
    while (!(p = HostFrameQueue_BeginWrite(q))) SDL_DelayNS(1000);
    p->tick = i;
    p->source_ns = (uint64_t)i * 12345;
    memset(p->pixels, i & 255, 128);
    HostFrameQueue_Publish(q);
  }
  return 0;
}

int main(void) {
  HostFrameQueue *q = HostFrameQueue_Create();
  assert(q && !HostFrameQueue_Read(q));
  assert(HostFrameQueue_ReadyCount(q) == 0);
  TestPixels(HostFrameQueue_BeginWrite(q));
  TestBackgroundPacket(HostFrameQueue_BeginWrite(q));
  for (int i = 0; i < 3; ++i) {
    HostFramePacket *p = HostFrameQueue_BeginWrite(q);
    assert(p); p->tick = i;
    HostFrameQueue_Publish(q);
    assert(HostFrameQueue_ReadyCount(q) == (unsigned)i + 1);
  }
  assert(!HostFrameQueue_BeginWrite(q));
  const HostFramePacket *held = HostFrameQueue_Read(q);
  assert(held && held->tick == 0 && !HostFrameQueue_BeginWrite(q));
  for (int i = 0; i < 3; ++i) {
    assert(HostFrameQueue_Read(q)->tick == i);
    HostFrameQueue_Release(q);
    assert(HostFrameQueue_ReadyCount(q) == 2u - (unsigned)i);
  }
  assert(!HostFrameQueue_Read(q));
  HostFrameQueue_RequestPause(q);
  assert(HostFrameQueue_PauseRequested(q));
  HostFrameQueue_Resume(q);
  assert(!HostFrameQueue_PauseRequested(q));
  SDL_Thread *thread = SDL_CreateThread(Writer, "queue-test", q);
  assert(thread);
  for (int i = 0; i < kFrames; ++i) {
    const HostFramePacket *p;
    while (!(p = HostFrameQueue_Read(q))) SDL_DelayNS(1000);
    assert(p->tick == i && p->source_ns == (uint64_t)i * 12345);
    for (int b = 0; b < 128; ++b) assert(p->pixels[b] == (i & 255));
    HostFrameQueue_Release(q);
  }
  SDL_WaitThread(thread, NULL);
  HostFrameQueue_Destroy(q);
  puts("frame_queue_test: PASS");
  return 0;
}
