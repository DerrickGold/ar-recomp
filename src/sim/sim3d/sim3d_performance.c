#include "sim/sim3d/sim3d_performance.h"

#include <inttypes.h>
#include <string.h>

#include "host/host_clock.h"
#include "app/diagnostic_options.h"
#include "app/performance_metrics.h"
#include "app/performance_report.h"
#include "app/runtime_log.h"

_Static_assert((int)kPerformance_SimCount == (int)kSim3DPerformanceStage_Count,
               "SIM metric stage mapping");

enum {
  kNoPerformanceStage = -1,
  kPerformanceWindowNs = 1000000000,
};

typedef struct Sim3DPerformanceCounters {
  uint64_t elapsed_ns;
  uint64_t maximum_ns;
  uint64_t calls;
  uint64_t draws;
  uint64_t vertices;
  uint64_t indices;
  uint64_t uploads;
  uint64_t upload_bytes;
  uint64_t paths[kSim3DPath_Count];
} Sim3DPerformanceCounters;

static Sim3DPerformanceCounters
    s_counters[kSim3DPerformanceStage_Count];
static int s_current_stage = kNoPerformanceStage;
static uint64_t s_window_started_ns;
static uint64_t s_presentations;
static uint64_t s_geometry_upload_bytes;

static const char *const kStageNames[] = {
  "upload", "backdrop", "underlay", "terrain", "depth-voxel",
  "depth-cull", "depth-mountain", "depth-project", "depth-submit",
  "shadow", "billboard", "effects", "cloud", "host-ui", "world-animation", "world-transfer",
  "world-prepare", "world-atmosphere", "world-ocean",
};
static const char *const kPathNames[] = {
  "cpu-project", "cpu-stage", "gpu-reuse", "publish", "opt-out", "limit", "rejected",
};
_Static_assert(sizeof(kPathNames) / sizeof(*kPathNames) == kSim3DPath_Count,
    "every geometry path needs a label");
_Static_assert(kPerformanceCount_GeometryRejected - kPerformanceCount_CpuProject + 1 ==
    kSim3DPath_Count, "geometry path metrics mapping");
_Static_assert(
    sizeof(kStageNames) / sizeof(kStageNames[0]) ==
        kSim3DPerformanceStage_Count,
    "every SIM 3D performance stage needs a report label");

bool Sim3DPerformance_LoggingEnabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = DiagnosticOptionEnabled("AR_PERF") || DiagnosticOptionEnabled("AR_SIM3D_PERF");
  return enabled != 0;
}

bool Sim3DPerformance_Enabled(void) {
  return Sim3DPerformance_LoggingEnabled() || PerformanceMetrics_Enabled();
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
  const uint64_t elapsed_ns = now_ns - scope.started_ns;
  PerformanceMetrics_Record(PerformanceMetrics_Epoch(),
      (PerformanceStage)(kPerformance_SimFirst + scope.stage), elapsed_ns);
  counter->elapsed_ns += elapsed_ns;
  if (elapsed_ns > counter->maximum_ns) counter->maximum_ns = elapsed_ns;
  counter->calls++;
  s_current_stage = scope.previous_stage;
}

void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  PerformanceMetrics_Add(kPerformanceCount_Draws, 1);
  PerformanceMetrics_Add(kPerformanceCount_Vertices, vertices);
  if (!Sim3DPerformance_Enabled() ||
      s_current_stage < 0 || s_current_stage >= kSim3DPerformanceStage_Count)
    return;
  Sim3DPerformanceCounters *counter = &s_counters[s_current_stage];
  counter->draws++;
  counter->vertices += vertices;
  counter->indices += indices;
}

void Sim3DPerformance_AddUpload(uint64_t bytes) {
  /* Global traffic is counted by the backend. These are SIM-local totals. */
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

typedef struct Sim3DReportSnapshot {
  Sim3DPerformanceCounters counters[kSim3DPerformanceStage_Count];
  uint64_t presentations, window_ns, geometry_upload_bytes;
} Sim3DReportSnapshot;

static void AppendPerformanceReport(PerformanceReport *report, const Sim3DReportSnapshot *snapshot) {
  const double divisor = snapshot->presentations ? (double)snapshot->presentations : 1.0;
  PerformanceReport_Append(report,
          "[sim3d-perf] presents=%" PRIu64 " window=%.2fs",
          snapshot->presentations, (double)snapshot->window_ns / 1000000000.0);
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    const Sim3DPerformanceCounters *counter = &snapshot->counters[stage];
    if (!counter->calls && !counter->draws && !counter->uploads) continue;
    PerformanceReport_Append(report, " %s=%.3fms", kStageNames[stage],
            (double)counter->elapsed_ns / 1000000.0 / divisor);
  }
  PerformanceReport_Append(report, "\n");

  /* A one-off asset build can disappear in the per-presentation mean. Keep
   * maxima separate from the existing mean/work report for cold-entry audits. */
  PerformanceReport_Append(report, "[sim3d-perf-max]");
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    const Sim3DPerformanceCounters *counter = &snapshot->counters[stage];
    if (!counter->calls) continue;
    PerformanceReport_Append(report, " %s=%.3fms", kStageNames[stage],
            (double)counter->maximum_ns / 1000000.0);
  }
  PerformanceReport_Append(report, "\n");

  uint64_t draws = 0;
  uint64_t vertices = 0;
  uint64_t indices = 0;
  const Sim3DPerformanceCounters *upload =
      &snapshot->counters[kSim3DPerformance_Upload];
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    draws += snapshot->counters[stage].draws;
    vertices += snapshot->counters[stage].vertices;
    indices += snapshot->counters[stage].indices;
  }
  PerformanceReport_Append(report,
          "[sim3d-work] draws/present=%.1f vertices/present=%.1f "
          "indices/present=%.1f uploads=%" PRIu64 " upload-MiB=%.2f",
          (double)draws / divisor, (double)vertices / divisor,
          (double)indices / divisor, upload->uploads,
          (double)upload->upload_bytes / (1024.0 * 1024.0));
  PerformanceReport_Append(report, " vertex-upload-MiB/present=%.3f",
      (double)snapshot->geometry_upload_bytes / divisor / (1024.0 * 1024.0));
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; stage++) {
    const Sim3DPerformanceCounters *counter = &snapshot->counters[stage];
    if (!counter->draws) continue;
    PerformanceReport_Append(report, " %s=%.1fd/%.1fv", kStageNames[stage],
            (double)counter->draws / divisor,
            (double)counter->vertices / divisor);
  }
  PerformanceReport_Append(report, "\n");
}

void Sim3DPerformance_AddPath(Sim3DPerformancePath path) {
  if (path < 0 || path >= kSim3DPath_Count) return;
  PerformanceMetrics_Add((PerformanceCount)(kPerformanceCount_CpuProject + path), 1);
  if (Sim3DPerformance_LoggingEnabled() &&
      s_current_stage >= 0 && s_current_stage < kSim3DPerformanceStage_Count)
    ++s_counters[s_current_stage].paths[path];
}

void Sim3DPerformance_AddGeometryUpload(uint64_t bytes) {
  PerformanceMetrics_Add(kPerformanceCount_DepthUploadBytes, bytes);
  if (Sim3DPerformance_Enabled() && s_current_stage >= 0 &&
      s_current_stage < kSim3DPerformanceStage_Count)
    s_geometry_upload_bytes += bytes;
}

void Sim3DPerformance_AddGeometryCopy(uint64_t bytes, uint64_t calls) {
  PerformanceMetrics_Add(kPerformanceCount_DepthCopyBytes, bytes);
  PerformanceMetrics_Add(kPerformanceCount_DepthCopyCalls, calls);
}

void Sim3DPerformance_AddAtlasCopy(uint64_t bytes) {
  PerformanceMetrics_Add(kPerformanceCount_AtlasCopyBytes, bytes);
  PerformanceMetrics_Add(kPerformanceCount_AtlasCopyCalls, 1);
}

void Sim3DPerformance_AddAtlasReuse(void) {
  PerformanceMetrics_Add(kPerformanceCount_AtlasReuse, 1);
}

static void WriteSim3DReport(FILE *output, const void *record, size_t size) {
  (void)size;
  const Sim3DReportSnapshot *snapshot = record;
  PerformanceReport report = {.output = output};
  AppendPerformanceReport(&report, snapshot);
  for (int stage = 0; stage < kSim3DPerformanceStage_Count; ++stage) {
    bool any = false;
    for (int path = 0; path < kSim3DPath_Count; ++path)
      any |= snapshot->counters[stage].paths[path] != 0;
    if (!any) continue;
    PerformanceReport_Append(&report, "[sim3d-path] stage=%s", kStageNames[stage]);
    for (int path = 0; path < kSim3DPath_Count; ++path)
      PerformanceReport_Append(&report, " %s=%.2f", kPathNames[path],
          (double)snapshot->counters[stage].paths[path] / (double)snapshot->presentations);
    PerformanceReport_Append(&report, " (events/present)\n");
  }
  PerformanceReport_Flush(&report);
}

void Sim3DPerformance_EndPresentation(void) {
  if (!Sim3DPerformance_Enabled()) return;
  uint64_t now_ns = HostClock_Nanoseconds();
  if (!s_window_started_ns) s_window_started_ns = now_ns;
  s_presentations++;
  uint64_t window_ns = now_ns - s_window_started_ns;
  if (window_ns < (uint64_t)kPerformanceWindowNs) return;
  if (Sim3DPerformance_LoggingEnabled()) {
    Sim3DReportSnapshot snapshot = {.presentations = s_presentations,
        .window_ns = window_ns, .geometry_upload_bytes = s_geometry_upload_bytes};
    memcpy(snapshot.counters, s_counters, sizeof(s_counters));
    RuntimeLog_Record(WriteSim3DReport, &snapshot, sizeof(snapshot));
  }
  memset(s_counters, 0, sizeof(s_counters));
  s_geometry_upload_bytes = 0;
  s_presentations = 0;
  s_window_started_ns = now_ns;
}
