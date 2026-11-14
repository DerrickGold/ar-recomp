#ifndef AR_RENDER_SDL_INTERNAL_H
#define AR_RENDER_SDL_INTERNAL_H
/* ArSdlRenderBackend internals: native renderer and shader access for
 * SDL-owned adapters and their focused tests only. Game-side code goes
 * through ArRenderDevice and opaque textures.
 * Phase: present (render owner thread). */

#include <SDL3/SDL.h>

#include "platform/sdl/render_sdl_interop.h"
#include "platform/sdl/gpu_shader_blob.h"
#include "platform/sdl/smaa_sdl.h"

typedef struct ArSdlRenderBackend {
  SDL_Renderer *renderer;
  SDL_GPUDevice *gpu_device;
  /* Default ordered interop: an offscreen SDL renderer records 2D work;
   * the adapter owns its default output and the one final window present. */
  SDL_Window *output_window;
  SDL_Texture *output_target;
  int output_width, output_height;
  SDL_GPUPresentMode output_present_mode;
  uint32_t applied_frames_in_flight;
  bool frames_in_flight_applied;
  bool metal_drawable_policy, metal_drawable_warning;
  unsigned metal_drawable_override, reported_metal_drawables;
  bool reuse_texture_uploads; /* Ordered output only; staging owned by each texture. */
  /* Upload-only owner scope; commands are acquired lazily on the first update. */
  SDL_GPUCommandBuffer *upload_commands;
  SDL_GPUCopyPass *upload_copy;
  bool upload_open, upload_failed;
  bool owns_renderer;
  bool owns_context;
  bool owns_gpu_device;
  bool present_trace_enabled;
  ArSdlPresentTrace present_trace;
  struct ArSdlFragmentShaderEntry *fragment_shaders;
  SDL_GPURenderState *scene_resolve;
  bool scene_resolve_attempted;
  ArSdlSmaa smaa;
  /* One scoped external GPU batch. Its owner snapshots draw state and drains
   * before ordinary draws, resource mutation, target changes or presentation.
   * A failed drain must abandon its queued work so cleanup can still proceed. */
  bool (*external_flush)(void *userdata);
  void *external_userdata;
  bool external_flushing;
  bool external_sdl_idle; /* Callback-local certificate, cleared after every drain. */
} ArSdlRenderBackend;

/* Borrow a device/renderer-lifetime shader. SDL's pipeline cache keys custom
 * fragment shaders by pointer; recycling that pointer during an effect reset
 * can select a different effect's old pipeline. Release only at backend
 * teardown, after all effect states have been reset, never in an effect. */
SDL_GPUShader *ArSdlRenderBackend_FragmentShader(ArRenderDevice *device,
    const GpuShaderBlobs *blobs, const char *label,
    Uint32 samplers, Uint32 uniform_buffers);
SDL_GPUShader *ArSdlRenderBackend_GetFragmentShader(ArSdlRenderBackend *backend,
    const GpuShaderBlobs *blobs, const char *label,
    Uint32 samplers, Uint32 uniform_buffers);
bool ArSdlRenderBackend_WindowOutputSize(const ArRenderDevice *device,
    int *width, int *height);
ArRenderTexture ArSdlRenderBackend_BorrowTexture(SDL_Texture *texture);
/* Register or remove an owned scope; NULL flush removes this userdata's scope.
 * Removal drains first, and detaches even if that drain fails. The callback
 * must not submit through the portable renderer or change the registration. */
bool ArSdlRenderBackend_SetExternalFlush(ArRenderDevice *, bool (*flush)(void *), void *userdata);
/* Optional notification from the currently draining owner after a successful
 * nonempty native submission. The owner must have submitted all prior SDL
 * drawing before collecting native work, with no intervening SDL drawing.
 * Only this same drain may omit SDL's redundant offscreen RenderPresent;
 * SDL_FlushRenderer still runs. Direct calls, wrong owners and legacy adapters
 * cannot certify anything and simply return false. */
bool ArSdlRenderBackend_CertifyExternalSdlIdle(const ArRenderDevice *, void *userdata);
/* Focused SDL adapter tests may bind an externally owned renderer. Production
 * boot uses CreateForWindow so native lifetime remains backend-owned. */
bool ArSdlRenderBackend_Bind(ArRenderDevice *device,
                             ArSdlRenderBackend *backend,
                             SDL_Renderer *renderer);

#endif /* AR_RENDER_SDL_INTERNAL_H */
