#ifndef AR_PRESENTATION_LAYOUT_H
#define AR_PRESENTATION_LAYOUT_H

#include <stdbool.h>

#include "render_types.h"

/* World producers supply their own coverage limits. These are per-side
 * logical pixels, independent of the drawable, HUD skin, or hardware BG layer.
 * Increasing them requires matching capture capacity and valid world data. */
typedef struct ArPresentationCanvasLimits {
  int native_width, native_height;
  int extra_columns, extra_rows;
} ArPresentationCanvasLimits;

typedef struct ArPresentationCanvas {
  int extra_columns, extra_rows;
  bool coverage_limited;
} ArPresentationCanvas;

/* Expand one native axis to approach the drawable's aspect without cropping
 * the native view. The resulting canvas still goes through ResolveViewport;
 * a coverage limit leaves borders instead of stretching or inventing pixels.
 * Invalid/minimized sizes leave the caller's result intact. */
bool ArPresentationLayout_ResolveCanvas(
    int output_width, int output_height, bool crt_pixel_aspect,
    ArPresentationCanvasLimits limits, ArPresentationCanvas *canvas);

/* Explicit ratios expand horizontally only. Diorama capture padding is a
 * producer policy and must not change the width presented to the player. */
int ArPresentationLayout_ResolveHorizontalMargin(
    int native_width, int native_height, int aspect_x, int aspect_y,
    bool crt_pixel_aspect, int maximum_extra);

/* Logical content axes used by platform presentation APIs. The CRT pixel
 * aspect expresses one SNES pixel as 7:6; square-pixel output stays 1:1. */
void ArPresentationLayout_ResolveLogicalSize(
    bool crt_pixel_aspect, int visible_width, int visible_height,
    int *logical_width, int *logical_height);

/* Resolve the physical output rectangle occupied by game content. Stretch is
 * the only policy that may change the content aspect ratio; widescreen is
 * represented by visible_width and does not bypass aspect correction. */
ArRenderRectI ArPresentationLayout_ResolveViewport(
    int output_width, int output_height, bool stretch,
    bool crt_pixel_aspect, int visible_width, int visible_height);

/* Place only valid captured rows inside a stable requested canvas. Missing
 * rows remain the caller's clear colour, never stretched or sampled. */
ArRenderRectF ArPresentationLayout_CaptureDestination(
    ArRenderRectI viewport, int visible_height, int requested_top,
    int capture_height, int capture_top);

#endif /* AR_PRESENTATION_LAYOUT_H */
