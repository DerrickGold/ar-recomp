#ifndef AR_RENDER_SDL_INTERNAL_H
#define AR_RENDER_SDL_INTERNAL_H
/* ArSdlRenderBackend internals: native renderer and shader access for
 * SDL-owned adapters and their focused tests only. Game-side code goes
 * through ArRenderDevice and opaque textures.
 * Phase: present (render owner thread). */

#include <SDL3/SDL.h>

#include "platform/sdl/render_sdl.h"
#include "platform/sdl/gpu_shader_blob.h"

typedef struct ArSdlRenderBackend {
  SDL_Renderer *renderer;
  SDL_GPUDevice *gpu_device;
  /* Default ordered interop: an offscreen SDL renderer records 2D work;
   * the adapter owns its default output and the one final window present. */
  SDL_Window *output_window;
  SDL_Texture *output_target;
  SDL_GPUCommandBuffer *present_commands; /* Prepared final blit; owner-thread only. */
  int output_width, output_height;
  SDL_GPUPresentMode output_present_mode;
  uint32_t applied_frames_in_flight;
  bool frames_in_flight_applied;
  bool reuse_texture_uploads; /* Ordered output only; staging owned by each texture. */
  bool owns_renderer;
  bool owns_context;
  bool owns_gpu_device;
  bool present_trace_enabled;
  ArSdlPresentTrace present_trace;
  struct ArSdlFragmentShaderEntry *fragment_shaders;
} ArSdlRenderBackend;

/* Native interop shared only by SDL-owned adapters and their focused tests.
 * Game-side presentation must use ArRenderDevice and opaque textures instead. */
SDL_Renderer *ArSdlRenderBackend_Renderer(const ArRenderDevice *device);
/* Borrow a device/renderer-lifetime shader. SDL's pipeline cache keys custom
 * fragment shaders by pointer; recycling that pointer during an effect reset
 * can select a different effect's old pipeline. Release only at backend
 * teardown, after all effect states have been reset, never in an effect. */
SDL_GPUShader *ArSdlRenderBackend_FragmentShader(ArRenderDevice *device,
    const GpuShaderBlobs *blobs, const char *label,
    Uint32 samplers, Uint32 uniform_buffers);
bool ArSdlRenderBackend_WindowOutputSize(const ArRenderDevice *device,
    int *width, int *height);
ArRenderTexture ArSdlRenderBackend_BorrowTexture(SDL_Texture *texture);
SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture);
/* Focused SDL adapter tests may bind an externally owned renderer. Production
 * boot uses CreateForWindow so native lifetime remains backend-owned. */
bool ArSdlRenderBackend_Bind(ArRenderDevice *device,
                             ArSdlRenderBackend *backend,
                             SDL_Renderer *renderer);

#endif /* AR_RENDER_SDL_INTERNAL_H */
