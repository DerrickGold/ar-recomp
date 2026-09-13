#ifndef AR_PRESENTATION_VIEW_H
#define AR_PRESENTATION_VIEW_H

#include "frame_slot.h"
#include "performance_metrics.h"
#include "render_comparison.h"

typedef struct PresentationViewDecision {
  PerformanceScene scene;
  bool visible;
  bool unexpected_native;
} PresentationViewDecision;

/* Matches the top-level compositor, not just the producer's requested view.
 * Runtime render failures abort presentation and are counted separately. */
PresentationViewDecision PresentationView_Resolve(
    const FrameSlot *slot, RenderComparisonView comparison);

#endif
