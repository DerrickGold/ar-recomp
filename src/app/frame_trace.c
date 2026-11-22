#include "app/frame_trace.h"

#include <SDL3/SDL_timer.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "app/runtime_log.h"
#include "platform/sdl/diagnostic_writer_sdl.h"
#include "snesrecomp/support/utf8_fs.h"

struct FrameTrace {
  DiagnosticWriter *writer;
  DiagnosticWrite write;
  char path[];
};

typedef struct TracePublication {
  char *partial;
  char path[];
} TracePublication;

static bool PublishTrace(void *context, bool complete) {
  TracePublication *publication = context;
  const bool ok = !complete || sr_replace_file(publication->partial, publication->path);
  free(publication);
  return ok;
}

static void WriteStream(FILE *file, const void *data, size_t size) {
  (void)size;
  const FrameStreamTraceRow *r = data;
  fprintf(file, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.8f,%" PRIu64 ",%d,%" PRIu64 ",%d,%" PRIu64 "\n",
          r->start_ns, r->complete_ns, r->endpoint_ns, r->alpha, r->target_ns, r->tick,
          r->interval_ns, r->interpolation, r->epoch);
}

static void WritePacing(FILE *file, const void *data, size_t size) {
  (void)size;
  const FramePacingTraceRow *r = data;
  const HostDisplayPresentTrace *s = &r->render;
  fprintf(file,
          "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
          ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64
          ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
          ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d"
          ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
          r->loop_ns, r->prepare_ns, r->ready_ns, s->draw_start_ns, r->complete_ns,
          s->deadline_ns, r->ready_ns - r->prepare_ns, r->uploads, s->draw_ns, s->swap_ns,
          s->vector_wait_ns, r->queue_before, r->queue_after, r->presented, r->tick,
          r->epoch, r->source_ns, r->producer_start_ns, r->producer_complete_ns, r->input_ns,
          r->pump_ns, r->input_events_ns, r->owner_poll_ns, s->submit_start_ns, s->backend_flush_ns,
          s->backend_acquire_ns, s->backend_submit_ns, r->producer_cpu_ns, r->playout_target_ns,
          r->sample_ns, r->interval_ns, r->interpolation, r->pacing_source, s->draw_cpu_ns,
          s->backend_command_ns, s->backend_swapchain_ns, s->backend_blit_ns,
          s->backend_queue_submit_ns);
}

static void WriteSync(FILE *file, const void *data, size_t size) {
  (void)size;
  const FrameSyncTraceRow *r = data;
  fprintf(file, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.8f,%d,%d,%d,%" PRIu64
          ",%" PRIu64 ",%d,%d,%" PRIu64 ",%u,%u,%" PRIu64 "\n",
          r->sample_ns, r->complete_ns, r->interval_ns, r->remainder_ns, r->alpha, r->tick,
          r->produced, r->interpolation, r->source_ns, r->epoch, r->tick_delta,
          r->pacing_source, r->target_ns, r->map_group, r->map_number, r->presentation_sample_ns);
}

static void WriteGpuPrepare(FILE *file, const void *data, size_t size) {
  (void)size;
  const FrameGpuPrepareTraceRow *r = data;
  fprintf(file, "%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%d\n",
          r->job, r->queued_ns, r->started_ns, r->submitted_ns, r->join_ns, r->joined_ns,
          r->mask, r->ok);
}

static const struct {
  const char *header;
  DiagnosticWrite write;
  size_t row_size;
} kFormats[] = {
  [kFrameTrace_Stream] = {
    "start_ns,complete_ns,endpoint_ns,alpha,target_ns,tick,interval_ns,interpolation,epoch\n",
    WriteStream, sizeof(FrameStreamTraceRow),
  },
  [kFrameTrace_Pacing] = {
    "loop_ns,prepare_ns,ready_ns,draw_ns,complete_ns,deadline_ns,upload_ns,uploads,draw_"
    "work_ns,swap_ns,vector_wait_ns,queue_before,queue_after,presented,tick,epoch,source_"
    "ns,producer_start_ns,producer_complete_ns,input_ns,pump_ns,input_events_ns,owner_poll_"
    "ns,submit_start_ns,backend_flush_ns,backend_acquire_ns,backend_submit_ns,producer_cpu_"
    "ns,playout_target_ns,sample_ns,interval_ns,interpolation,pacing_source,draw_cpu_ns,"
    "backend_command_ns,backend_swapchain_ns,backend_blit_ns,backend_queue_submit_ns\n",
    WritePacing, sizeof(FramePacingTraceRow),
  },
  [kFrameTrace_Synchronous] = {
    "sample_ns,complete_ns,interval_ns,remainder_ns,alpha,tick,produced,interpolation,source_ns,"
    "epoch,tick_delta,pacing_source,target_ns,map_group,map_number,presentation_sample_ns\n",
    WriteSync, sizeof(FrameSyncTraceRow),
  },
  [kFrameTrace_GpuPrepare] = {
    "job,queued_ns,started_ns,submitted_ns,join_ns,joined_ns,mask,ok\n",
    WriteGpuPrepare, sizeof(FrameGpuPrepareTraceRow),
  },
};

FrameTrace *FrameTrace_Open(const char *path, FrameTraceType type) {
  if (!path || !*path || (unsigned)type >= sizeof(kFormats) / sizeof(kFormats[0])) return NULL;
  const size_t size = strlen(path) + 1;
  FrameTrace *trace = malloc(sizeof(*trace) + size);
  TracePublication *publication = malloc(sizeof(*publication) + 2 * size + sizeof(".partial") - 1);
  if (trace && publication) {
    memcpy(trace->path, path, size);
    memcpy(publication->path, path, size);
    publication->partial = publication->path + size;
    snprintf(publication->partial, size + sizeof(".partial") - 1, "%s.partial", path);
    FILE *file = sr_fopen(publication->partial, "wbx");
    if (file) {
      if (fputs(kFormats[type].header, file) >= 0) {
        trace->write = kFormats[type].write;
        trace->writer = DiagnosticWriter_Create(file, 2048, kFormats[type].row_size,
            (DiagnosticWriterOptions){
              .close_output = true, .finalize = PublishTrace, .context = publication,
            });
        if (trace->writer) return trace;
      }
      fclose(file);
      sr_remove(publication->partial);
    }
  }
  RuntimeLog_Printf("[frame-trace] could not start writer for %s\n", path);
  free(publication);
  free(trace);
  return NULL;
}

void FrameTrace_Record(FrameTrace *trace, const void *row, size_t size) {
  (void)DiagnosticWriter_Submit(trace->writer, trace->write, row, size);
}

void FrameTrace_CloseAll(FrameTrace *const *traces, size_t count) {
  const Uint64 deadline = SDL_GetTicks() + 250;
  for (size_t i = 0; i < count; ++i)
    if (traces[i]) DiagnosticWriter_RequestStop(traces[i]->writer);
  for (size_t i = 0; i < count; ++i) {
    FrameTrace *trace = traces[i];
    if (!trace) continue;
    const Uint64 now = SDL_GetTicks();
    const DiagnosticWriterResult result =
        DiagnosticWriter_Finish(trace->writer, now < deadline ? (int)(deadline - now) : 0);
    if (result.dropped || result.failed || result.timed_out)
      RuntimeLog_Printf("[frame-trace] incomplete %s.partial dropped=%zu failed=%d timed-out=%d\n",
                        trace->path, result.dropped, result.failed, result.timed_out);
    free(trace);
  }
}
