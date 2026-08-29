#ifndef AR_DIORAMA_PERFORMANCE_H
#define AR_DIORAMA_PERFORMANCE_H

#include <stdbool.h>
#include <stdint.h>

#include "render/render_types.h"

typedef enum DioramaPerformanceStage {
  kDioramaPerformance_Total,
  kDioramaPerformance_Upload,
  kDioramaPerformance_FrameAnalysis,
  kDioramaPerformance_ProducerSetup,
  kDioramaPerformance_Scanout,
  kDioramaPerformance_ProducerFinish,
  kDioramaPerformance_HostPost,
  kDioramaPerformance_FrameSynthesis,
  kDioramaPerformance_Mesh,
  kDioramaPerformance_Supersample,
  kDioramaPerformance_Submit,
  kDioramaPerformance_Callback,
  kDioramaPerformanceStage_Count,
} DioramaPerformanceStage;

typedef struct DioramaPerformanceScope {
  DioramaPerformanceStage stage;
  uint64_t started_ns;
  bool active;
} DioramaPerformanceScope;

/* AR_ACTION_PERF=1 enables this profiler independently. AR_PERF=1 enables it
 * beside the existing whole-runtime and SIM profilers for recorded A/B runs. */
bool DioramaPerformance_Enabled(void);
DioramaPerformanceScope DioramaPerformance_Begin(
    DioramaPerformanceStage stage);
void DioramaPerformance_End(DioramaPerformanceScope scope);

void DioramaPerformance_AddPlaneSync(bool succeeded, bool uploaded,
                                     uint64_t uploaded_bytes);
/* Establish the compositor-local pixel rectangle used to clip projected
 * triangle coverage. Call once before the first draw of each presentation. */
void DioramaPerformance_SetViewport(int width, int height);
/* Switch the viewport used by draw-area accounting without adding another
 * presentation denominator. Internal render targets use this around their
 * submissions, then restore the ordinary compositor viewport. */
void DioramaPerformance_SetRasterViewport(int width, int height);
/* Tags subsequent compositor submissions with their source plane. -1 is
 * reserved for untextured enclosure geometry. */
void DioramaPerformance_SetPlane(int plane);
void DioramaPerformance_AddDraw(
    bool succeeded, const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count, ArRenderBlendMode blend);

/* Call exactly once after each enhanced Action presentation, after ending its
 * kDioramaPerformance_Total scope. Reports and clears one-second windows. */
void DioramaPerformance_PresentCompleted(void);

#endif /* AR_DIORAMA_PERFORMANCE_H */
