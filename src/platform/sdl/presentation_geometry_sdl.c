#include "presentation_geometry_sdl.h"

#include <string.h>

#include "render/presentation_layout.h"

bool ArSdlPresentation_ApplyLogical(SDL_Renderer *renderer, bool stretch,
                                    bool crt_pixel_aspect,
                                    int visible_width, int visible_height) {
  if (!renderer || visible_width <= 0 || visible_height <= 0) return false;
  int logical_width, logical_height;
  ArPresentationLayout_ResolveLogicalSize(
      crt_pixel_aspect, visible_width, visible_height,
      &logical_width, &logical_height);
  return SDL_SetRenderLogicalPresentation(
      renderer, logical_width, logical_height,
      stretch ? SDL_LOGICAL_PRESENTATION_STRETCH
              : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

bool ArSdlPresentation_PushFullOutput(
    SDL_Renderer *renderer, ArSdlPresentationOutputState *state) {
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
    ArSdlPresentation_PopFullOutput(renderer, state);
    return false;
  }
  return true;
}

bool ArSdlPresentation_PopFullOutput(
    SDL_Renderer *renderer, const ArSdlPresentationOutputState *state) {
  if (!renderer || !state || !state->valid) return false;
  bool restored = SDL_SetRenderLogicalPresentation(
      renderer, state->logical_width, state->logical_height,
      state->logical_mode);
  restored = SDL_SetRenderViewport(
      renderer, state->viewport_set ? &state->viewport : NULL) && restored;
  restored = SDL_SetRenderClipRect(
      renderer, state->clip_enabled ? &state->clip : NULL) && restored;
  return restored;
}
