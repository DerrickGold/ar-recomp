#include "present_sim3d_canvas.h"

#include <stdio.h>
#include "render/upload_rect_run.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_town_canvas.h"

typedef enum SimCanvasState {
  kSimCanvas_NeedsFull,
  kSimCanvas_Ready,
  kSimCanvas_RetryFull,
  kSimCanvas_Unavailable,
} SimCanvasState;

static struct {
  ArRenderTexture texture;
  uint32_t serial;
  SimCanvasState state;
} s_canvas;

static bool UploadRect(ArRenderDevice *device, ArRenderRectI rect) {
  if (!ArRenderDevice_UpdateTexture(device, s_canvas.texture, &rect,
          SimTownCanvas_Pixels() + (size_t)rect.y * kSimTownCanvasPixels + rect.x,
          kSimTownCanvasPixels * (int)sizeof(uint32_t)))
    return false;
  Sim3DPerformance_AddUpload((uint64_t)rect.w * rect.h * sizeof(uint32_t));
  return true;
}

static bool UploadFull(ArRenderDevice *device) {
  const ArRenderRectI full = {0, 0, kSimTownCanvasPixels, kSimTownCanvasPixels};
  if (!UploadRect(device, full)) return false;
  int x, y, width, height;
  while (SimTownCanvas_TakeDirtyRect(&x, &y, &width, &height)) {}
  return true;
}

void PresentSim3DCanvas_Upload(ArRenderDevice *device, bool needed) {
  if (s_canvas.state == kSimCanvas_Unavailable) return;
  if (!needed) {
    s_canvas.serial = 0;
    s_canvas.state = kSimCanvas_NeedsFull;
    return;
  }
  const uint32_t serial = SimTownCanvas_Serial();
  if (!serial || !ArRenderDevice_IsReady(device)) return;
  if (!ArRenderTexture_IsValid(s_canvas.texture)) {
    const ArRenderTextureDesc desc = {
      .width = kSimTownCanvasPixels, .height = kSimTownCanvasPixels,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_canvas.texture)) {
      s_canvas.state = kSimCanvas_Unavailable;
      fprintf(stderr, "[sim3d-canvas] texture allocation failed: %s\n",
          ArRenderDevice_LastError(device));
      return;
    }
  }
  if (s_canvas.state == kSimCanvas_Ready && s_canvas.serial == serial) return;

  bool uploaded = true;
  if (s_canvas.state != kSimCanvas_Ready) {
    uploaded = UploadFull(device);
  } else {
    /* The coalescer may resend clean texels from the complete CPU image. */
    UploadRectRun run = {0};
    ArRenderRectI rect;
    int x, y, width, height;
    bool found_dirty = false;
    while (uploaded && SimTownCanvas_TakeDirtyRect(&x, &y, &width, &height)) {
      found_dirty = true;
      if (UploadRectRun_Add(&run, (ArRenderRectI){x, y, width, height}, &rect))
        uploaded = UploadRect(device, rect);
    }
    if (uploaded && UploadRectRun_Finish(&run, &rect))
      uploaded = UploadRect(device, rect);
    if (!found_dirty) uploaded = UploadFull(device);
  }
  if (uploaded) {
    s_canvas.serial = serial;
    s_canvas.state = kSimCanvas_Ready;
  } else {
    s_canvas.serial = 0; /* Never expose a partially updated generation. */
    if (s_canvas.state == kSimCanvas_RetryFull) {
      ArRenderDevice_DestroyTexture(device, s_canvas.texture);
      s_canvas.texture = ArRenderTexture_Invalid();
      s_canvas.state = kSimCanvas_Unavailable;
      fprintf(stderr, "[sim3d-canvas] full retry failed; disabling town canvas: %s\n",
          ArRenderDevice_LastError(device));
    } else {
      s_canvas.state = kSimCanvas_RetryFull;
    }
  }
}

ArRenderTexture PresentSim3DCanvas_Texture(uint32_t serial) {
  return serial && s_canvas.state == kSimCanvas_Ready && serial == s_canvas.serial
      ? s_canvas.texture : ArRenderTexture_Invalid();
}

void PresentSim3DCanvas_Reset(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_canvas.texture);
  s_canvas.texture = ArRenderTexture_Invalid();
  s_canvas.serial = 0;
  s_canvas.state = kSimCanvas_NeedsFull;
}
