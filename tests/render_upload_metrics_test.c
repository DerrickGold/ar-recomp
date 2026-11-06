#include "support/test_assert.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#include "platform/sdl/render_sdl_internal.h"
#include "present/presentation_upload_mirror.h"
#include "app/performance_metrics.h"
#include "sim/sim3d/sim3d_performance.h"
#include "diorama/diorama_performance.h"

#if defined(_MSC_VER)
static __declspec(thread) uint64_t test_now = 1;
#else
static _Thread_local uint64_t test_now = 1;
#endif
uint64_t HostClock_Nanoseconds(void) { return test_now; }
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

static int MeasureWorker(void *unused) {
  (void)unused;
  test_now = 1000000;
  const DioramaPerformanceScope work = DioramaPerformance_Begin(kDioramaPerformance_Submit);
  test_now = 8000000;
  DioramaPerformance_End(work);
  return 0;
}

static void CheckNestedSubmission(void) {
  BeginMeasurement();
  test_now = 1000000;
  const DioramaPerformanceScope outer = DioramaPerformance_Begin(kDioramaPerformance_Submit);
  test_now = 2000000;
  const DioramaPerformanceScope nested = DioramaPerformance_Begin(kDioramaPerformance_Submit);
  const DioramaPerformanceScope mesh = DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  test_now = 4000000;
  DioramaPerformance_End(mesh);
  test_now = 5000000;
  DioramaPerformance_End(nested);
  /* Another thread's submission must not be hidden by this thread's scope. */
  SDL_Thread *worker = SDL_CreateThread(MeasureWorker,"nested action timing",NULL);
  assert(worker);
  int status = -1; SDL_WaitThread(worker,&status); assert(status == 0);
  test_now = 10000000;
  DioramaPerformance_End(outer);
  /* A deferred drain outside the outer submission is an additional 4 ms. */
  test_now = 11000000;
  const DioramaPerformanceScope deferred = DioramaPerformance_Begin(kDioramaPerformance_Submit);
  test_now = 15000000;
  DioramaPerformance_End(deferred);
  DioramaPerformance_End(DioramaPerformance_Begin((DioramaPerformanceStage)-1));
  DioramaPerformance_End(DioramaPerformance_Begin(kDioramaPerformanceStage_Count));
  PerformanceMetrics_PresentCompleted(1000000000);
  PerformanceSnapshot snapshot; PerformanceMetrics_Snapshot(&snapshot);
  const PerformanceValue submit = snapshot.stages[kPerformance_ActionFirst+kDioramaPerformance_Submit];
  const PerformanceValue meshes = snapshot.stages[kPerformance_ActionFirst+kDioramaPerformance_Mesh];
  assert(snapshot.ready && snapshot.presents == 2);
  assert(submit.calls == 3 && submit.mean_ms == 10 && submit.maximum_ms == 9);
  assert(meshes.calls == 1 && meshes.mean_ms == 1 && meshes.maximum_ms == 2);
  PerformanceMetrics_Configure(false,false);
  DioramaPerformance_End(DioramaPerformance_Begin(kDioramaPerformance_Submit));
}

int main(void) {
  CheckNestedSubmission();
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
