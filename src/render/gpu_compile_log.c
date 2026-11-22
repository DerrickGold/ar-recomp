#include "render/gpu_compile_log.h"
#include "app/runtime_log.h"

#include <stdio.h>

#include "app/diagnostic_options.h"

static const double kNanosecondsPerMillisecond = 1e6;

typedef struct GpuCompileLogState {
  bool preparing;
  unsigned builds;
  unsigned failures;
  uint64_t total_ns;
  uint64_t slowest_ns;
  const char *slowest_label;
  const char *slowest_stage;
  GpuCompileLogObserver observer;
  void *observer_context;
} GpuCompileLogState;

/* A preparation worker owns a separate bracket. Unbracketed late builds
 * remain on the render owner, which also reads and resets the totals. */
static _Thread_local GpuCompileLogState s_log;
static struct {
  unsigned builds, failures;
  uint64_t total_ns, maximum_ns;
} s_late;

void GpuCompileLog_Record(const char *label, const char *stage, uint64_t elapsed_ns,
                          bool succeeded) {
  const double ms = (double)elapsed_ns / kNanosecondsPerMillisecond;
  if (!label) label = "unnamed";
  if (s_log.preparing) {
    s_log.builds++;
    s_log.failures += !succeeded;
    s_log.total_ns += elapsed_ns;
    if (elapsed_ns >= s_log.slowest_ns) {
      s_log.slowest_ns = elapsed_ns;
      s_log.slowest_label = label;
      s_log.slowest_stage = stage;
    }
    if (s_log.observer) s_log.observer(s_log.observer_context);
    if (succeeded && ms < kGpuCompileLogThresholdMs) return;
  } else {
    ++s_late.builds;
    s_late.failures += !succeeded;
    s_late.total_ns += elapsed_ns;
    if (elapsed_ns > s_late.maximum_ns) s_late.maximum_ns = elapsed_ns;
    if (succeeded && ms < kGpuCompileLogThresholdMs) {
      static int trace = -1;
      if (trace < 0) trace = DiagnosticOptionEnabled("AR_GPU_COMPILE_TRACE");
      if (!trace) return;
    }
  }
  RuntimeLog_Printf("[gpu-compile] %s%s %s %.1f ms%s\n", s_log.preparing ? "" : "late ",
          stage, label, ms, succeeded ? "" : " FAILED");
}

unsigned GpuCompileLog_LateBuildCount(void) {
  return s_late.builds;
}

void GpuCompileLog_ReportLate(void) {
  if (!s_late.builds) return;
  RuntimeLog_Printf("[gpu-compile] late-summary builds=%u failed=%u total-ms=%.1f max-ms=%.1f\n",
          s_late.builds, s_late.failures, (double)s_late.total_ns / kNanosecondsPerMillisecond,
          (double)s_late.maximum_ns / kNanosecondsPerMillisecond);
  s_late.builds = s_late.failures = 0;
  s_late.total_ns = s_late.maximum_ns = 0;
}

void GpuCompileLog_BeginPreparation(GpuCompileLogObserver observer, void *context) {
  s_log = (GpuCompileLogState){
      .preparing = true, .observer = observer, .observer_context = context};
}

void GpuCompileLog_EndPreparation(void) {
  if (s_log.builds) {
    RuntimeLog_Printf("[gpu-compile] preparation: %u builds, %.1f ms total, %u failed, "
            "slowest %s %s %.1f ms\n",
            s_log.builds, (double)s_log.total_ns / kNanosecondsPerMillisecond,
            s_log.failures, s_log.slowest_stage, s_log.slowest_label,
            (double)s_log.slowest_ns / kNanosecondsPerMillisecond);
  }
  s_log.preparing = false;
}
