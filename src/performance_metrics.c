#include "performance_metrics.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

enum { kIntervalCapacity = 512, kReportNs = 1000000000 };
static atomic_uint s_epoch;
static uint32_t s_generation;
static bool s_log;
static struct {
  atomic_uint_fast64_t elapsed, maximum, calls;
} s_pending[kPerformanceStage_Count];
static atomic_uint_fast64_t s_counts[kPerformanceCount_Count];
static struct {
  PerformanceSnapshot snapshot;
  PerformanceContext context;
  uint64_t started, previous, frames, revision, maximum_interval;
  uint64_t intervals[kIntervalCapacity], interval_count;
  uint64_t elapsed[kPerformanceStage_Count], maximum[kPerformanceStage_Count], calls[kPerformanceStage_Count];
  uint64_t counts[kPerformanceCount_Count];
} s_window;

static const char *const kNames[kPerformanceStage_Count] = {
  "events/input", "emulation", "PPU + capture", "world map build", "SIM metadata", "town canvas",
  "frame snapshot", "upload", "presentation", "CRT/resolve", "host UI", "settings UI",
  "perf overlay", "present/wait", "pacing/sleep", "housekeeping",
  "PPU setup", "PPU scanout", "PPU finish", "SIM color math", "SIM HUD restore", "SIM diagnostics",
  "canvas raster", "canvas enhance", "job owner", "job helpers*", "job join wait",
  "SIM upload", "SIM backdrop", "SIM underlay", "SIM terrain", "SIM depth pass",
  "SIM depth cull", "SIM mountains", "SIM model project", "SIM depth submit",
  "SIM shadows", "SIM billboards", "SIM effects", "SIM clouds", "SIM HUD",
  "world animation", "world transfer", "world prepare", "world atmosphere", "world ocean",
  "action total", "action upload", "action analysis", "action setup", "action scanout",
  "action finish", "action host-post", "action synthesis", "action mesh", "action supersample",
  "action DOF", "action submit", "action callback",
};

const char *PerformanceMetrics_StageName(PerformanceStage stage) {
  return stage >= 0 && stage < kPerformanceStage_Count ? kNames[stage] : "unknown";
}
const char *PerformanceMetrics_SceneName(PerformanceScene scene) {
  static const char *const names[kPerformanceScene_Count] = {
    "Native/menu", "Action 3D", "Town 3D", "World 3D", "Sky Palace", "Action 2D",
  };
  return scene >= 0 && scene < kPerformanceScene_Count ? names[scene] : "unknown";
}
uint32_t PerformanceMetrics_Epoch(void) { return atomic_load_explicit(&s_epoch, memory_order_relaxed); }
bool PerformanceMetrics_Enabled(void) { return PerformanceMetrics_Epoch() != 0; }

void PerformanceMetrics_Configure(bool enabled, bool log_reports) {
  s_log = log_reports;
  if (enabled == PerformanceMetrics_Enabled()) return;
  atomic_store_explicit(&s_epoch, 0, memory_order_release);
  for (int i = 0; i < kPerformanceStage_Count; i++) {
    atomic_store(&s_pending[i].elapsed, 0); atomic_store(&s_pending[i].maximum, 0); atomic_store(&s_pending[i].calls, 0);
  }
  for (int i = 0; i < kPerformanceCount_Count; i++) atomic_store(&s_counts[i], 0);
  memset(&s_window, 0, sizeof(s_window));
  if (enabled) {
    if (!++s_generation) ++s_generation;
    atomic_store_explicit(&s_epoch, s_generation, memory_order_release);
  }
}

void PerformanceMetrics_Record(uint32_t epoch, PerformanceStage stage, uint64_t elapsed_ns) {
  if (!epoch || epoch != PerformanceMetrics_Epoch() || stage < 0 || stage >= kPerformanceStage_Count) return;
  atomic_fetch_add_explicit(&s_pending[stage].elapsed, elapsed_ns, memory_order_relaxed);
  atomic_fetch_add_explicit(&s_pending[stage].calls, 1, memory_order_relaxed);
  uint_fast64_t maximum = atomic_load_explicit(&s_pending[stage].maximum, memory_order_relaxed);
  while (maximum < elapsed_ns && !atomic_compare_exchange_weak_explicit(
      &s_pending[stage].maximum, &maximum, elapsed_ns, memory_order_relaxed, memory_order_relaxed)) {}
}
void PerformanceMetrics_Add(PerformanceCount counter, uint64_t value) {
  if (PerformanceMetrics_Enabled() && counter >= 0 && counter < kPerformanceCount_Count)
    atomic_fetch_add_explicit(&s_counts[counter], value, memory_order_relaxed);
}

static bool SameContext(const PerformanceContext *a, const PerformanceContext *b) {
  return a->scene == b->scene && a->host_mode == b->host_mode &&
      a->map_group == b->map_group && a->map_number == b->map_number &&
      a->width == b->width && a->height == b->height &&
      a->refresh_mode == b->refresh_mode && a->limit_fps == b->limit_fps && a->vsync == b->vsync;
}
void PerformanceMetrics_SetContext(const PerformanceContext *context) {
  if (!PerformanceMetrics_Enabled() || !context || SameContext(context, &s_window.context)) return;
  const uint64_t revision = s_window.revision + 1;
  memset(&s_window, 0, sizeof(s_window));
  s_window.context = *context;
  s_window.revision = revision;
  s_window.snapshot.context = *context;
  s_window.snapshot.revision = revision;
}

static int CompareIntervals(const void *a, const void *b) {
  const uint64_t aa = *(const uint64_t *)a, bb = *(const uint64_t *)b;
  return (aa > bb) - (aa < bb);
}
static void Publish(uint64_t now) {
  PerformanceSnapshot *out = &s_window.snapshot;
  *out = (PerformanceSnapshot){.context = s_window.context, .revision = ++s_window.revision,
      .presents = s_window.frames, .seconds = (now - s_window.started) / 1e9, .ready = true};
  out->fps = (s_window.frames - 1) / out->seconds;
  out->frame_mean_ms = out->seconds * 1000 / (s_window.frames - 1);
  size_t count = s_window.interval_count < kIntervalCapacity ? (size_t)s_window.interval_count : kIntervalCapacity;
  uint64_t sorted[kIntervalCapacity];
  memcpy(sorted, s_window.intervals, count * sizeof(*sorted));
  qsort(sorted, count, sizeof(*sorted), CompareIntervals);
  if (count) {
    out->frame_p95_ms = sorted[(count * 95 + 99) / 100 - 1] / 1e6;
    out->frame_max_ms = s_window.maximum_interval / 1e6;
  }
  for (int i = 0; i < kPerformanceStage_Count; i++) {
    out->stages[i] = (PerformanceValue){s_window.elapsed[i] / 1e6 / s_window.frames,
        s_window.maximum[i] / 1e6, s_window.calls[i]};
  }
  for (int i = 0; i < kPerformanceCount_Count; i++) out->counts[i] = (double)s_window.counts[i] / s_window.frames;
  if (s_log) {
    fprintf(stderr, "[pipeline-perf] scene=%s host=%d map=%02x/%02x output=%dx%d frames=%" PRIu64
        " fps=%.1f cadence-ms=%.3f p95=%.3f max=%.3f refresh=%d vsync=%d cap=%d gpu-ms=unavailable\n",
        PerformanceMetrics_SceneName(out->context.scene), out->context.host_mode,
        out->context.map_group, out->context.map_number, out->context.width, out->context.height,
        out->presents, out->fps, out->frame_mean_ms, out->frame_p95_ms, out->frame_max_ms,
        out->context.refresh_mode, (int)out->context.vsync, out->context.limit_fps);
    for (int i = 0; i < kPerformanceStage_Count; i++) if (out->stages[i].calls)
      fprintf(stderr, "[pipeline-stage] %s mean-ms=%.4f peak-call-ms=%.4f calls=%" PRIu64 "\n",
          kNames[i], out->stages[i].mean_ms, out->stages[i].maximum_ms, out->stages[i].calls);
    fprintf(stderr, "[pipeline-work] ticks=%.2f repre=%.2f draws=%.1f vertices=%.0f upload-MiB=%.3f depth-MiB=%.3f depth-copy-MiB=%.3f depth-copy-calls=%.2f jobs=%.2f helpers=%.2f fallback=%.2f failed=%.2f (per-present)\n",
        out->counts[kPerformanceCount_Ticks], out->counts[kPerformanceCount_Represents],
        out->counts[kPerformanceCount_Draws], out->counts[kPerformanceCount_Vertices],
        out->counts[kPerformanceCount_UploadBytes] / 1048576,
        out->counts[kPerformanceCount_DepthUploadBytes] / 1048576,
        out->counts[kPerformanceCount_DepthCopyBytes] / 1048576,
        out->counts[kPerformanceCount_DepthCopyCalls],
        out->counts[kPerformanceCount_WorkJobs], out->counts[kPerformanceCount_HelperJobs],
        out->counts[kPerformanceCount_Fallbacks], out->counts[kPerformanceCount_FailedPresents]);
    fprintf(stderr, "[pipeline-traffic] upload-calls=%.2f skipped=%.2f scan-MiB=%.3f upload-MiB=%.3f mirror-realloc=%.2f (per-present)\n",
        out->counts[kPerformanceCount_UploadCalls],
        out->counts[kPerformanceCount_UploadSkipped],
        out->counts[kPerformanceCount_ScanBytes] / 1048576,
        out->counts[kPerformanceCount_UploadBytes] / 1048576,
        out->counts[kPerformanceCount_MirrorReallocs]);
    fprintf(stderr, "[pipeline-path] cpu-project=%.2f cpu-stage=%.2f gpu-reuse=%.2f publish=%.2f opt-out=%.2f limit=%.2f rejected=%.2f cpu-reuse=%.2f (events/present, not view fallbacks)\n",
        out->counts[kPerformanceCount_CpuProject], out->counts[kPerformanceCount_CpuStage],
        out->counts[kPerformanceCount_GpuReuse], out->counts[kPerformanceCount_GeometryPublish],
        out->counts[kPerformanceCount_GeometryOptOut], out->counts[kPerformanceCount_GeometryLimit],
        out->counts[kPerformanceCount_GeometryRejected], out->counts[kPerformanceCount_CpuGeometryReuse]);
  }
  memset(s_window.elapsed, 0, sizeof(s_window.elapsed));
  memset(s_window.maximum, 0, sizeof(s_window.maximum));
  memset(s_window.calls, 0, sizeof(s_window.calls));
  memset(s_window.counts, 0, sizeof(s_window.counts));
  s_window.frames = s_window.interval_count = 0;
  s_window.maximum_interval = 0;
  s_window.started = s_window.previous = 0;
}

void PerformanceMetrics_PresentCompleted(uint64_t now_ns) {
  if (!PerformanceMetrics_Enabled()) return;
  for (int i = 0; i < kPerformanceStage_Count; i++) {
    s_window.elapsed[i] += atomic_exchange_explicit(&s_pending[i].elapsed, 0, memory_order_relaxed);
    s_window.calls[i] += atomic_exchange_explicit(&s_pending[i].calls, 0, memory_order_relaxed);
    const uint64_t maximum = atomic_exchange_explicit(&s_pending[i].maximum, 0, memory_order_relaxed);
    if (maximum > s_window.maximum[i]) s_window.maximum[i] = maximum;
  }
  for (int i = 0; i < kPerformanceCount_Count; i++)
    s_window.counts[i] += atomic_exchange_explicit(&s_counts[i], 0, memory_order_relaxed);
  if (!s_window.frames) s_window.started = now_ns;
  else if (now_ns >= s_window.previous) {
    const uint64_t interval = now_ns - s_window.previous;
    s_window.intervals[s_window.interval_count++ % kIntervalCapacity] = interval;
    if (interval > s_window.maximum_interval) s_window.maximum_interval = interval;
  }
  s_window.previous = now_ns;
  s_window.frames++;
  if (s_window.frames > 1 && now_ns >= s_window.started && now_ns - s_window.started >= kReportNs) Publish(now_ns);
}
void PerformanceMetrics_Snapshot(PerformanceSnapshot *snapshot) {
  if (snapshot) *snapshot = s_window.snapshot;
}
