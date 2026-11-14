#include "render_output.h"

#include <string.h>

#include "presentation_layout.h"

static struct {
  ArRenderDevice *device;
  ArRenderTexture texture;
  ArRenderExtentI extent;
  ArRenderRectI destination;
  ArRenderTargetState parent;
  SceneAntiAliasing antialiasing;
} s_scene_scale;

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

  ArRenderRectI resolved;
  if (!ArRenderSceneScale_Viewport(device, &resolved))
    resolved = ArPresentationLayout_ResolveViewport(
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
    bool full_output_prepared, ArRenderOutputFrame *frame) {
  if (!frame || !ValidViewport(viewport, output_width, output_height))
    return false;

  const bool viewport_is_output =
      viewport.x == 0 && viewport.y == 0 &&
      viewport.w == output_width && viewport.h == output_height;
  bool prepared = full_output_prepared ||
      ArRenderDevice_SetClipRect(device, NULL);
  if (viewport_is_output) {
    prepared = prepared &&
        (full_output_prepared || ArRenderDevice_SetViewport(device, NULL)) &&
        ArRenderDevice_Clear(device, scene_color);
  } else {
    const ArRenderRectF scene_rectangle = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    prepared = prepared &&
        (full_output_prepared || ArRenderDevice_SetViewport(device, NULL)) &&
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
      margin_color, scene_color, true, frame);
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
      margin_color, scene_color, false, frame);
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

ArRenderTargetBeginResult ArRenderSceneScale_Begin(
    ArRenderDevice *device, ArRenderRectI destination, RenderResolution preset,
    bool allow_supersampling, SceneAntiAliasing antialiasing) {
  if (s_scene_scale.parent.valid) return kArRenderTargetBegin_StateLost;
  ArRenderExtentI extent =
      ArPresentationLayout_RenderExtent(destination, preset, allow_supersampling);
  const ArRenderCapabilities *caps = ArRenderDevice_Capabilities(device);
  if (!caps || extent.width <= 0 || extent.height <= 0)
    return kArRenderTargetBegin_Omitted;
  if (extent.height > destination.h &&
      ((caps->maximum_render_target_width > 0 &&
        extent.width > caps->maximum_render_target_width) ||
       (caps->maximum_render_target_height > 0 &&
        extent.height > caps->maximum_render_target_height)))
    extent = (ArRenderExtentI){destination.w, destination.h};
  if (!ArRenderCapabilities_Has(caps, kArRenderCapability_CustomShaders))
    antialiasing = kSceneAntiAliasing_Off;
  if (antialiasing == kSceneAntiAliasing_Off && extent.height == destination.h)
    return kArRenderTargetBegin_Omitted;
  if (s_scene_scale.device != device || !ArRenderTexture_IsValid(s_scene_scale.texture) ||
      extent.width != s_scene_scale.extent.width ||
      extent.height != s_scene_scale.extent.height) {
    ArRenderSceneScale_Reset(s_scene_scale.device);
    const ArRenderTextureDesc desc = {
      .width = extent.width, .height = extent.height,
      .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Target,
      .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Opaque,
    };
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_scene_scale.texture))
      return kArRenderTargetBegin_Omitted;
    s_scene_scale.extent = extent;
  }
  s_scene_scale.device = device;
  s_scene_scale.destination = destination;
  s_scene_scale.antialiasing = antialiasing;
  return ArRenderDevice_BeginTarget(device, s_scene_scale.texture, &s_scene_scale.parent);
}

bool ArRenderSceneScale_Viewport(ArRenderDevice *device, ArRenderRectI *viewport) {
  if (s_scene_scale.device != device || !s_scene_scale.parent.valid) return false;
  *viewport = (ArRenderRectI){0, 0, s_scene_scale.extent.width, s_scene_scale.extent.height};
  return true;
}

bool ArRenderSceneScale_Cancel(ArRenderDevice *device) {
  if (s_scene_scale.device != device || !s_scene_scale.parent.valid) return true;
  const bool restored = ArRenderDevice_EndTarget(device, &s_scene_scale.parent);
  s_scene_scale.parent = (ArRenderTargetState){0};
  return restored;
}

bool ArRenderSceneScale_Resolve(
    ArRenderDevice *device, ArRenderRectI *viewport, ArRenderFilter filter) {
  if (s_scene_scale.device != device || !s_scene_scale.parent.valid) return true;
  const ArRenderRectF source = {viewport->x, viewport->y, viewport->w, viewport->h};
  if (!ArRenderSceneScale_Cancel(device)) return false;
  *viewport = s_scene_scale.destination;
  int width, height;
  if (!ArRenderOutput_UseFull(device, &width, &height) ||
      !ValidViewport(*viewport, width, height))
    return false;
  // The opaque resolve covers the entire destination. Only clear when
  // letterboxing or pillarboxing leaves part of the parent uncovered.
  if ((viewport->x || viewport->y || viewport->w != width || viewport->h != height) &&
      !ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 1}))
    return false;
  const ArRenderRectF destination = {viewport->x, viewport->y, viewport->w, viewport->h};
  ArRenderDrawState state = {
    .flags = kArRenderDrawState_Filter | kArRenderDrawState_Blend,
    .filter = filter,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (source.w > destination.w || source.h > destination.h) {
    state.flags |= kArRenderDrawState_SceneResolve;
    state.scene_resolve = kArRenderSceneResolve_Downsample;
    state.filter = kArRenderFilter_Linear;
  } else if (s_scene_scale.antialiasing != kSceneAntiAliasing_Off) {
    state.flags |= kArRenderDrawState_SceneResolve;
    state.scene_resolve = s_scene_scale.antialiasing == kSceneAntiAliasing_Smaa
        ? kArRenderSceneResolve_Smaa : kArRenderSceneResolve_Fxaa;
    state.filter = kArRenderFilter_Linear;
  }
  return ArRenderDevice_DrawTextureWithState(
      device, s_scene_scale.texture, &source, &destination, &state);
}

bool ArRenderSceneScale_ResolveFrame(ArRenderOutputFrame *frame, ArRenderFilter filter) {
  if (!frame || !frame->active) return false;
  if (s_scene_scale.device != frame->device || !s_scene_scale.parent.valid) return true;
  ArRenderDevice *device = frame->device;
  ArRenderRectI viewport = frame->viewport;
  if (!ArRenderOutputFrame_Finish(frame) ||
      !ArRenderSceneScale_Resolve(device, &viewport, filter))
    return false;
  int width, height;
  if (!ArRenderDevice_GetOutputSize(device, &width, &height)) return false;
  const bool full = viewport.x == 0 && viewport.y == 0 &&
      viewport.w == width && viewport.h == height;
  if (!full && !ArRenderDevice_SetViewport(device, &viewport)) return false;
  *frame = (ArRenderOutputFrame){
    .device = device, .viewport = viewport, .output_width = width, .output_height = height,
    .viewport_is_output = full, .active = true,
  };
  return true;
}

void ArRenderSceneScale_Reset(ArRenderDevice *device) {
  if (device != s_scene_scale.device) return;
  ArRenderDevice_DestroyTexture(device, s_scene_scale.texture);
  memset(&s_scene_scale, 0, sizeof(s_scene_scale));
}
