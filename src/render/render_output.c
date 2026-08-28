#include "render_output.h"

#include <stdint.h>
#include <string.h>

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
  bool prepared = false;
  if (viewport_is_output) {
    prepared = ArRenderDevice_SetViewport(device, NULL) &&
        ArRenderDevice_Clear(device, scene_color);
  } else {
    const ArRenderRectF scene_rectangle = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    prepared = ArRenderDevice_SetViewport(device, NULL) &&
        ArRenderDevice_Clear(device, margin_color) &&
        ArRenderDevice_SetViewport(device, &viewport) &&
        ArRenderDevice_DrawSolidRect(
            device, &scene_rectangle, scene_color,
            kArRenderBlendMode_Opaque);
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
  if (!ArRenderDevice_UseOutputCoordinates(device) ||
      !ArRenderDevice_GetOutputSize(
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
  if (aspect_width <= 0 || aspect_height <= 0 ||
      !ArRenderDevice_UseOutputCoordinates(device) ||
      !ArRenderDevice_GetOutputSize(
          device, &output_width, &output_height))
    return false;

  ArRenderRectI viewport = {0, 0, output_width, output_height};
  if (!stretch &&
      (int64_t)output_width * aspect_height !=
          (int64_t)output_height * aspect_width) {
    if ((int64_t)output_width * aspect_height >
        (int64_t)output_height * aspect_width) {
      viewport.w = (int)((int64_t)output_height * aspect_width /
                         aspect_height);
      viewport.x = (output_width - viewport.w) / 2;
    } else {
      viewport.h = (int)((int64_t)output_width * aspect_height /
                         aspect_width);
      viewport.y = (output_height - viewport.h) / 2;
    }
  }
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
