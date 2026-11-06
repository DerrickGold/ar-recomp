#ifndef AR_DIORAMA_DEPTH_SINK_SDL_H
#define AR_DIORAMA_DEPTH_SINK_SDL_H

#include "diorama_depth_pass_sdl.h"
#include "render/render_device.h"

/* Present-thread owner for the compositor's optional world-depth sink.
 * Main and native color-math targets have independent depth lifetimes. Begin
 * each composite explicitly; its finish callback closes the scope even after
 * failure. Reset before the GPU device is destroyed. Queued geometry/material
 * parameters are copied; borrowed GPU resources are drained before mutation.
 * Consecutive draws batch until an SDL ordering boundary or bounded capacity.
 * Direct SDL/GPU callers must SubmitPending before recording outside work. */
typedef struct ArSdlDioramaDepthSink {
  SDL_GPUDevice *device;
  ArRenderDevice *owner;
  DioramaDepthSink sink;
  struct {
    SDL_Texture *target;
    ArGpuDioramaDepthPass pass;
    bool valid;
  } targets[4];
  DioramaDepthVertex *vertices;
  int32_t *indices;
  struct ArSdlDioramaDepthBatch *batch;
  unsigned capacity, index_capacity, vertex_count, index_count, count;
  bool failed, hybrid;
  struct { unsigned draws, submissions, largest_batch; } stats;
} ArSdlDioramaDepthSink;

const DioramaDepthSink *ArSdlDioramaDepthSink_Begin(ArSdlDioramaDepthSink *, ArRenderDevice *);
/* Production policy: opaque world depth plus bounded ordered fractional
 * coverage within each uninterrupted native/effect band. Ordinary SDL work
 * remains at its original boundary. The cutout entry point above is retained
 * for narrow adapter tests and explicit cutout consumers. */
const DioramaDepthSink *ArSdlDioramaDepthSink_BeginHybrid(ArSdlDioramaDepthSink *, ArRenderDevice *);
/* Prepare known render targets before Begin, e.g. during room/output setup.
 * An invalid texture handle means the backend output target. Does not change
 * the bound target, clear color/depth, or register a flush callback. Repeated
 * calls retain pipelines and storage; budgets may grow to the pass limits.
 * Unknown targets/capacity growth still have a lazy fallback during Submit. */
bool ArSdlDioramaDepthSink_PrepareTarget(ArSdlDioramaDepthSink *, ArRenderDevice *,
    ArRenderTexture target, unsigned vertices, unsigned indices);
bool ArSdlDioramaDepthSink_PrepareHybridTarget(ArSdlDioramaDepthSink *, ArRenderDevice *,
    ArRenderTexture target, unsigned vertices, unsigned indices);
void ArSdlDioramaDepthSink_Reset(ArSdlDioramaDepthSink *);

#endif
