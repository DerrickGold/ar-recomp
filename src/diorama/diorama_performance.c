#include "diorama_performance.h"

#include <SDL3/SDL.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kDioramaPerformanceWindowNs = 1000000000 };

typedef struct DioramaPerformanceCounter {
  uint64_t elapsed_ns;
  uint64_t calls;
} DioramaPerformanceCounter;

typedef struct DioramaPerformanceData {
  DioramaPerformanceCounter counters[kDioramaPerformanceStage_Count];
  uint64_t window_started_ns;
  uint64_t presentations;
  uint64_t plane_syncs;
  uint64_t plane_sync_failures;
  uint64_t texture_uploads;
  uint64_t upload_bytes;
  uint64_t draws;
  uint64_t draw_failures;
  uint64_t vertices;
  uint64_t indices;
} DioramaPerformanceData;

static DioramaPerformanceData s_data;
static SDL_SpinLock s_data_lock;

static const char *const kDioramaStageNames[] = {
  "total", "upload", "frame-analysis", "producer-setup", "scanout",
  "producer-finish", "host-post", "frame-synthesis", "mesh",
  "supersample", "submit", "callback",
};
_Static_assert(
    sizeof(kDioramaStageNames) / sizeof(kDioramaStageNames[0]) ==
        kDioramaPerformanceStage_Count,
    "every Diorama performance stage needs a report label");

bool DioramaPerformance_Enabled(void) {
  static SDL_InitState initialized;
  static bool enabled;
  if (SDL_ShouldInit(&initialized)) {
    enabled = getenv("AR_PERF") || getenv("AR_ACTION_PERF");
    SDL_SetInitialized(&initialized, true);
  }
  return enabled;
}

static uint64_t DioramaPerformanceNow(void) {
  return SDL_GetTicksNS();
}

DioramaPerformanceScope DioramaPerformance_Begin(
    DioramaPerformanceStage stage) {
  DioramaPerformanceScope scope = {.stage = stage};
  if (!DioramaPerformance_Enabled() || stage < 0 ||
      stage >= kDioramaPerformanceStage_Count)
    return scope;
  scope.started_ns = DioramaPerformanceNow();
  scope.active = true;
  return scope;
}

void DioramaPerformance_End(DioramaPerformanceScope scope) {
  if (!scope.active) return;
  const uint64_t elapsed_ns = SDL_GetTicksNS() - scope.started_ns;
  SDL_LockSpinlock(&s_data_lock);
  DioramaPerformanceCounter *counter = &s_data.counters[scope.stage];
  counter->elapsed_ns += elapsed_ns;
  counter->calls++;
  SDL_UnlockSpinlock(&s_data_lock);
}

void DioramaPerformance_AddPlaneSync(bool succeeded, bool uploaded,
                                     uint64_t uploaded_bytes) {
  if (!DioramaPerformance_Enabled()) return;
  SDL_LockSpinlock(&s_data_lock);
  s_data.plane_syncs++;
  if (!succeeded) {
    s_data.plane_sync_failures++;
  } else if (uploaded) {
    s_data.texture_uploads++;
    s_data.upload_bytes += uploaded_bytes;
  }
  SDL_UnlockSpinlock(&s_data_lock);
}

void DioramaPerformance_AddDraw(bool succeeded, int vertices, int indices) {
  if (!DioramaPerformance_Enabled()) return;
  SDL_LockSpinlock(&s_data_lock);
  s_data.draws++;
  if (!succeeded) {
    s_data.draw_failures++;
  } else {
    if (vertices > 0) s_data.vertices += (uint64_t)vertices;
    if (indices > 0) s_data.indices += (uint64_t)indices;
  }
  SDL_UnlockSpinlock(&s_data_lock);
}

static void DioramaPerformanceReport(const DioramaPerformanceData *data,
                                     uint64_t window_ns) {
  const double presentations =
      data->presentations ? (double)data->presentations : 1.0;
  uint64_t child_ns = 0;
  /* Upload and frame analysis run before presentation ownership transfers to
   * the compositor, so they are reported beside total but are not children. */
  for (int stage = kDioramaPerformance_FrameSynthesis;
       stage < kDioramaPerformanceStage_Count; stage++)
    child_ns += data->counters[stage].elapsed_ns;
  const uint64_t total_ns =
      data->counters[kDioramaPerformance_Total].elapsed_ns;
  const uint64_t other_ns = total_ns > child_ns ? total_ns - child_ns : 0;

  fprintf(stderr, "[diorama-perf] presents=%" PRIu64 " window=%.2fs",
          data->presentations, (double)window_ns / 1000000000.0);
  for (int stage = 0; stage < kDioramaPerformanceStage_Count; stage++) {
    const DioramaPerformanceCounter *counter = &data->counters[stage];
    if (!counter->calls) continue;
    fprintf(stderr, " %s=%.3fms", kDioramaStageNames[stage],
            (double)counter->elapsed_ns / 1000000.0 / presentations);
  }
  fprintf(stderr, " other=%.3fms\n",
          (double)other_ns / 1000000.0 / presentations);

  fprintf(stderr,
          "[diorama-work] sync/present=%.1f uploads/present=%.1f "
          "upload-KiB/present=%.1f draws/present=%.1f "
          "vertices/present=%.1f indices/present=%.1f "
          "failures={sync:%" PRIu64 ",draw:%" PRIu64 "}\n",
          (double)data->plane_syncs / presentations,
          (double)data->texture_uploads / presentations,
          (double)data->upload_bytes / 1024.0 / presentations,
          (double)data->draws / presentations,
          (double)data->vertices / presentations,
          (double)data->indices / presentations,
          data->plane_sync_failures, data->draw_failures);
}

void DioramaPerformance_PresentCompleted(void) {
  if (!DioramaPerformance_Enabled()) return;
  const uint64_t now = DioramaPerformanceNow();
  DioramaPerformanceData snapshot;
  bool report = false;

  SDL_LockSpinlock(&s_data_lock);
  if (!s_data.window_started_ns) s_data.window_started_ns = now;
  s_data.presentations++;
  const uint64_t window_ns = now - s_data.window_started_ns;
  if (window_ns >= (uint64_t)kDioramaPerformanceWindowNs) {
    snapshot = s_data;
    memset(&s_data, 0, sizeof(s_data));
    s_data.window_started_ns = now;
    report = true;
  }
  SDL_UnlockSpinlock(&s_data_lock);

  if (report) DioramaPerformanceReport(&snapshot, window_ns);
}
