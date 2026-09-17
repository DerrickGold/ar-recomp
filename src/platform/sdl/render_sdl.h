#ifndef AR_PLATFORM_SDL_RENDER_SDL_H
#define AR_PLATFORM_SDL_RENDER_SDL_H

#include "render/render_device.h"

typedef struct SDL_Window SDL_Window;

/* Create the production SDL GPU renderer and bind it to the portable device.
 * Platform builds select one backend implementation; it is created once at
 * video boot and owns the native renderer until shutdown-time Destroy.
 * `gpu_driver` names an SDL GPU driver ("direct3d12", "vulkan", "metal") or is
 * NULL for SDL's own order; a named driver that cannot create a working
 * output is logged and retried with SDL's order. */
bool ArSdlRenderBackend_CreateForWindow(ArRenderDevice *device,
                                        SDL_Window *window,
                                        const char *gpu_driver);
void ArSdlRenderBackend_Destroy(ArRenderDevice *device);
/* Whether this SDL build contains the named GPU driver. Compiled in is not the
 * same as usable on this machine; creation still falls back. */
bool ArSdlRenderBackend_HasGpuDriver(const char *name);
/* SDL driver name of the running device, or NULL before creation. */
const char *ArSdlRenderBackend_GpuDriver(const ArRenderDevice *device);

/* Platform display policy expressed without exporting the native renderer to
 * host callers. `active` reports the state observed after the request. */
bool ArSdlRenderBackend_SetVSync(ArRenderDevice *device, int requested,
                                 bool *active);
bool ArSdlRenderBackend_SetAllowedFramesInFlight(ArRenderDevice *device,
                                                 uint32_t requested,
                                                 bool *changed);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_H */
