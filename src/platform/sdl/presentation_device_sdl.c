#include "presentation_device_sdl.h"

#include <SDL3/SDL.h>

#include "platform/sdl/render_sdl_internal.h"
#include "render/presentation_layout.h"

bool ArSdlPresentationDevice_ApplyLogical(
    ArRenderDevice *device, bool stretch, bool crt_pixel_aspect,
    int visible_width, int visible_height) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || visible_width <= 0 || visible_height <= 0) return false;
  int logical_width = 0;
  int logical_height = 0;
  ArPresentationLayout_ResolveLogicalSize(
      crt_pixel_aspect, visible_width, visible_height,
      &logical_width, &logical_height);
  return SDL_SetRenderLogicalPresentation(
      renderer, logical_width, logical_height,
      stretch ? SDL_LOGICAL_PRESENTATION_STRETCH
              : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}
