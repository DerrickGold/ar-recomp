#include "render_output.h"

#include <string.h>

#include "presentation_layout.h"

static void ResetFrame(ArRenderOutputFrame *frame) {
  if (frame) memset(frame, 0, sizeof(*frame));
}

static bool ValidViewport(ArRenderRectI viewport,
                          int output_width, int output_height) {
  return output_width > 0 && output_height > 0 &&
      viewport.x >= 0 && viewport.y >= 0 &&
      viewport.w > 0 && viewport.h > 0 &&
      viewport.x <= output_width - viewport.w &&
      viewport.y <= output_height - viewport.h;
}

static bool ColorsEqual(ArRenderColorF left, ArRenderColorF right) {
  return left.r == right.r && left.g == right.g &&
      left.b == right.b && left.a == right.a;
}

bool ArRenderOutput_ResolveAspectFit(
    ArRenderDevice *device, bool stretch,
    int aspect_width, int aspect_height,
    ArRenderRectI *viewport, int *output_width, int *output_height) {
  if (!viewport || aspect_width <= 0 || aspect_height <= 0)
    return false;
  int width = 0;
  int height = 0;
  if (!ArRenderDevice_UseOutputCoordinates(device) ||
      !ArRenderDevice_GetOutputSize(device, &width, &height) ||
      width <= 0 || height <= 0)
    return false;

  const ArRenderRectI resolved = ArPresentationLayout_ResolveViewport(
      width, height, stretch, false, aspect_width, aspect_height);
  *viewport = resolved;
  if (output_width) *output_width = width;
  if (output_height) *output_height = height;
  return true;
}

bool ArRenderOutput_UseFull(
    ArRenderDevice *device, int *output_width, int *output_height) {
  int width = 0;
  int height = 0;
  if (!ArRenderDevice_UseOutputCoordinates(device) ||
      !ArRenderDevice_SetViewport(device, NULL) ||
      !ArRenderDevice_SetClipRect(device, NULL) ||
      !ArRenderDevice_GetOutputSize(device, &width, &height))
    return false;
  if (output_width) *output_width = width;
  if (output_height) *output_height = height;
  return true;
}

static bool BeginResolvedFrame(
    ArRenderDevice *device, ArRenderRectI viewport,
    int output_width, int output_height,
    ArRenderColorF margin_color, ArRenderColorF scene_color,
    ArRenderOutputFrame *frame) {
  if (!frame || !ValidViewport(viewport, output_width, output_height))
    return false;

  const bool viewport_is_output =
      viewport.x == 0 && viewport.y == 0 &&
      viewport.w == output_width && viewport.h == output_height;
  bool prepared = ArRenderDevice_SetClipRect(device, NULL);
  if (viewport_is_output) {
    prepared = prepared && ArRenderDevice_SetViewport(device, NULL) &&
        ArRenderDevice_Clear(device, scene_color);
  } else {
    const ArRenderRectF scene_rectangle = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    prepared = prepared && ArRenderDevice_SetViewport(device, NULL) &&
        ArRenderDevice_Clear(device, margin_color) &&
        ArRenderDevice_SetViewport(device, &viewport) &&
        (ColorsEqual(margin_color, scene_color) ||
         ArRenderDevice_DrawSolidRect(
             device, &scene_rectangle, scene_color,
             kArRenderBlendMode_Opaque));
  }
  if (!prepared) {
    (void)ArRenderDevice_SetViewport(device, NULL);
    return false;
  }

  *frame = (ArRenderOutputFrame){
    .device = device,
    .viewport = viewport,
    .output_width = output_width,
    .output_height = output_height,
    .viewport_is_output = viewport_is_output,
    .active = true,
  };
  return true;
}

bool ArRenderOutputFrame_Begin(
    ArRenderDevice *device, ArRenderRectI viewport,
    ArRenderColorF margin_color, ArRenderColorF scene_color,
    ArRenderOutputFrame *frame) {
  if (!frame) return false;
  ResetFrame(frame);
  int output_width = 0;
  int output_height = 0;
  if (!ArRenderOutput_UseFull(
          device, &output_width, &output_height))
    return false;
  return BeginResolvedFrame(
      device, viewport, output_width, output_height,
      margin_color, scene_color, frame);
}

bool ArRenderOutputFrame_BeginAspectFit(
    ArRenderDevice *device, bool stretch,
    int aspect_width, int aspect_height,
    ArRenderColorF margin_color, ArRenderColorF scene_color,
    ArRenderOutputFrame *frame) {
  if (!frame) return false;
  ResetFrame(frame);
  int output_width = 0;
  int output_height = 0;
  ArRenderRectI viewport;
  if (!ArRenderOutput_ResolveAspectFit(
          device, stretch, aspect_width, aspect_height,
          &viewport, &output_width, &output_height))
    return false;
  return BeginResolvedFrame(
      device, viewport, output_width, output_height,
      margin_color, scene_color, frame);
}

bool ArRenderOutputFrame_EnterFullOutput(ArRenderOutputFrame *frame) {
  return frame && frame->active &&
      (frame->viewport_is_output ||
       ArRenderDevice_SetViewport(frame->device, NULL));
}

bool ArRenderOutputFrame_RestoreViewport(ArRenderOutputFrame *frame) {
  return frame && frame->active &&
      (frame->viewport_is_output ||
       ArRenderDevice_SetViewport(frame->device, &frame->viewport));
}

bool ArRenderOutputFrame_Finish(ArRenderOutputFrame *frame) {
  if (!frame || !frame->active) return false;
  const bool restored = frame->viewport_is_output ||
      ArRenderDevice_SetViewport(frame->device, NULL);
  ResetFrame(frame);
  return restored;
}

void ArRenderOutputFrame_Abort(ArRenderOutputFrame *frame) {
  if (!frame || !frame->active) return;
  if (!frame->viewport_is_output)
    (void)ArRenderDevice_SetViewport(frame->device, NULL);
  ResetFrame(frame);
}
