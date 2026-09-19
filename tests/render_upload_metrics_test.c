#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "platform/sdl/render_sdl_internal.h"
#include "presentation_upload_mirror.h"
#include "performance_metrics.h"
#include "sim/sim3d_performance.h"
#include "diorama/diorama_performance.h"

uint64_t HostClock_Nanoseconds(void) { return 1; }
uint64_t HostClock_Milliseconds(void) { return 0; }

static void BeginMeasurement(void) {
  PerformanceMetrics_Configure(false, false);
  PerformanceMetrics_Configure(true, false);
  PerformanceMetrics_PresentCompleted(0);
}

static void CheckMeasurement(unsigned calls, unsigned bytes, unsigned skipped) {
  PerformanceMetrics_PresentCompleted(1000000000);
  PerformanceSnapshot snapshot;
  PerformanceMetrics_Snapshot(&snapshot);
  assert(snapshot.ready && snapshot.presents == 2);
  assert(snapshot.counts[kPerformanceCount_UploadCalls] * 2 == calls);
  assert(snapshot.counts[kPerformanceCount_UploadBytes] * 2 == bytes);
  assert(snapshot.counts[kPerformanceCount_UploadSkipped] * 2 == skipped);
}

int main(void) {
  SDL_Surface *surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_ARGB8888);
  assert(surface);
  SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
  assert(renderer);
  ArRenderDevice device = {0};
  ArSdlRenderBackend backend = {0};
  assert(ArSdlRenderBackend_Bind(&device, &backend, renderer));
  const ArRenderTextureDesc desc = {.width = 4, .height = 4,
    .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
  ArRenderTexture texture;
  assert(ArRenderDevice_CreateTexture(&device, &desc, &texture));
  uint32_t pixels[32] = {0}; /* Source rows include padding. */
  const ArRenderRectI rect = {1, 1, 2, 2};
  BeginMeasurement();
  assert(ArRenderDevice_UpdateTexture(&device, texture, NULL, pixels, 32));
  Sim3DPerformance_AddUpload(64);
  assert(ArRenderDevice_UpdateTexture(&device, texture, &rect, pixels, 32));
  Sim3DPerformance_AddUpload(16);
  CheckMeasurement(2, 80, 0);

  PresentationUploadMirror mirror = {0};
  PresentationUploadResult result;
  assert(PresentationUploadMirror_UploadArgb8888(&mirror, &device, texture,
      (const uint8_t *)pixels, 4, 4, 32, 0, 0, &result));
  BeginMeasurement();
  assert(PresentationUploadMirror_UploadArgb8888(&mirror, &device, texture,
      (const uint8_t *)pixels, 4, 4, 32, 0, 0, &result));
  DioramaPerformance_AddPlaneSync(true, result.changed, result.uploaded_bytes);
  CheckMeasurement(0, 0, 1);

  BeginMeasurement();
  pixels[10] = 0xff123456;
  assert(PresentationUploadMirror_UploadArgb8888(&mirror, &device, texture,
      (const uint8_t *)pixels, 4, 4, 32, 0, 0, &result));
  DioramaPerformance_AddPlaneSync(true, result.changed, result.uploaded_bytes);
  CheckMeasurement(1, 4, 0);
  PresentationUploadMirror_Reset(&mirror);
  ArRenderDevice_DestroyTexture(&device, texture);
  ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  puts("render_upload_metrics_test: PASS");
  return 0;
}
