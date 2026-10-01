/* Standalone preview/replay has no host profiler or clock. */
#include "diorama/diorama_performance.h"
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

