#ifndef AR_RENDER_SDL_H
#define AR_RENDER_SDL_H
/* ArSdlRenderBackend: creates the production SDL GPU renderer behind the
 * portable render device, choosing the GPU driver (D3D12, Vulkan, Metal) and
 * falling back to SDL's own order.
 * Phase: host (video boot and shutdown).
 * Tests: tests/render_sdl_present_test.c */

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

/* Submit preceding offscreen commands without presenting the window or
 * waiting for a fence/readback. Legacy adapters retain their Flush behavior. */
bool ArSdlRenderBackend_SubmitPending(const ArRenderDevice *device);

/* Split the final offscreen submission from window presentation. Both calls
 * run on the renderer/window owner thread. After a successful PreparePresent,
 * the caller may wait for its deadline, then MUST call PresentPrepared without
 * drawing, changing targets, or resizing in between. No swapchain image or
 * command buffer is held across that wait; no fence/readback is introduced.
 * Failed preparation must not be followed by PresentPrepared. */
bool ArSdlRenderBackend_PreparePresent(const ArRenderDevice *device);
bool ArSdlRenderBackend_PresentPrepared(const ArRenderDevice *device);

/* Opt-in owner-thread CPU scopes, not GPU timestamps or scanout timings. */
typedef struct ArSdlPresentTrace {
  uint64_t flush_ns, acquire_ns, submit_ns;
  /* Nested breakdowns: command setup + swapchain = acquire;
   * blit recording + queue submit/present API = submit. All are CPU scopes. */
  uint64_t command_ns, swapchain_ns, blit_ns, queue_submit_ns;
} ArSdlPresentTrace;
void ArSdlRenderBackend_EnablePresentTrace(const ArRenderDevice *device, bool enabled);
ArSdlPresentTrace ArSdlRenderBackend_LastPresentTrace(const ArRenderDevice *device);

/* Platform display policy expressed without exporting the native renderer to
 * host callers. `active` reports the state observed after the request. */
bool ArSdlRenderBackend_SetVSync(ArRenderDevice *device, int requested,
                                 bool *active);
bool ArSdlRenderBackend_SetAllowedFramesInFlight(ArRenderDevice *device,
                                                 uint32_t requested,
                                                 bool *changed);

#endif /* AR_RENDER_SDL_H */
