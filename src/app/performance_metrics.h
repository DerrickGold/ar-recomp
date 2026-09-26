#ifndef AR_PERFORMANCE_METRICS_H
#define AR_PERFORMANCE_METRICS_H

#include <stdbool.h>
#include <stdint.h>
#include "host/host_clock.h"

/* Application diagnostics, never simulation inputs. Stage durations are CPU
 * wall time; nested stages overlap and worker durations can run in parallel. */
enum { kPerformance_SimCount = 19, kPerformance_ActionCount = 13 };
typedef enum PerformanceStage {
  kPerformance_Events, kPerformance_Emulation, kPerformance_Ppu,
  kPerformance_WorldMap, kPerformance_Metadata, kPerformance_TownCanvas,
  kPerformance_Capture, kPerformance_Upload, kPerformance_Presentation,
  kPerformance_PostProcess, kPerformance_HostUi, kPerformance_SettingsUi,
  kPerformance_Overlay, kPerformance_Swap, kPerformance_Pacing,
  kPerformance_Housekeeping,
  kPerformance_PpuSetup, kPerformance_PpuScanout, kPerformance_PpuFinish,
  kPerformance_SimColor, kPerformance_SimHud, kPerformance_SimDiagnostics,
  kPerformance_CanvasRaster, kPerformance_CanvasEnhance,
  kPerformance_WorkOwner, kPerformance_WorkHelpers, kPerformance_WorkJoin,
  kPerformance_SimFirst,
  kPerformance_ActionFirst = kPerformance_SimFirst + kPerformance_SimCount,
  kPerformance_SettingsWrite = kPerformance_ActionFirst + kPerformance_ActionCount,
  kPerformance_SaveWrite, kPerformance_MusicStart,
  kPerformance_TerrainPrepare, kPerformance_TerrainSamples, kPerformance_GlobeBuild,
  kPerformanceStage_Count,
} PerformanceStage;

typedef enum PerformanceCount {
  kPerformanceCount_Ticks, kPerformanceCount_Represents,
  kPerformanceCount_Draws, kPerformanceCount_Vertices,
  kPerformanceCount_UploadBytes, kPerformanceCount_DepthUploadBytes,
  kPerformanceCount_WorkJobs, kPerformanceCount_HelperJobs,
  kPerformanceCount_Fallbacks, kPerformanceCount_FailedPresents,
  kPerformanceCount_CpuProject, kPerformanceCount_CpuStage,
  kPerformanceCount_GpuReuse, kPerformanceCount_GeometryPublish,
  kPerformanceCount_GeometryOptOut, kPerformanceCount_GeometryLimit,
  kPerformanceCount_GeometryRejected,
  /* Backend texture updates, including direct GPU atlas uploads. Calls and
   * payload bytes count requests issued, including failed attempts; bytes
   * exclude row padding and hidden driver copies. Geometry is separate. */
  kPerformanceCount_UploadCalls,      /* texture-update/copy calls issued */
  kPerformanceCount_UploadSkipped,    /* uploads avoided by change detection */
  kPerformanceCount_ScanBytes,        /* requested comparison bytes, both inputs;
                                      * includes edge scans, not measured DRAM reads */
  kPerformanceCount_MirrorReallocs,   /* upload-mirror storage reallocations */
  kPerformanceCount_DepthCopyBytes, kPerformanceCount_DepthCopyCalls,
  kPerformanceCount_AtlasReuse, kPerformanceCount_AtlasCopyBytes,
  kPerformanceCount_AtlasCopyCalls,
  kPerformanceCount_Count,
} PerformanceCount;

typedef enum PerformanceScene {
  kPerformanceScene_Native, kPerformanceScene_Action,
  kPerformanceScene_Town, kPerformanceScene_World,
  kPerformanceScene_Palace, kPerformanceScene_ActionFlat, kPerformanceScene_Count,
} PerformanceScene;

typedef struct PerformanceContext {
  PerformanceScene scene;
  int host_mode; /* 0 game, 1 paused, 2 settings, 3 comparison */
  int map_group, map_number, width, height, refresh_mode, limit_fps;
  bool vsync;
} PerformanceContext;

typedef struct PerformanceValue {
  double mean_ms, maximum_ms;
  uint64_t calls;
} PerformanceValue;

typedef struct PerformanceSnapshot {
  PerformanceContext context;
  uint64_t revision, presents;
  double seconds, fps, frame_mean_ms, frame_p95_ms, frame_max_ms;
  PerformanceValue stages[kPerformanceStage_Count];
  double counts[kPerformanceCount_Count]; /* per completed present */
  bool ready;
} PerformanceSnapshot;

typedef struct PerformanceScope {
  uint64_t started_ns;
  uint32_t epoch;
  PerformanceStage stage;
} PerformanceScope;

/* Configure/context/completion/snapshot are main-owner-thread operations,
 * after outstanding fork/join work has completed (never concurrently with Record).
 * Record/Add are thread safe and allocation-free. An epoch rejects stale
 * scopes across toggles. Context changes clear completed history, preserving
 * pending work for the new scene's first frame. Failed presents do not count
 * as completions. Snapshot is a value copy; no renderer or live game access. */
void PerformanceMetrics_Configure(bool enabled, bool log_reports);
bool PerformanceMetrics_Enabled(void);
uint32_t PerformanceMetrics_Epoch(void);
void PerformanceMetrics_Record(uint32_t epoch, PerformanceStage stage, uint64_t elapsed_ns);
/* Merge owner-collected worker completions without treating their sum as one
 * long call. Worker time may overlap the frame or be included in an owner's
 * synchronous wait; it is not additional serial frame cost. */
void PerformanceMetrics_RecordBatch(uint32_t epoch, PerformanceStage stage,
    uint64_t elapsed_ns, uint64_t maximum_ns, uint64_t calls);
void PerformanceMetrics_Add(PerformanceCount counter, uint64_t value);
/* Called once at the backend upload boundary, never by its caller. */
void PerformanceMetrics_AddTextureUpload(uint64_t calls, uint64_t bytes);
void PerformanceMetrics_SetContext(const PerformanceContext *context);
void PerformanceMetrics_PresentCompleted(uint64_t now_ns);
void PerformanceMetrics_Snapshot(PerformanceSnapshot *snapshot);
const char *PerformanceMetrics_StageName(PerformanceStage stage);
const char *PerformanceMetrics_SceneName(PerformanceScene scene);

static inline PerformanceScope PerformanceMetrics_Begin(PerformanceStage stage) {
  PerformanceScope scope = {.epoch = PerformanceMetrics_Epoch(), .stage = stage};
  if (scope.epoch) scope.started_ns = HostClock_Nanoseconds();
  return scope;
}
static inline void PerformanceMetrics_End(PerformanceScope scope) {
  if (!scope.epoch) return;
  const uint64_t now = HostClock_Nanoseconds();
  if (now >= scope.started_ns)
    PerformanceMetrics_Record(scope.epoch, scope.stage, now - scope.started_ns);
}

#endif
