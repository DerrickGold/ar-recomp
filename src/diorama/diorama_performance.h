#ifndef AR_DIORAMA_PERFORMANCE_H
#define AR_DIORAMA_PERFORMANCE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum DioramaPerformanceStage {
  kDioramaPerformance_Total,
  kDioramaPerformance_Upload,
  kDioramaPerformance_ProducerSetup,
  kDioramaPerformance_Scanout,
  kDioramaPerformance_ProducerFinish,
  kDioramaPerformance_HostPost,
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
void DioramaPerformance_AddDraw(bool succeeded, int vertices, int indices);

/* Call exactly once after each enhanced Action presentation, after ending its
 * kDioramaPerformance_Total scope. Reports and clears one-second windows. */
void DioramaPerformance_PresentCompleted(void);

#endif /* AR_DIORAMA_PERFORMANCE_H */
