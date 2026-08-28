#ifndef AR_PLATFORM_SDL_RENDER_SDL_H
#define AR_PLATFORM_SDL_RENDER_SDL_H

#include <SDL3/SDL.h>

#include "render/render_device.h"

typedef struct ArSdlRenderBackend {
  SDL_Renderer *renderer;
  SDL_GPUDevice *gpu_device;
  uint32_t applied_frames_in_flight;
  bool frames_in_flight_applied;
  bool owns_renderer;
} ArSdlRenderBackend;

/* Create the production SDL GPU renderer and bind it to the portable device.
 * The backend owns the native renderer until Destroy. */
bool ArSdlRenderBackend_CreateForWindow(ArRenderDevice *device,
                                        ArSdlRenderBackend *backend,
                                        SDL_Window *window);
void ArSdlRenderBackend_Destroy(ArRenderDevice *device,
                                ArSdlRenderBackend *backend);

/* Platform display policy expressed without exporting the native renderer to
 * host callers. `active` reports the state observed after the request. */
bool ArSdlRenderBackend_SetVSync(ArRenderDevice *device, int requested,
                                 bool *active);
bool ArSdlRenderBackend_SetAllowedFramesInFlight(ArRenderDevice *device,
                                                 uint32_t requested,
                                                 bool *changed);

#endif /* AR_PLATFORM_SDL_RENDER_SDL_H */
