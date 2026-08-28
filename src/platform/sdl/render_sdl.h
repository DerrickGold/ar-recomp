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

/* Temporary migration bridge for SDL textures not yet owned by the portable
 * render device. It is intentionally confined to the SDL platform header and
 * disappears as each presentation subsystem moves behind the device. */
ArRenderTexture ArSdlRenderBackend_BorrowTexture(SDL_Texture *texture);
SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_H */
