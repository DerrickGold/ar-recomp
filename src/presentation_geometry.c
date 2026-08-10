#include "presentation_geometry.h"

#include <stdint.h>
#include <string.h>

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

bool PresentationGeometry_PushFullOutput(
    SDL_Renderer *renderer, PresentationOutputState *state) {
  if (!renderer || !state) return false;
  memset(state, 0, sizeof(*state));

  if (!SDL_GetRenderLogicalPresentation(
          renderer, &state->logical_width, &state->logical_height,
          &state->logical_mode) ||
      !SDL_GetRenderViewport(renderer, &state->viewport))
    return false;
  state->viewport_set = SDL_RenderViewportSet(renderer);
  state->clip_enabled = SDL_RenderClipEnabled(renderer);
  if (state->clip_enabled &&
      !SDL_GetRenderClipRect(renderer, &state->clip))
    return false;
  state->valid = true;

  if (!SDL_SetRenderLogicalPresentation(
          renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) ||
      !SDL_SetRenderViewport(renderer, NULL) ||
      !SDL_SetRenderClipRect(renderer, NULL)) {
    PresentationGeometry_PopFullOutput(renderer, state);
    return false;
  }
  return true;
}

void PresentationGeometry_PopFullOutput(
    SDL_Renderer *renderer, const PresentationOutputState *state) {
  if (!renderer || !state || !state->valid) return;
  SDL_SetRenderLogicalPresentation(
      renderer, state->logical_width, state->logical_height,
      state->logical_mode);
  SDL_SetRenderViewport(
      renderer, state->viewport_set ? &state->viewport : NULL);
  SDL_SetRenderClipRect(
      renderer, state->clip_enabled ? &state->clip : NULL);
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
