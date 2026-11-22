#ifndef AR_FRAME_TRACE_H
#define AR_FRAME_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/host_display.h"

typedef struct FrameTrace FrameTrace;
typedef enum FrameTraceType {
  kFrameTrace_Stream,
  kFrameTrace_Pacing,
  kFrameTrace_Synchronous,
  kFrameTrace_GpuPrepare,
} FrameTraceType;

typedef struct FrameStreamTraceRow {
  uint64_t start_ns, complete_ns, endpoint_ns, target_ns, interval_ns, epoch;
  double alpha;
  int tick;
  bool interpolation;
} FrameStreamTraceRow;

typedef struct FramePacingTraceRow {
  uint64_t loop_ns, prepare_ns, ready_ns, complete_ns;
  uint64_t epoch, source_ns, producer_start_ns, producer_complete_ns, input_ns;
  uint64_t pump_ns, input_events_ns, owner_poll_ns, producer_cpu_ns;
  uint64_t playout_target_ns, sample_ns, interval_ns;
  unsigned uploads, queue_before, queue_after;
  int tick, pacing_source;
  bool presented, interpolation;
  HostDisplayPresentTrace render;
} FramePacingTraceRow;

typedef struct FrameSyncTraceRow {
  uint64_t sample_ns, complete_ns, interval_ns, remainder_ns, source_ns, epoch;
  uint64_t target_ns, presentation_sample_ns;
  double alpha;
  int tick, tick_delta, pacing_source;
  unsigned map_group, map_number;
  bool produced, interpolation;
} FrameSyncTraceRow;

typedef struct FrameGpuPrepareTraceRow {
  uint64_t queued_ns, started_ns, submitted_ns, join_ns, joined_ns;
  unsigned job, mask;
  bool ok;
} FrameGpuPrepareTraceRow;

/* CSV is published to path on a clean drain. A dropped row or I/O failure
 * retains the .partial capture and reports the reason at shutdown. */
FrameTrace *FrameTrace_Open(const char *path, FrameTraceType type);
void FrameTrace_Record(FrameTrace *trace, const void *row, size_t size);
void FrameTrace_CloseAll(FrameTrace *const *traces, size_t count);

#endif
