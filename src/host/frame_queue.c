#include "host/frame_queue.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "diorama/diorama_planes.h"
#include "present/presentation_surface.h"

enum { kQueueCapacity = 3 };
struct HostFrameQueue {
  HostFramePacket packets[kQueueCapacity];
  atomic_uint_fast64_t published, consumed;
  atomic_bool pause;
};

HostFrameQueue *HostFrameQueue_Create(void) {
  HostFrameQueue *q = calloc(1, sizeof(*q));
  if (!q) return NULL;
  atomic_init(&q->published, 0);
  atomic_init(&q->consumed, 0);
  atomic_init(&q->pause, false);
  for (int i = 0; i < kQueueCapacity; ++i) {
    HostFramePacket *p = &q->packets[i];
    p->capacity = (size_t)SR_PPU_SURFACE_MAX_WIDTH * SR_PPU_SURFACE_MAX_HEIGHT *
        sizeof(uint32_t) * (kDioramaPlane_Count + 2);
    p->pixels = malloc(p->capacity);
    if (!p->pixels) {
      HostFrameQueue_Destroy(q);
      return NULL;
    }
  }
  return q;
}

void HostFrameQueue_Destroy(HostFrameQueue *q) {
  if (!q) return;
  for (int i = 0; i < kQueueCapacity; ++i) {
    free(q->packets[i].pixels);
    free(q->packets[i].background_storage);
  }
  free(q);
}

HostFramePacket *HostFrameQueue_BeginWrite(HostFrameQueue *q) {
  const uint64_t w = atomic_load_explicit(&q->published, memory_order_relaxed);
  const uint64_t r = atomic_load_explicit(&q->consumed, memory_order_acquire);
  return w - r < kQueueCapacity ? &q->packets[w % kQueueCapacity] : NULL;
}

void HostFrameQueue_Publish(HostFrameQueue *q) {
  atomic_fetch_add_explicit(&q->published, 1, memory_order_release);
}

const HostFramePacket *HostFrameQueue_Read(HostFrameQueue *q) {
  return HostFrameQueue_Peek(q, 0);
}

const HostFramePacket *HostFrameQueue_Peek(HostFrameQueue *q, unsigned offset) {
  const uint64_t r = atomic_load_explicit(&q->consumed, memory_order_relaxed);
  const uint64_t w = atomic_load_explicit(&q->published, memory_order_acquire);
  return offset < w - r ? &q->packets[(r + offset) % kQueueCapacity] : NULL;
}

unsigned HostFrameQueue_ReadyCount(const HostFrameQueue *q) {
  const uint64_t r = atomic_load_explicit(&q->consumed, memory_order_relaxed);
  const uint64_t w = atomic_load_explicit(&q->published, memory_order_acquire);
  return (unsigned)(w - r);
}

void HostFrameQueue_Release(HostFrameQueue *q) {
  atomic_fetch_add_explicit(&q->consumed, 1, memory_order_release);
}

void HostFrameQueue_RequestPause(HostFrameQueue *q) {
  atomic_store_explicit(&q->pause, true, memory_order_release);
}

bool HostFrameQueue_PauseRequested(const HostFrameQueue *q) {
  return atomic_load_explicit(&q->pause, memory_order_acquire);
}

void HostFrameQueue_Resume(HostFrameQueue *q) {
  atomic_store_explicit(&q->pause, false, memory_order_release);
}

static bool CopyView(HostFramePacket *p, SrPpuSurfaceView *view, bool needed) {
  if (!needed || !view->data) {
    *view = (SrPpuSurfaceView){0};
    return true;
  }
  if (!PresentationSurface_Bound(view) || !view->width_pixels || view->width_pixels > SR_PPU_SURFACE_MAX_WIDTH ||
      !view->height_pixels || view->height_pixels > SR_PPU_SURFACE_MAX_HEIGHT ||
      !PresentationSurface_Holds(view, (int)view->width_pixels, (int)view->height_pixels))
    return false;
  const size_t pitch = (size_t)view->width_pixels * sizeof(uint32_t);
  const size_t bytes = pitch * view->height_pixels;
  if (p->copied_bytes > p->capacity || bytes > p->capacity - p->copied_bytes)
    return false;
  uint8_t *dst = p->pixels + p->copied_bytes;
  if (pitch == view->pitch_bytes) memcpy(dst, view->data, bytes);
  else for (unsigned y = 0; y < view->height_pixels; ++y)
    memcpy(dst + y * pitch, view->data + y * view->pitch_bytes, pitch);
  view->data = dst;
  view->pitch_bytes = pitch;
  view->byte_size = bytes;
  p->copied_bytes += bytes;
  return true;
}

SrPpuBgPacket *HostFramePacket_BackgroundTarget(HostFramePacket *p) {
  if (!p) return NULL;
  if (!p->background_storage) p->background_storage = malloc(sizeof(SrPpuBgPacket));
  return p->background_storage;
}

bool HostFramePacket_OwnPixels(HostFramePacket *p) {
  FrameSlot *f = &p->frame;
  if (!HostFramePacket_Supports(f)) return false;
  const uint32_t mask = f->diorama_plane_request_mask & f->diorama_plane_content_mask &
      ~DioramaPlanes_GpuOwnedMask(f->background_packet);
  bool needed[SR_PPU_OVERLAY_SOURCE_COUNT][SR_PPU_SURFACE_BAND_COUNT] = {{false}};
  for (unsigned source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source)
    needed[source][0] = (mask & (1u << source)) != 0;
  size_t count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&count);
  for (size_t i = 0; i < count; ++i)
    needed[bands[i].source][bands[i].band] = (mask & (1u << bands[i].plane)) != 0;
  /* HUD sources coexist with the game plane mask, including flat HUD mode. */
  needed[SR_PPU_OVERLAY_BG3][0] = true;
  needed[SR_PPU_OVERLAY_OBJ][0] = true;
  p->copied_bytes = 0;
  size_t background_copied = 0;
  if (f->background_packet) {
    if (!HostFramePacket_BackgroundTarget(p)) return false;
    if (f->background_packet != p->background_storage) {
      background_copied = SrPpuBgPacket_Size(f->background_packet);
      memcpy(p->background_storage, f->background_packet, background_copied);
    }
    f->background_packet = p->background_storage;
  }
  if (!CopyView(p, &f->ppu_surfaces.main, (mask & (1u << kDioramaPlane_Backdrop)) != 0) ||
      !CopyView(p, &f->hud_obj_surface, true)) return false;
  if (f->background_packet && (f->background_packet->owned_sources & 4u))
    f->diorama_skybox_surface.data = NULL;
  else if (!CopyView(p, &f->diorama_skybox_surface, true)) return false;
  for (unsigned source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source)
    for (unsigned band = 0; band < SR_PPU_SURFACE_BAND_COUNT; ++band)
      if (!CopyView(p, &f->ppu_surfaces.overlays[source][band], needed[source][band]))
        return false;
  f->ppu_surfaces.authentic = (SrPpuSurfaceView){0};
  f->ppu_surfaces.mode7 = (SrPpuSurfaceView){0};
  f->authentic_frame_serial = 0;
  memset(&f->sim3d_output_surfaces, 0, sizeof(f->sim3d_output_surfaces));
  p->copied_bytes += background_copied;
  return true;
}
