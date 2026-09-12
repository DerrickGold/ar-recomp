#ifndef AR_SIM3D_PERFORMANCE_H
#define AR_SIM3D_PERFORMANCE_H

#include <stdbool.h>
#include <stdint.h>

/* Presentation-side Sim 3D profiling. The interactive pipeline collector uses
 * these lightweight stage/work hooks. AR_PERF or AR_SIM3D_PERF additionally
 * enables the legacy focused report, independently of the overlay. */
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
  kSim3DPerformance_WorldAnimation,
  kSim3DPerformance_WorldTransfer,
  kSim3DPerformance_WorldPrepare,
  kSim3DPerformance_WorldAtmosphere,
  kSim3DPerformance_WorldOcean,
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
/* Vertex stream traffic, separate from texture atlas uploads. */
void Sim3DPerformance_AddGeometryUpload(uint64_t bytes);
/* GPU-local geometry copies, not CPU uploads or GPU elapsed time. */
void Sim3DPerformance_AddGeometryCopy(uint64_t bytes, uint64_t calls);

/* Coarse path decisions, never per vertex. These are event counts, not
 * percentages or mutually exclusive frame outcomes. CPU projection is normal
 * work, not an authentic-view fallback. Rejected covers optional resource/API
 * rejection; only an explicit caller-side budget guard reports Limit. */
typedef enum Sim3DPerformancePath {
  kSim3DPath_CpuProject, kSim3DPath_CpuStage, kSim3DPath_GpuReuse,
  kSim3DPath_Publish, kSim3DPath_OptOut, kSim3DPath_Limit,
  kSim3DPath_Rejected, kSim3DPath_CpuReuse, kSim3DPath_Count,
} Sim3DPerformancePath;
void Sim3DPerformance_AddPath(Sim3DPerformancePath path);

/* One completed enhanced Sim 3D presentation. Reports and resets a rolling
 * one-second window when profiling is enabled. Means sum all scopes per
 * presentation; maxima measure one scope call (inclusive of nested work). */
void Sim3DPerformance_EndPresentation(void);

#endif /* AR_SIM3D_PERFORMANCE_H */
