#include "sim3d_performance.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host_clock.h"

enum {
  kNoPerformanceStage = -1,
  kPerformanceWindowNs = 1000000000,
};

typedef struct Sim3DPerformanceCounters {
  uint64_t elapsed_ns;
  uint64_t calls;
  uint64_t draws;
  uint64_t vertices;
  uint64_t indices;
  uint64_t uploads;
  uint64_t upload_bytes;
} Sim3DPerformanceCounters;

static Sim3DPerformanceCounters
    s_counters[kSim3DPerformanceStage_Count];
static int s_current_stage = kNoPerformanceStage;
static uint64_t s_window_started_ns;
static uint64_t s_presentations;

static const char *const kStageNames[] = {
  "upload", "backdrop", "underlay", "terrain", "depth-voxel",
  "depth-cull", "depth-mountain", "depth-project", "depth-submit",
  "shadow", "billboard", "effects", "cloud", "host-ui",
};
_Static_assert(
    sizeof(kStageNames) / sizeof(kStageNames[0]) ==
        kSim3DPerformanceStage_Count,
    "every SIM 3D performance stage needs a report label");

bool Sim3DPerformance_Enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = (getenv("AR_PERF") || getenv("AR_SIM3D_PERF")) ? 1 : 0;
  return enabled != 0;
}

Sim3DPerformanceScope Sim3DPerformance_Begin(
    Sim3DPerformanceStage stage) {
  Sim3DPerformanceScope scope = {
    .stage = stage,
    .previous_stage = s_current_stage,
  };
  if (!Sim3DPerformance_Enabled() || stage < 0 ||
      stage >= kSim3DPerformanceStage_Count)
    return scope;
  scope.started_ns = HostClock_Nanoseconds();
  scope.active = true;
  s_current_stage = (int)stage;
  return scope;
}

void Sim3DPerformance_End(Sim3DPerformanceScope scope) {
  if (!scope.active) return;
  uint64_t now_ns = HostClock_Nanoseconds();
  Sim3DPerformanceCounters *counter = &s_counters[scope.stage];
  counter->elapsed_ns += now_ns - scope.started_ns;
  counter->calls++;
  s_current_stage = scope.previous_stage;
}

void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  if (!Sim3DPerformance_Enabled() ||
      s_current_stage < 0 || s_current_stage >= kSim3DPerformanceStage_Count)
    return;
  Sim3DPerformanceCounters *counter = &s_counters[s_current_stage];
  counter->draws++;
  counter->vertices += vertices;
  counter->indices += indices;
}

void Sim3DPerformance_AddUpload(uint64_t bytes) {
  /* Upload helpers are shared by flat/action presentations. Count them only
   * while the enhanced-SIM upload scope is active, otherwise an action-stage
   * frame rendered before entering a town pollutes the first SIM report. */
  if (!Sim3DPerformance_Enabled() ||
      s_current_stage != kSim3DPerformance_Upload)
    return;
  Sim3DPerformanceCounters *counter =
      &s_counters[kSim3DPerformance_Upload];
  counter->uploads++;
  counter->upload_bytes += bytes;
}

static void ReportPerformance(uint64_t window_ns) {
  const double divisor = s_presentations ? (double)s_presentations : 1.0;
  fprintf(stderr,
          "[sim3d-perf] presents=%" PRIu64 " window=%.2fs",
          s_presentations, (double)window_ns / 1000000000.0);
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    const Sim3DPerformanceCounters *counter = &s_counters[stage];
    if (!counter->calls && !counter->draws && !counter->uploads) continue;
    fprintf(stderr, " %s=%.3fms", kStageNames[stage],
            (double)counter->elapsed_ns / 1000000.0 / divisor);
  }
  fputc('\n', stderr);

  uint64_t draws = 0;
  uint64_t vertices = 0;
  uint64_t indices = 0;
  const Sim3DPerformanceCounters *upload =
      &s_counters[kSim3DPerformance_Upload];
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    draws += s_counters[stage].draws;
    vertices += s_counters[stage].vertices;
    indices += s_counters[stage].indices;
  }
  fprintf(stderr,
          "[sim3d-work] draws/present=%.1f vertices/present=%.1f "
          "indices/present=%.1f uploads=%" PRIu64 " upload-MiB=%.2f",
          (double)draws / divisor, (double)vertices / divisor,
          (double)indices / divisor, upload->uploads,
          (double)upload->upload_bytes / (1024.0 * 1024.0));
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    const Sim3DPerformanceCounters *counter = &s_counters[stage];
    if (!counter->draws) continue;
    fprintf(stderr, " %s=%.1fd/%.1fv", kStageNames[stage],
            (double)counter->draws / divisor,
            (double)counter->vertices / divisor);
  }
  fputc('\n', stderr);
}

void Sim3DPerformance_EndPresentation(void) {
  if (!Sim3DPerformance_Enabled()) return;
  uint64_t now_ns = HostClock_Nanoseconds();
  if (!s_window_started_ns) s_window_started_ns = now_ns;
  s_presentations++;
  uint64_t window_ns = now_ns - s_window_started_ns;
  if (window_ns < (uint64_t)kPerformanceWindowNs) return;
  ReportPerformance(window_ns);
  memset(s_counters, 0, sizeof(s_counters));
  s_presentations = 0;
  s_window_started_ns = now_ns;
}
