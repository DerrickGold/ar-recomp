#include "presentation_layout.h"

#include <stdint.h>

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
