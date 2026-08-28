#ifndef AR_PLATFORM_SDL_RENDER_SDL_INTERNAL_H
#define AR_PLATFORM_SDL_RENDER_SDL_INTERNAL_H

#include <SDL3/SDL.h>

#include "platform/sdl/render_sdl.h"

typedef struct ArSdlRenderBackend {
  SDL_Renderer *renderer;
  SDL_GPUDevice *gpu_device;
  uint32_t applied_frames_in_flight;
  bool frames_in_flight_applied;
  bool owns_renderer;
  bool owns_context;
} ArSdlRenderBackend;

/* Native interop shared only by SDL-owned adapters and their focused tests.
 * Game-side presentation must use ArRenderDevice and opaque textures instead. */
SDL_Renderer *ArSdlRenderBackend_Renderer(const ArRenderDevice *device);
ArRenderTexture ArSdlRenderBackend_BorrowTexture(SDL_Texture *texture);
SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture);
/* Focused SDL adapter tests may bind an externally owned renderer. Production
 * boot uses CreateForWindow so native lifetime remains backend-owned. */
bool ArSdlRenderBackend_Bind(ArRenderDevice *device,
                             ArSdlRenderBackend *backend,
                             SDL_Renderer *renderer);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_INTERNAL_H */
