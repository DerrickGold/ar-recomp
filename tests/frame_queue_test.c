#include "host/frame_queue.h"
#include "action/action_voxel.h"
#include "action/action_trunk.h"
#include "host/frame_producer.h"
#include "sim/sim_frame_capture.h"
#include "diorama/diorama_planes.h"
#include "support/test_assert.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Packet ownership validates guard layout but must never split geometry.
 * Keep the full action compiler outside this focused queue executable. */
void ActionTrunk_Split(const ActionTrunkAssets *assets, int origin_x, int origin_y,
    int width, int height, int pitch, const uint32_t *const input[3],
    const size_t input_pitch[3], uint32_t *const output[3], unsigned brightness) {
  (void)assets; (void)origin_x; (void)origin_y; (void)width; (void)height;
  (void)pitch; (void)input; (void)input_pitch; (void)output; (void)brightness;
  assert(!"Queue ownership must not invoke ActionTrunk_Split");
  abort();
}

static SrPpuSurfaceView View(uint32_t *pixels) {
  return (SrPpuSurfaceView){.data = (const uint8_t *)pixels,
      .byte_size = 16, .pitch_bytes = 8, .width_pixels = 2, .height_pixels = 2,
      .flags = SR_PPU_SURFACE_BOUND, .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32};
}

static void TestCastleOwnership(void) {
  HostFrameQueue *q=HostFrameQueue_Create(); assert(q);
  ActionVoxelGeometry *geometry=ActionVoxel_Create(); assert(geometry);
  HostFramePacket *p=HostFrameQueue_BeginWrite(q); assert(p);
  p->frame=(FrameSlot){.diorama_active=true,.diorama_voxel=geometry};
  assert(HostFramePacket_OwnPixels(p)); HostFrameQueue_Publish(q);
  ActionVoxel_Release(geometry); /* producer can leave the room now */
  const HostFramePacket *read=HostFrameQueue_Read(q);assert(read);
  const ActionVoxelGeometry *display=ActionVoxel_Retain(read->frame.diorama_voxel);
  HostFrameQueue_Release(q); HostFrameQueue_Destroy(q);
  unsigned count;assert(ActionVoxel_Edges(display,&count) && count==0);
  ActionVoxel_Release(display);
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
      .diorama_obj_owner_tags=true,
      .diorama_plane_request_mask = (1u << kDioramaPlane_Bg1Hi) | (1u << kDioramaPlane_ObjAttributes2),
      .diorama_plane_content_mask = (1u << kDioramaPlane_Bg1Hi) | (1u << kDioramaPlane_ObjAttributes2)};
  uint32_t attributes[]={0x074000ffu,0xff40ff00u,0,0x084000ffu};
  f->diorama_obj_attributes[2]=View(attributes);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0] = View(pixels);
  f->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0] = View(pixels);
  f->hud_icon = (HudIconFrame){.lifetime_generation = 7,
                               .frame_serial = 12,
                               .x = 144,
                               .y = 11,
                               .first = 11,
                               .count = 4,
                               .scene_removed = true};
  f->hud_icon.surface = f->diorama_skybox_surface = View(pixels);
  f->voxel_sprite_atlas=View(pixels);
  f->voxel_sprites.count=1;
  f->voxel_sprites.sprites[0]=(ActionVoxelSprite){.owner=0xee0,.owner_tag=7,.x=24,.y=180,
      .component_x=64,.component_y=32,.solid_y=4,.solid_height=12,.join_left=0x1234};
  f->ppu_surfaces.main = f->ppu_surfaces.authentic = f->ppu_surfaces.mode7 = View(pixels);
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == 7 * sizeof(pixels));
  memset(pixels, 0, sizeof(pixels));
  memset(attributes,0,sizeof(attributes));
  const uint32_t *owned=(const uint32_t *)f->diorama_obj_attributes[2].data;
  assert(f->diorama_obj_owner_tags && owned!=attributes && owned[0]==0x074000ffu &&
      owned[1]==0xff40ff00u && owned[3]==0x084000ffu && f->voxel_sprites.sprites[0].owner_tag==7);
  const uint32_t *copy = (const uint32_t *)f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1].data;
  assert(copy != pixels && copy[0] == 1 && copy[3] == 4);
  copy=(const uint32_t *)f->voxel_sprite_atlas.data;
  assert(copy!=pixels && copy[0]==1 && copy[3]==4 && f->voxel_sprites.sprites[0].owner==0xee0);
  assert(f->voxel_sprites.sprites[0].component_x==64 && f->voxel_sprites.sprites[0].component_y==32 &&
      f->voxel_sprites.sprites[0].solid_y==4 && f->voxel_sprites.sprites[0].solid_height==12 &&
      f->voxel_sprites.sprites[0].join_left==0x1234);
  assert(!f->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0].data);
  assert(!f->ppu_surfaces.main.data && !f->ppu_surfaces.authentic.data && !f->ppu_surfaces.mode7.data);
  assert(f->hud_icon.surface.data && f->diorama_skybox_surface.data);
  assert(f->hud_icon.scene_removed && f->hud_icon.frame_serial == 12 &&
         f->hud_icon.lifetime_generation == 7 && f->hud_icon.x == 144 && f->hud_icon.count == 4);
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
  p->frame.hud_icon.surface = p->frame.diorama_skybox_surface = View(pixels);
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == SrPpuBgPacket_Size(packet) + sizeof(pixels));
  assert(!p->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0].data);
  assert(!p->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0].data);
  assert(!p->frame.diorama_skybox_surface.data && p->frame.diorama_skybox_surface.width_pixels == 2);
  assert(p->frame.hud_icon.surface.data &&
         p->frame.hud_icon.surface.data != (const uint8_t *)pixels);
  free(packet);
  SrPpuBgPacket *direct = HostFramePacket_BackgroundTarget(p);
  assert(direct && direct == p->background_storage);
  SrPpuBgPacket_Begin(direct, 2, 2);
  direct->words[2] = direct->owned_sources = 3;
  p->frame = (FrameSlot){.diorama_active = true, .background_packet = direct};
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == 0 && p->frame.background_packet == direct);
  assert(direct->words[2] == 3);
  /* A recycled slot with CPU-only output must not retain the previous export. */
  p->frame = (FrameSlot){.diorama_active = true};
  assert(HostFramePacket_OwnPixels(p) && p->copied_bytes == 0);
}

static void TestSimPacket(HostFramePacket *p) {
  uint32_t pixels[] = {0xff001122, 0xff334455, 0xff667788, 0xff99aabb};
  SimFrameInputs *inputs = HostFramePacket_SimTarget(p);
  assert(inputs && inputs == HostFramePacket_SimTarget(p));
  *inputs = (SimFrameInputs){0};
  inputs->wram[0x12000] = 7;
  inputs->vram[123] = 0xface;
  inputs->cgram[45] = 0x1234;
  FrameSlot *f = &p->frame;
  *f = (FrameSlot){.sim = {.view = kSimView_Enhanced, .town = 3,
      .master_enabled = true, .separated_valid = true, .underlay_serial = 17,
      .atlas_valid = true, .separated_plane_mask = 1}, .sim_inputs = inputs};
  assert(HostFramePacket_Supports(f));
  f->sim.separated_valid = false;
  assert(!HostFramePacket_Supports(f));
  f->sim.separated_valid = true;
  f->sim.view = kSimView_WorldNavigation;
  assert(!HostFramePacket_Supports(f));
  f->sim.view = kSimView_Enhanced;
  f->sim_menu.model.phase = kSimMenu_Browse;
  assert(!HostFramePacket_Supports(f));
  f->sim_menu.model.phase = kSimMenu_Closed;
  f->hud_icon = (HudIconFrame){.frame_serial = 23, .x = 144, .y = 11,
      .first = 11, .count = 4, .scene_removed = true};
  f->ppu_surfaces.main = f->ppu_surfaces.authentic = View(pixels);
  f->sim3d_output_surfaces.upload_plane_mask = 1;
  f->sim3d_output_surfaces.planes[0] = View(pixels);
  f->sim3d_output_surfaces.atlas = f->sim3d_output_surfaces.flat = View(pixels);
  f->sim3d_output_surfaces.hud_bg = f->hud_icon.surface = View(pixels);
  assert(HostFramePacket_OwnPixels(p));
  assert(p->copied_bytes == 6 * sizeof(pixels) + sizeof(*inputs));
  memset(pixels, 0, sizeof(pixels));
  assert(!f->ppu_surfaces.authentic.data && !f->background_packet);
  assert(((const uint32_t *)f->sim3d_output_surfaces.atlas.data)[3] == 0xff99aabb);
  assert(((const uint32_t *)f->sim3d_output_surfaces.planes[0].data)[0] == 0xff001122);
  assert(((const uint32_t *)f->sim3d_output_surfaces.hud_bg.data)[1] == 0xff334455);
  assert(((const uint32_t *)f->hud_icon.surface.data)[3] == 0xff99aabb);
  assert(f->hud_icon.frame_serial == 23 && f->hud_icon.scene_removed &&
         f->hud_icon.x == 144 && f->hud_icon.count == 4);
  assert(f->sim_inputs->wram[0x12000] == 7 && f->sim_inputs->vram[123] == 0xface);
  /* The 512-row atlas is taller than a PPU surface. Own its final row too. */
  uint32_t *atlas = calloc(512 * 512, sizeof(uint32_t));
  assert(atlas);
  atlas[512 * 512 - 1] = 0xffabcdef;
  f->sim3d_output_surfaces.atlas = (SrPpuSurfaceView){
    .data = (const uint8_t *)atlas, .byte_size = 512 * 512 * 4,
    .pitch_bytes = 512 * 4, .width_pixels = 512, .height_pixels = 512,
    .flags = SR_PPU_SURFACE_BOUND, .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32,
  };
  /* Reinitialize other borrowed views; don't recopy overlapping packet storage. */
  f->ppu_surfaces.main = f->sim3d_output_surfaces.planes[0] = View(pixels);
  f->sim3d_output_surfaces.flat = f->sim3d_output_surfaces.hud_bg =
      f->hud_icon.surface = View(pixels);
  assert(HostFramePacket_OwnPixels(p));
  free(atlas);
  assert(((const uint32_t *)f->sim3d_output_surfaces.atlas.data)[512 * 512 - 1] == 0xffabcdef);
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

typedef struct Stream {
  HostFrameQueue *queue;
  unsigned next, stop_at;
  SDL_ThreadID owner;
} Stream;

static void ProduceUntilPaused(void *context) {
  Stream *stream = context;
  if (stream->owner) assert(stream->owner == SDL_GetCurrentThreadID());
  stream->owner = SDL_GetCurrentThreadID();
  while (!HostFrameQueue_PauseRequested(stream->queue) &&
         (!stream->stop_at || stream->next < stream->stop_at)) {
    HostFramePacket *packet = HostFrameQueue_BeginWrite(stream->queue);
    if (!packet) { SDL_DelayNS(1000); continue; }
    uint32_t pixels[] = {stream->next, 1, 2, 3};
    packet->frame = (FrameSlot){.diorama_active = true, .hud_icon.surface = View(pixels)};
    SrPpuBgPacket *background = HostFramePacket_BackgroundTarget(packet);
    assert(background);
    SrPpuBgPacket_Begin(background, 2, 2);
    background->words[2] = 3;
    background->words[SR_PPU_BG_PACKET_ARENA_BASE] = stream->next;
    background->words[3]++;
    packet->frame.background_packet = background;
    if (stream->next & 1u) {
      SimFrameInputs *inputs = HostFramePacket_SimTarget(packet);
      assert(inputs);
      inputs->wram[0x12000] = (uint8_t)stream->next;
      inputs->vram[9] = (uint16_t)(stream->next + 7);
      packet->frame.diorama_active = false;
      packet->frame.sim = (SimFrameData){.view = kSimView_Enhanced, .town = 3,
          .master_enabled = true, .separated_valid = true, .underlay_serial = 17,
          .atlas_valid = true};
      packet->frame.sim_inputs = inputs;
      packet->frame.sim3d_output_surfaces.atlas = View(pixels);
    }
    packet->tick = (int)stream->next;
    packet->source_ns = (uint64_t)stream->next * 16666667;
    assert(HostFramePacket_OwnPixels(packet));
    if (stream->next & 1u) {
      assert(!packet->frame.background_packet);
      assert(packet->copied_bytes == sizeof(*packet->sim_storage) + 2 * sizeof(pixels));
    } else {
      assert(packet->frame.background_packet == background);
      assert(packet->copied_bytes == sizeof(pixels));
    }
    HostFrameQueue_Publish(stream->queue);
    ++stream->next;
  }
}

static void AwaitFullQueue(HostFrameQueue *queue) {
  const uint64_t timeout = SDL_GetTicksNS() + 2000000000;
  while (HostFrameQueue_ReadyCount(queue) < 3) {
    assert(SDL_GetTicksNS() < timeout);
    SDL_DelayNS(1000);
  }
}

static void AssertHeldPacket(const HostFramePacket *held, unsigned tick) {
  if (tick & 1u) {
    assert(held->frame.sim_inputs == held->sim_storage);
    assert(held->frame.sim_inputs->wram[0x12000] == tick);
    assert(held->frame.sim_inputs->vram[9] == tick + 7);
    assert(((const uint32_t *)held->frame.sim3d_output_surfaces.atlas.data)[0] == tick);
  } else {
    assert(held->frame.background_packet == held->background_storage);
    assert(held->frame.background_packet->words[SR_PPU_BG_PACKET_ARENA_BASE] == tick);
  }
  assert(((const uint32_t *)held->frame.hud_icon.surface.data)[0] == tick);
}

static void TestMaintenanceWithFutureFrames(HostFrameQueue *queue) {
  Stream stream = {.queue = queue};
  assert(HostFrameProducer_Init(NULL, NULL));
  assert(HostFrameProducer_Submit(ProduceUntilPaused, &stream));
  AwaitFullQueue(queue);
  HostFrameQueue_RequestPause(queue);
  HostFrameProducer_Wait();
  for (unsigned tick = 0; tick < 64; ++tick) {
    const HostFramePacket *held = HostFrameQueue_Read(queue);
    assert(held && held->tick == (int)tick);
    assert(stream.next == tick + 3);
    assert(held->source_ns == (uint64_t)tick * 16666667);
    AssertHeldPacket(held, tick);
    /* Maintenance reads the idle owner's state; restart must not discard or
     * overwrite this retained packet, even when the producer immediately runs. */
    /* Exercise both an explicit pause and a producer returning after its own
     * maintenance deadline, without requiring the consumer to drain first. */
    stream.stop_at = (tick & 1) ? tick + 4 : 0;
    HostFrameQueue_Resume(queue);
    assert(HostFrameProducer_Submit(ProduceUntilPaused, &stream));
    assert(HostFrameQueue_Read(queue) == held);
    AssertHeldPacket(held, tick);
    HostFrameQueue_Release(queue);
    AwaitFullQueue(queue);
    HostFrameQueue_RequestPause(queue);
    HostFrameProducer_Wait();
  }
  for (unsigned tick = 64; tick < 67; ++tick) {
    assert(HostFrameQueue_Read(queue)->tick == (int)tick);
    HostFrameQueue_Release(queue);
  }
  assert(!HostFrameQueue_Read(queue));
  HostFrameProducer_Shutdown();
}

static void TestTrunkGuardOwnership(void) {
  HostFrameQueue *q = HostFrameQueue_Create(); assert(q);
  uint32_t *pixels = calloc(kDioramaTrunkGuardPixels, sizeof(uint32_t)); assert(pixels);
  DioramaTrunkGuardSource source = {.pixels = pixels, .pitch_pixels = 640,
      .pixel_count = kDioramaTrunkGuardPixels};
  assert(DioramaTrunkGuard_Resolve(208, 1696, 624, 352,
      (DioramaTrunkGuardBounds){0, 0, 1024, 2048}, &source.layout));
  for (unsigned frame = 0; frame < 2; ++frame) {
    HostFramePacket *packet = HostFrameQueue_BeginWrite(q); assert(packet);
    pixels[0] = 0xff123400u + frame;
    pixels[kDioramaTrunkGuardPixels - 1] = frame + 17;
    source.capture_generation = frame + 100;
    packet->frame = (FrameSlot){.diorama_active = true, .diorama_trunk_guard = source};
    assert(HostFramePacket_OwnPixels(packet));
    assert(packet->frame.diorama_trunk_guard.pixels == packet->trunk_guard_storage);
    assert(packet->copied_bytes == kDioramaTrunkGuardPixels * sizeof(uint32_t));
    HostFrameQueue_Publish(q);
  }
  memset(pixels, 0, kDioramaTrunkGuardPixels * sizeof(uint32_t));
  for (unsigned frame = 0; frame < 2; ++frame) {
    const HostFramePacket *packet = HostFrameQueue_Peek(q, frame); assert(packet);
    const DioramaTrunkGuardSource *held = &packet->frame.diorama_trunk_guard;
    assert(held->pixels != pixels && held->pixels[0] == 0xff123400u + frame &&
        held->pixels[kDioramaTrunkGuardPixels - 1] == frame + 17 &&
        held->capture_generation == frame + 100);
    assert(!memcmp(&held->layout, &source.layout, sizeof(source.layout)));
  }
  HostFramePacket *packet = HostFrameQueue_BeginWrite(q); assert(packet);
  packet->frame = (FrameSlot){.diorama_active = true, .diorama_trunk_guard = source};
  packet->frame.diorama_trunk_guard.pixel_count = 1;
  assert(!HostFramePacket_OwnPixels(packet));
  packet->frame.diorama_trunk_guard = source;
  packet->frame.diorama_trunk_guard.pitch_pixels = kDioramaTrunkGuardWidth - 1;
  assert(!HostFramePacket_OwnPixels(packet));
  packet->frame.diorama_trunk_guard = source;
  packet->frame.diorama_trunk_guard.layout.strips[1].atlas_y = 0;
  assert(!HostFramePacket_OwnPixels(packet));
  packet->frame.diorama_trunk_guard = source;
  packet->frame.diorama_trunk_guard.pixels = NULL;
  assert(HostFramePacket_OwnPixels(packet));
  assert(!packet->frame.diorama_trunk_guard.capture_generation && !packet->trunk_guard_storage);
  /* The same clearing contract applies after this slot has owned a guard.
   * Retaining its lazy allocation must not publish stale metadata or pixels. */
  packet->frame.diorama_trunk_guard = source;
  assert(HostFramePacket_OwnPixels(packet));
  uint32_t *storage = packet->trunk_guard_storage; assert(storage);
  packet->frame.diorama_trunk_guard.pixels = NULL;
  assert(HostFramePacket_OwnPixels(packet));
  const DioramaTrunkGuardSource empty = {0};
  assert(!memcmp(&packet->frame.diorama_trunk_guard, &empty, sizeof(empty)));
  assert(packet->trunk_guard_storage == storage && packet->copied_bytes == 0);
  HostFrameQueue_Destroy(q); free(pixels);
}

static uint32_t VoxelGuardPixel(unsigned frame, unsigned band, unsigned pixel) {
  if (pixel % 29 == 0) return 0;
  if (pixel % 31 == 0) return UINT32_C(0xff000000);
  return UINT32_C(0xff000000) | ((frame + 1) << 20) | ((band + 1) << 16) | pixel;
}

static void TestVoxelGuardOwnership(void) {
  HostFrameQueue *q = HostFrameQueue_Create(); assert(q);
  const size_t lane_bytes = kDioramaVoxelGuardPixels * sizeof(uint32_t);
  uint32_t *input = malloc(3 * lane_bytes); assert(input);
  uint32_t primary[4];
  DioramaVoxelGuardSource source = {.pitch_pixels = kDioramaVoxelGuardWidth,
      .pixel_count = kDioramaVoxelGuardPixels};
  assert(DioramaVoxelGuard_Resolve(-16, 384, 624, 352,
      (DioramaTrunkGuardBounds){0, 0, 1024, 1024}, &source.layout));
  for (unsigned band = 0; band < 3; ++band)
    source.pixels[band] = input + band * kDioramaVoxelGuardPixels;
  for (unsigned frame = 0; frame < 2; ++frame) {
    HostFramePacket *packet = HostFrameQueue_BeginWrite(q); assert(packet);
    for (unsigned band = 0; band < 3; ++band)
      for (unsigned pixel = 0; pixel < kDioramaVoxelGuardPixels; ++pixel)
        input[band * kDioramaVoxelGuardPixels + pixel] = VoxelGuardPixel(frame, band, pixel);
    for (unsigned pixel = 0; pixel < 4; ++pixel) primary[pixel] = 100 * (frame + 1) + pixel;
    source.capture_generation = frame + 700;
    packet->frame = (FrameSlot){.diorama_active = true, .diorama_voxel_guard = source,
        .diorama_plane_request_mask = 1u << SR_PPU_OVERLAY_BG1,
        .diorama_plane_content_mask = 1u << SR_PPU_OVERLAY_BG1};
    packet->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0] = View(primary);
    assert(HostFramePacket_OwnPixels(packet));
    assert(packet->copied_bytes == sizeof(primary) + 3 * lane_bytes);
    for (unsigned band = 0; band < 3; ++band) {
      assert(packet->frame.diorama_voxel_guard.pixels[band] ==
          packet->voxel_guard_storage + band * kDioramaVoxelGuardPixels);
      assert(packet->frame.diorama_voxel_guard.pixels[band] != source.pixels[band]);
    }
    HostFrameQueue_Publish(q);
  }
  /* Reusing every producer byte may not alter either queued generation, nor
   * collapse native Far into the overlapping Low/High source lanes. */
  memset(input, 0xa5, 3 * lane_bytes); memset(primary, 0, sizeof(primary));
  const HostFramePacket *first = HostFrameQueue_Peek(q, 0);
  const HostFramePacket *second = HostFrameQueue_Peek(q, 1);
  assert(first && second && first->voxel_guard_storage != second->voxel_guard_storage);
  for (unsigned frame = 0; frame < 2; ++frame) {
    const HostFramePacket *packet = HostFrameQueue_Peek(q, frame); assert(packet);
    const DioramaVoxelGuardSource *held = &packet->frame.diorama_voxel_guard;
    assert(DioramaVoxelGuard_Valid(held));
    assert(held->capture_generation == frame + 700 && held->pixel_count == kDioramaVoxelGuardPixels);
    assert(!memcmp(&held->layout, &source.layout, sizeof(source.layout)));
    for (unsigned band = 0; band < 3; ++band)
      for (unsigned pixel = 0; pixel < kDioramaVoxelGuardPixels; ++pixel)
        assert(held->pixels[band][pixel] == VoxelGuardPixel(frame, band, pixel));
    const uint32_t *owned_primary = (const uint32_t *)packet->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0].data;
    assert(owned_primary != primary);
    for (unsigned pixel = 0; pixel < 4; ++pixel) assert(owned_primary[pixel] == 100 * (frame + 1) + pixel);
  }

  HostFramePacket *packet = HostFrameQueue_BeginWrite(q); assert(packet);
  packet->frame = (FrameSlot){.diorama_active = true, .diorama_voxel_guard = source};
  assert(HostFramePacket_OwnPixels(packet));
  uint32_t *storage = packet->voxel_guard_storage; assert(storage);
  const DioramaVoxelGuardSource empty = {0};
  for (unsigned invalid = 0; invalid < 11; ++invalid) {
    packet->frame = (FrameSlot){.diorama_active = true, .diorama_voxel_guard = source,
        .diorama_plane_request_mask = 1u << SR_PPU_OVERLAY_BG1,
        .diorama_plane_content_mask = 1u << SR_PPU_OVERLAY_BG1};
    primary[0] = invalid + 900;
    packet->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0] = View(primary);
    DioramaVoxelGuardSource *bad = &packet->frame.diorama_voxel_guard;
    switch (invalid) {
      case 0: case 1: case 2: bad->pixels[invalid] = NULL; break;
      case 3: bad->capture_generation = 0; break;
      case 4: --bad->pitch_pixels; break;
      case 5: --bad->pixel_count; break;
      case 6: ++bad->layout.lower.world_y; break;
      case 7: ++bad->layout.lower.atlas_y; break;
      case 8: --bad->layout.lower.valid.width; break;
      case 9: bad->layout.lower.width = kDioramaVoxelGuardWidth + 1; break;
      case 10: bad->layout.primary_height = 0; break;
    }
    assert(!DioramaVoxelGuard_Valid(bad));
    assert(HostFramePacket_OwnPixels(packet)); /* Optional failure preserves primary. */
    assert(!memcmp(&packet->frame.diorama_voxel_guard, &empty, sizeof(empty)));
    assert(packet->voxel_guard_storage == storage && packet->copied_bytes == sizeof(primary));
    const uint32_t *owned = (const uint32_t *)packet->frame.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0].data;
    primary[0] = 0;
    assert(owned != primary && owned[0] == invalid + 900);
  }
  /* Recovery reuses the allocation without leaking a prior invalid descriptor. */
  packet->frame.diorama_voxel_guard = source;
  assert(HostFramePacket_OwnPixels(packet));
  assert(packet->voxel_guard_storage == storage && DioramaVoxelGuard_Valid(&packet->frame.diorama_voxel_guard));
  assert(packet->copied_bytes == sizeof(primary) + 3 * lane_bytes);
  HostFrameQueue_Destroy(q); free(input);
}

int main(void) {
  TestTrunkGuardOwnership();
  TestVoxelGuardOwnership();
  TestCastleOwnership();
  HostFrameQueue *q = HostFrameQueue_Create();
  assert(q && !HostFrameQueue_Read(q));
  assert(HostFrameQueue_ReadyCount(q) == 0);
  assert(!HostFrameQueue_Peek(q, 1));
  TestPixels(HostFrameQueue_BeginWrite(q));
  TestBackgroundPacket(HostFrameQueue_BeginWrite(q));
  TestSimPacket(HostFrameQueue_BeginWrite(q));
  for (int i = 0; i < 3; ++i) {
    HostFramePacket *p = HostFrameQueue_BeginWrite(q);
    assert(p); p->tick = i;
    HostFrameQueue_Publish(q);
    assert(HostFrameQueue_ReadyCount(q) == (unsigned)i + 1);
  }
  assert(!HostFrameQueue_BeginWrite(q));
  assert(HostFrameQueue_Peek(q, 1)->tick == 1);
  assert(HostFrameQueue_Peek(q, 2)->tick == 2);
  assert(!HostFrameQueue_Peek(q, 3));
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
    const HostFramePacket *next = HostFrameQueue_Peek(q, 1);
    if (next) assert(next->tick == i + 1);
    for (int b = 0; b < 128; ++b) assert(p->pixels[b] == (i & 255));
    HostFrameQueue_Release(q);
  }
  SDL_WaitThread(thread, NULL);
  TestMaintenanceWithFutureFrames(q);
  HostFrameQueue_Destroy(q);
  puts("frame_queue_test: PASS");
  return 0;
}
