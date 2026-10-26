#include "presentation_layout.h"

#include <stdint.h>

/* SNES scene/capture dimensions fit in 16 bits. Bounding dimensions here also
 * keeps products with arbitrary positive host int sizes inside int64_t. */
static bool ValidCanvasLimits(ArPresentationCanvasLimits limits) {
  return limits.native_width > 0 && limits.native_width <= UINT16_MAX &&
      limits.native_height > 0 && limits.native_height <= UINT16_MAX &&
      limits.extra_columns >= 0 && limits.extra_columns <= UINT16_MAX &&
      limits.extra_rows >= 0 && limits.extra_rows <= UINT16_MAX;
}

static int64_t RequestedHorizontalMargin(
    int native_width, int native_height, int aspect_x, int aspect_y,
    bool crt_pixel_aspect) {
  const int64_t numerator =
      (int64_t)native_height * aspect_x * (crt_pixel_aspect ? 6 : 1);
  const int64_t denominator = (int64_t)aspect_y * (crt_pixel_aspect ? 7 : 1);
  const int64_t width = (numerator + denominator - 1) / denominator;
  return width > native_width ? (width - native_width + 1) / 2 : 0;
}

int ArPresentationLayout_ResolveHorizontalMargin(
    int native_width, int native_height, int aspect_x, int aspect_y,
    bool crt_pixel_aspect, int maximum_extra) {
  const ArPresentationCanvasLimits limits = {
    native_width, native_height, maximum_extra, 0,
  };
  if (!ValidCanvasLimits(limits) || aspect_x <= 0 || aspect_y <= 0) return 0;
  const int64_t requested = RequestedHorizontalMargin(
      native_width, native_height, aspect_x, aspect_y, crt_pixel_aspect);
  return requested > maximum_extra ? maximum_extra : (int)requested;
}

bool ArPresentationLayout_ResolveCanvas(
    int output_width, int output_height, bool crt_pixel_aspect,
    ArPresentationCanvasLimits limits, ArPresentationCanvas *canvas) {
  if (!canvas || output_width <= 0 || output_height <= 0 ||
      !ValidCanvasLimits(limits)) return false;
  const int64_t par_x = crt_pixel_aspect ? 7 : 1;
  const int64_t par_y = crt_pixel_aspect ? 6 : 1;
  const int64_t width = output_width, height = output_height;
  ArPresentationCanvas result = {0};
  if (width * limits.native_height * par_y >
      height * limits.native_width * par_x) {
    const int64_t requested = RequestedHorizontalMargin(
        limits.native_width, limits.native_height,
        output_width, output_height, crt_pixel_aspect);
    result.coverage_limited = requested > limits.extra_columns;
    result.extra_columns = result.coverage_limited
        ? limits.extra_columns : (int)requested;
  } else {
    const int64_t denominator = 2 * width * par_y;
    const int64_t requested =
        (height * limits.native_width * par_x -
         width * limits.native_height * par_y + denominator / 2) / denominator;
    result.coverage_limited = requested > limits.extra_rows;
    result.extra_rows = result.coverage_limited ? limits.extra_rows : (int)requested;
  }
  *canvas = result;
  return true;
}

ArRenderRectF ArPresentationLayout_CaptureDestination(
    ArRenderRectI viewport, int visible_height, int requested_top,
    int capture_height, int capture_top) {
  if (visible_height <= 0 || capture_height <= 0)
    return (ArRenderRectF){0};
  const float scale = (float)viewport.h / (float)visible_height;
  return (ArRenderRectF){
    (float)viewport.x,
    viewport.y + (requested_top - capture_top) * scale,
    (float)viewport.w, capture_height * scale,
  };
}

void ArPresentationLayout_ResolveLogicalSize(
    bool crt_pixel_aspect, int visible_width, int visible_height,
    int *logical_width, int *logical_height) {
  if (logical_width)
    *logical_width = visible_width * (crt_pixel_aspect ? 7 : 1);
  if (logical_height)
    *logical_height = visible_height * (crt_pixel_aspect ? 6 : 1);
}

ArRenderRectI ArPresentationLayout_ResolveViewport(
    int output_width, int output_height, bool stretch,
    bool crt_pixel_aspect, int visible_width, int visible_height) {
  ArRenderRectI viewport = {0, 0, output_width, output_height};
  if (stretch || output_width <= 0 || output_height <= 0 ||
      visible_width <= 0 || visible_height <= 0)
    return viewport;

  int logical_width, logical_height;
  ArPresentationLayout_ResolveLogicalSize(
      crt_pixel_aspect, visible_width, visible_height,
      &logical_width, &logical_height);
  if ((int64_t)output_width * logical_height >
      (int64_t)output_height * logical_width) {
    viewport.w = (int)((int64_t)output_height * logical_width /
                       logical_height);
    viewport.x = (output_width - viewport.w) / 2;
  } else {
    viewport.h = (int)((int64_t)output_width * logical_height /
                       logical_width);
    viewport.y = (output_height - viewport.h) / 2;
  }
  return viewport;
}
