#ifndef AR_PLATFORM_SDL_RENDER_SDL_H
#define AR_PLATFORM_SDL_RENDER_SDL_H

#include <SDL3/SDL.h>

#include "render/render_device.h"

typedef struct ArSdlRenderBackend {
  SDL_Renderer *renderer;
} ArSdlRenderBackend;

/* Bind an existing application-owned SDL renderer. The backend never destroys
 * the renderer; it owns only textures created through its device operations. */
bool ArSdlRenderBackend_Bind(ArRenderDevice *device,
                             ArSdlRenderBackend *backend,
                             SDL_Renderer *renderer);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_H */
