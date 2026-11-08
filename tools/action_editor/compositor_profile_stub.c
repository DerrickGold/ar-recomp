/* Standalone preview/replay has no host profiler or clock. */
#include "diorama/diorama_performance.h"
#include "app/performance_metrics.h"
#ifdef __EMSCRIPTEN__
/* Native replay/benchmark targets link the shared metrics implementation. */
void PerformanceMetrics_Add(PerformanceCount counter, uint64_t value) {
  (void)counter;
  (void)value;
}
#endif
DioramaPerformanceScope DioramaPerformance_Begin(DioramaPerformanceStage stage) {
  return (DioramaPerformanceScope){.stage=stage};
}
void DioramaPerformance_End(DioramaPerformanceScope s) { (void)s; }
void DioramaPerformance_SetViewport(int w,int h) { (void)w; (void)h; }
void DioramaPerformance_SetRasterViewport(int w,int h) { (void)w; (void)h; }
void DioramaPerformance_SetPlane(int p) { (void)p; }
void DioramaPerformance_AddDraw(bool ok,const ArRenderVertex2D *v,int nv,
    const int32_t *i,int ni,ArRenderBlendMode b) {
  (void)ok;
  (void)v;
  (void)nv;
  (void)i;
  (void)ni;
  (void)b;
}

