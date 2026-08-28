#ifndef AR_PLATFORM_SDL_RENDER_SDL_H
#define AR_PLATFORM_SDL_RENDER_SDL_H

#include "render/render_device.h"

typedef struct SDL_Window SDL_Window;

/* Create the production SDL GPU renderer and bind it to the portable device.
 * Platform builds select one backend implementation; it is created once at
 * video boot and owns the native renderer until shutdown-time Destroy. */
bool ArSdlRenderBackend_CreateForWindow(ArRenderDevice *device,
                                        SDL_Window *window);
void ArSdlRenderBackend_Destroy(ArRenderDevice *device);

/* Platform display policy expressed without exporting the native renderer to
 * host callers. `active` reports the state observed after the request. */
bool ArSdlRenderBackend_SetVSync(ArRenderDevice *device, int requested,
                                 bool *active);
bool ArSdlRenderBackend_SetAllowedFramesInFlight(ArRenderDevice *device,
                                                 uint32_t requested,
                                                 bool *changed);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_H */
