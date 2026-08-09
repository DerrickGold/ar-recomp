#include "presentation_geometry.h"

#include <stdint.h>

static void LogicalSize(bool crt_pixel_aspect, int visible_width,
                        int snes_height, int *width, int *height) {
  *width = visible_width * (crt_pixel_aspect ? 7 : 1);
  *height = snes_height * (crt_pixel_aspect ? 6 : 1);
}

bool PresentationGeometry_ApplyLogical(SDL_Renderer *renderer, bool stretch,
                                       bool crt_pixel_aspect,
                                       int visible_width, int snes_height) {
  if (!renderer || visible_width <= 0 || snes_height <= 0) return false;
  int logical_width, logical_height;
  LogicalSize(crt_pixel_aspect, visible_width, snes_height,
              &logical_width, &logical_height);
  return SDL_SetRenderLogicalPresentation(
      renderer, logical_width, logical_height,
      stretch ? SDL_LOGICAL_PRESENTATION_STRETCH
              : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

SDL_Rect PresentationGeometry_CalculateViewport(int output_width,
                                                int output_height,
                                                bool stretch,
                                                bool crt_pixel_aspect,
                                                int visible_width,
                                                int snes_height) {
  SDL_Rect viewport = { 0, 0, output_width, output_height };
  if (stretch || output_width <= 0 || output_height <= 0 ||
      visible_width <= 0 || snes_height <= 0)
    return viewport;

  int logical_width, logical_height;
  LogicalSize(crt_pixel_aspect, visible_width, snes_height,
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
