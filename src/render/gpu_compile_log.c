#include "render/gpu_compile_log.h"

#include <stdio.h>

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

static GpuCompileLogState s_log;

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
    if (succeeded && ms < kGpuCompileLogBootThresholdMs) return;
  }
  fprintf(stderr, "[gpu-compile] %s%s %s %.1f ms%s\n", s_log.preparing ? "" : "late ",
          stage, label, ms, succeeded ? "" : " FAILED");
}

void GpuCompileLog_BeginPreparation(GpuCompileLogObserver observer, void *context) {
  s_log = (GpuCompileLogState){
      .preparing = true, .observer = observer, .observer_context = context};
}

void GpuCompileLog_EndPreparation(void) {
  if (s_log.builds) {
    fprintf(stderr,
            "[gpu-compile] preparation: %u builds, %.1f ms total, %u failed, "
            "slowest %s %s %.1f ms\n",
            s_log.builds, (double)s_log.total_ns / kNanosecondsPerMillisecond,
            s_log.failures, s_log.slowest_stage, s_log.slowest_label,
            (double)s_log.slowest_ns / kNanosecondsPerMillisecond);
  }
  s_log.preparing = false;
}
