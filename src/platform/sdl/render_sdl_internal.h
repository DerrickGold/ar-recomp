#ifndef AR_PLATFORM_SDL_RENDER_SDL_INTERNAL_H
#define AR_PLATFORM_SDL_RENDER_SDL_INTERNAL_H

#include "platform/sdl/render_sdl.h"

/* Native interop shared only by SDL-owned adapters and their focused tests.
 * Game-side presentation must use ArRenderDevice and opaque textures instead. */
SDL_Renderer *ArSdlRenderBackend_Renderer(const ArRenderDevice *device);
ArRenderTexture ArSdlRenderBackend_BorrowTexture(SDL_Texture *texture);
SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_INTERNAL_H */
