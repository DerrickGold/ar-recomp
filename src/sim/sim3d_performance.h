#ifndef AR_SIM3D_PERFORMANCE_H
#define AR_SIM3D_PERFORMANCE_H

#include <stdbool.h>
#include <stdint.h>

/* Presentation-side Sim 3D profiling. The counters are dormant unless either
 * AR_PERF or AR_SIM3D_PERF is present in the environment. Keeping the switch
 * here gives focused terrain work a useful profiler without forcing the much
 * broader emulator instrumentation on. */
typedef enum Sim3DPerformanceStage {
  kSim3DPerformance_Upload,
  kSim3DPerformance_Backdrop,
  kSim3DPerformance_Underlay,
  kSim3DPerformance_Terrain,
  kSim3DPerformance_DepthVoxel,
  kSim3DPerformance_DepthCull,
  kSim3DPerformance_DepthMountain,
  kSim3DPerformance_DepthProject,
  kSim3DPerformance_DepthSubmit,
  kSim3DPerformance_Shadow,
  kSim3DPerformance_Billboard,
  kSim3DPerformance_Effects,
  kSim3DPerformance_Cloud,
  kSim3DPerformance_HostUi,
  kSim3DPerformanceStage_Count,
} Sim3DPerformanceStage;

typedef struct Sim3DPerformanceScope {
  uint64_t started_ns;
  Sim3DPerformanceStage stage;
  int previous_stage;
  bool active;
} Sim3DPerformanceScope;

bool Sim3DPerformance_Enabled(void);
Sim3DPerformanceScope Sim3DPerformance_Begin(Sim3DPerformanceStage stage);
void Sim3DPerformance_End(Sim3DPerformanceScope scope);

/* Attribute submitted work to the innermost active scope. A draw with no
 * vertex/index arrays can pass zero for both counts. */
void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices);
void Sim3DPerformance_AddUpload(uint64_t bytes);

/* One completed enhanced Sim 3D presentation. Reports and resets a rolling
 * one-second window when profiling is enabled. */
void Sim3DPerformance_EndPresentation(void);

#endif /* AR_SIM3D_PERFORMANCE_H */
