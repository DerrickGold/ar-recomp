#ifndef AR_RENDER_OUTPUT_H
#define AR_RENDER_OUTPUT_H

#include "render_device.h"

/* Scoped physical-output frame used by presentation paths that render into an
 * aspect-fit viewport. Begin clears margins when present, selects
 * viewport-local coordinates, and clears that area to `scene_color`. Finish
 * restores the complete output viewport. */
typedef struct ArRenderOutputFrame {
  ArRenderDevice *device;
  ArRenderRectI viewport;
  int output_width;
  int output_height;
  bool viewport_is_output;
  bool active;
} ArRenderOutputFrame;

bool ArRenderOutputFrame_Begin(
    ArRenderDevice *device, ArRenderRectI viewport,
    ArRenderColorF margin_color, ArRenderColorF scene_color,
    ArRenderOutputFrame *frame);

/* Temporarily switch a viewport-local frame to full-output coordinates. These
 * are separate operations so a caller can omit a callback when entry fails,
 * yet treat a failed restoration after the callback as fatal. */
bool ArRenderOutputFrame_EnterFullOutput(ArRenderOutputFrame *frame);
bool ArRenderOutputFrame_RestoreViewport(ArRenderOutputFrame *frame);

/* Finish reports restoration failure. Abort is best-effort cleanup for an
 * already-failed scene and deliberately cannot hide the original error. */
bool ArRenderOutputFrame_Finish(ArRenderOutputFrame *frame);
void ArRenderOutputFrame_Abort(ArRenderOutputFrame *frame);

#endif /* AR_RENDER_OUTPUT_H */
