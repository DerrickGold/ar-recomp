#ifndef AR_RENDER_SDL_INTEROP_H
#define AR_RENDER_SDL_INTEROP_H
/* Native SDL interop for GPU adapters, developer tools and readback tests.
 * Portable game presentation uses ArRenderDevice and opaque textures.
 * Phase: present (render owner thread). */

#include "platform/sdl/render_sdl.h"

typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_Texture SDL_Texture;

/* Borrowed backend handles, valid until backend teardown. Do not destroy them.
 * Returns NULL for an unbound/non-SDL device; GpuDevice also returns NULL for
 * an SDL renderer without a GPU device, such as a software test renderer. */
SDL_Renderer *ArSdlRenderBackend_Renderer(const ArRenderDevice *device);
SDL_GPUDevice *ArSdlRenderBackend_GpuDevice(const ArRenderDevice *device);
/* Borrow the native handle of an SDL-backed texture. Its owner retains the
 * lifetime; destroying the opaque texture also invalidates this handle. */
SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture);

#endif /* AR_RENDER_SDL_INTEROP_H */
