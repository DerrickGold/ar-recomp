#ifndef AR_PRESENTATION_VIEW_H
#define AR_PRESENTATION_VIEW_H
/* PresentationView: decides which scene the compositor actually shows this
 * frame, and whether the native image appeared unexpectedly, by matching the
 * top-level compositor rather than the producer's requested view.
 * Phase: present (FrameSlot only).
 * Tests: tests/presentation_view_test.c */

#include "present/present.h"
#include "app/performance_metrics.h"
#include "present/render_comparison.h"

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
