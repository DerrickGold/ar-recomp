#ifndef AR_PERFORMANCE_OVERLAY_H
#define AR_PERFORMANCE_OVERLAY_H

#include "performance_metrics.h"
#include "render/render_device.h"

enum { kPerformanceOverlay_MaxLines = 72, kPerformanceOverlay_LineBytes = 96 };
typedef struct PerformanceOverlayLine {
  int x, y;
  char text[kPerformanceOverlay_LineBytes];
} PerformanceOverlayLine;
typedef struct PerformanceOverlayModel {
  ArRenderRectI panel;
  int scale, line_count;
  PerformanceOverlayLine lines[kPerformanceOverlay_MaxLines];
} PerformanceOverlayModel;

/* Pure, bounded layout for physical-output coordinates. Detailed needs two
 * readable columns; small outputs and the host settings menu use a compact
 * summary, while the run log always retains every measured stage. */
void PerformanceOverlay_Build(const PerformanceSnapshot *snapshot, int level,
    ArRenderExtentI output, PerformanceOverlayModel *model);
/* Terminal host pass, after CRT. Does not inspect settings or live game state. */
/* False only if a failed target restoration lost caller render state. */
bool PerformanceOverlay_Render(ArRenderDevice *device,
    const PerformanceSnapshot *snapshot, int level, ArRenderExtentI output);
void PerformanceOverlay_Reset(ArRenderDevice *device);

#endif
