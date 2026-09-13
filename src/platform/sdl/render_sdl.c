#include "render_sdl_internal.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct ArSdlFragmentShaderEntry {
  const GpuShaderBlobs *blobs;
  Uint32 samplers, uniform_buffers;
  SDL_GPUShader *shader;
  struct ArSdlFragmentShaderEntry *next;
} ArSdlFragmentShaderEntry;

_Static_assert(sizeof(int32_t) == sizeof(int),
               "SDL geometry indices require 32-bit int");
_Static_assert(sizeof(ArRenderVertex2D) == sizeof(SDL_Vertex),
               "portable and SDL vertex batches must stay layout-compatible");
_Static_assert(offsetof(ArRenderVertex2D, position) ==
                   offsetof(SDL_Vertex, position),
               "portable and SDL vertex positions must stay compatible");
_Static_assert(offsetof(ArRenderVertex2D, color) ==
                   offsetof(SDL_Vertex, color),
               "portable and SDL vertex colors must stay compatible");
_Static_assert(offsetof(ArRenderVertex2D, tex_coord) ==
                   offsetof(SDL_Vertex, tex_coord),
               "portable and SDL vertex UVs must stay compatible");

ArRenderTexture ArSdlRenderBackend_BorrowTexture(SDL_Texture *texture) {
  return (ArRenderTexture){(uintptr_t)texture};
}

SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture) {
  return (SDL_Texture *)texture.value;
}

static SDL_PixelFormat ToSdlPixelFormat(ArRenderPixelFormat format) {
  switch (format) {
    case kArRenderPixelFormat_Argb8888: return SDL_PIXELFORMAT_ARGB8888;
    case kArRenderPixelFormat_Abgr8888: return SDL_PIXELFORMAT_ABGR8888;
    case kArRenderPixelFormat_Rgba8888: return SDL_PIXELFORMAT_RGBA8888;
    case kArRenderPixelFormat_Rgb565: return SDL_PIXELFORMAT_RGB565;
    case kArRenderPixelFormat_Rgba4444: return SDL_PIXELFORMAT_RGBA4444;
    /* SDL_Renderer has no portable single-channel alpha texture format. */
    case kArRenderPixelFormat_A8: return SDL_PIXELFORMAT_UNKNOWN;
  }
  return SDL_PIXELFORMAT_UNKNOWN;
}

static SDL_TextureAccess ToSdlTextureAccess(ArRenderTextureUsage usage) {
  switch (usage) {
    case kArRenderTextureUsage_Static: return SDL_TEXTUREACCESS_STATIC;
    case kArRenderTextureUsage_Streaming: return SDL_TEXTUREACCESS_STREAMING;
    case kArRenderTextureUsage_Target: return SDL_TEXTUREACCESS_TARGET;
  }
  return SDL_TEXTUREACCESS_STATIC;
}

static SDL_ScaleMode ToSdlScaleMode(ArRenderFilter filter) {
  return filter == kArRenderFilter_Linear
      ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
}

static SDL_BlendMode ToSdlBlendMode(ArRenderBlendMode blend) {
  switch (blend) {
    case kArRenderBlendMode_Opaque: return SDL_BLENDMODE_NONE;
    case kArRenderBlendMode_Alpha: return SDL_BLENDMODE_BLEND;
    case kArRenderBlendMode_AlphaPremultiplied:
      return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
    case kArRenderBlendMode_Add: return SDL_BLENDMODE_ADD;
    case kArRenderBlendMode_AddPremultiplied:
      return SDL_BLENDMODE_ADD_PREMULTIPLIED;
    case kArRenderBlendMode_AlphaAccumulate:
      return SDL_ComposeCustomBlendMode(
          SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
          SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
          SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    case kArRenderBlendMode_DestinationAlphaMask:
      return SDL_ComposeCustomBlendMode(
          SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
          SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
          SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
    case kArRenderBlendMode_Modulate: return SDL_BLENDMODE_MOD;
    case kArRenderBlendMode_Multiply: return SDL_BLENDMODE_MUL;
  }
  return SDL_BLENDMODE_INVALID;
}

static SDL_TextureAddressMode ToSdlTextureAddressMode(
    ArRenderTextureAddressMode address) {
  switch (address) {
    case kArRenderTextureAddressMode_Auto: return SDL_TEXTURE_ADDRESS_AUTO;
    case kArRenderTextureAddressMode_Clamp: return SDL_TEXTURE_ADDRESS_CLAMP;
    case kArRenderTextureAddressMode_Wrap: return SDL_TEXTURE_ADDRESS_WRAP;
  }
  return SDL_TEXTURE_ADDRESS_INVALID;
}

static bool CreateTexture(void *context, const ArRenderTextureDesc *desc,
                          ArRenderTexture *out_texture) {
  ArSdlRenderBackend *backend = context;
  SDL_PixelFormat format = ToSdlPixelFormat(desc->format);
  SDL_BlendMode blend = ToSdlBlendMode(desc->blend);
  if (!backend || !backend->renderer || format == SDL_PIXELFORMAT_UNKNOWN ||
      blend == SDL_BLENDMODE_INVALID) {
    SDL_SetError("invalid portable texture descriptor");
    return false;
  }
  SDL_Texture *texture = SDL_CreateTexture(
      backend->renderer, format, ToSdlTextureAccess(desc->usage),
      desc->width, desc->height);
  if (!texture) return false;
  if (!SDL_SetTextureScaleMode(texture, ToSdlScaleMode(desc->filter)) ||
      !SDL_SetTextureBlendMode(texture, blend)) {
    SDL_DestroyTexture(texture);
    return false;
  }
  *out_texture = ArSdlRenderBackend_BorrowTexture(texture);
  return true;
}

static void DestroyTexture(void *context, ArRenderTexture texture) {
  (void)context;
  SDL_DestroyTexture(ArSdlRenderBackend_UnwrapTexture(texture));
}

static bool UpdateTexture(void *context, ArRenderTexture texture,
                          const ArRenderRectI *destination,
                          const void *pixels, int pitch_bytes) {
  (void)context;
  SDL_Rect converted;
  const SDL_Rect *rect = NULL;
  if (destination) {
    converted = (SDL_Rect){destination->x, destination->y,
                          destination->w, destination->h};
    rect = &converted;
  }
  return SDL_UpdateTexture(
      ArSdlRenderBackend_UnwrapTexture(texture), rect, pixels, pitch_bytes);
}

static bool EnsureOutputTarget(ArSdlRenderBackend *backend) {
  if (!backend->output_window) return true;
  int width = 0, height = 0;
  if (!SDL_GetWindowSizeInPixels(backend->output_window, &width, &height)) return false;
  /* Minimized windows can have no drawable extent. Keep their last complete
   * target; swapchain acquisition can skip the final blit until restored. */
  if (width <= 0 || height <= 0) return backend->output_target != NULL;
  if (backend->output_target && width == backend->output_width && height == backend->output_height)
    return true;
  const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(
      backend->gpu_device, backend->output_window);
  SDL_Texture *target = SDL_CreateTexture(backend->renderer,
      SDL_GetPixelFormatFromGPUTextureFormat(format), SDL_TEXTUREACCESS_TARGET, width, height);
  if (!target) return false;
  if (!SDL_SetTextureBlendMode(target, SDL_BLENDMODE_NONE) ||
      !SDL_SetTextureScaleMode(target, SDL_SCALEMODE_LINEAR)) {
    SDL_DestroyTexture(target);
    return false;
  }
  /* Resize only when selecting/querying the logical default output. Its old
   * handle is never exported through a portable saved-target state. */
  SDL_Texture *old = backend->output_target;
  if (SDL_GetRenderTarget(backend->renderer) == old &&
      !SDL_SetRenderTarget(backend->renderer, target)) {
    SDL_DestroyTexture(target);
    return false;
  }
  backend->output_target = target;
  backend->output_width = width;
  backend->output_height = height;
  if (old) SDL_DestroyTexture(old);
  return true;
}

static bool SetRenderTarget(void *context, ArRenderTexture target) {
  ArSdlRenderBackend *backend = context;
  if (!ArRenderTexture_IsValid(target) && backend->output_window) {
    if (!EnsureOutputTarget(backend)) return false;
    return SDL_SetRenderTarget(backend->renderer, backend->output_target);
  }
  return SDL_SetRenderTarget(
      backend->renderer, ArSdlRenderBackend_UnwrapTexture(target));
}

static bool UseOutputCoordinates(void *context) {
  ArSdlRenderBackend *backend = context;
  return SDL_SetRenderLogicalPresentation(
      backend->renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
}

static bool GetOutputSize(void *context, int *width, int *height) {
  ArSdlRenderBackend *backend = context;
  SDL_Texture *target = SDL_GetRenderTarget(backend->renderer);
  if (backend->output_window && target == backend->output_target) {
    if (!EnsureOutputTarget(backend)) return false;
    *width = backend->output_width;
    *height = backend->output_height;
    return true;
  }
  if (!target)
    return SDL_GetRenderOutputSize(backend->renderer, width, height);
  float target_width = 0.0f;
  float target_height = 0.0f;
  if (!SDL_GetTextureSize(target, &target_width, &target_height)) return false;
  *width = (int)target_width;
  *height = (int)target_height;
  return *width > 0 && *height > 0;
}

static bool SetViewport(void *context, const ArRenderRectI *viewport) {
  ArSdlRenderBackend *backend = context;
  SDL_Rect converted;
  const SDL_Rect *rect = NULL;
  if (viewport) {
    converted = (SDL_Rect){viewport->x, viewport->y,
                          viewport->w, viewport->h};
    rect = &converted;
  }
  return SDL_SetRenderViewport(backend->renderer, rect);
}

static bool SetClipRect(void *context, const ArRenderRectI *clip) {
  ArSdlRenderBackend *backend = context;
  SDL_Rect converted;
  const SDL_Rect *rect = NULL;
  if (clip) {
    converted = (SDL_Rect){clip->x, clip->y, clip->w, clip->h};
    rect = &converted;
  }
  return SDL_SetRenderClipRect(backend->renderer, rect);
}

static bool CaptureRenderTargetState(void *context,
                                     ArRenderTargetState *state) {
  ArSdlRenderBackend *backend = context;
  if (!backend || !backend->renderer || !state) return false;
  SDL_Rect viewport;
  if (!SDL_GetRenderViewport(backend->renderer, &viewport)) return false;
  SDL_Texture *target = SDL_GetRenderTarget(backend->renderer);
  *state = (ArRenderTargetState){
    .target = ArSdlRenderBackend_BorrowTexture(
        target == backend->output_target ? NULL : target),
    .viewport = {viewport.x, viewport.y, viewport.w, viewport.h},
    .viewport_set = SDL_RenderViewportSet(backend->renderer),
    .clip_enabled = SDL_RenderClipEnabled(backend->renderer),
    .valid = true,
  };
  if (state->clip_enabled) {
    SDL_Rect clip;
    if (!SDL_GetRenderClipRect(backend->renderer, &clip)) return false;
    state->clip = (ArRenderRectI){clip.x, clip.y, clip.w, clip.h};
  }
  return true;
}

static bool RestoreRenderTargetState(
    void *context, const ArRenderTargetState *state) {
  ArSdlRenderBackend *backend = context;
  if (!backend || !backend->renderer || !state || !state->valid) return false;
  const SDL_Rect viewport = {
    state->viewport.x, state->viewport.y,
    state->viewport.w, state->viewport.h,
  };
  const SDL_Rect clip = {
    state->clip.x, state->clip.y, state->clip.w, state->clip.h,
  };
  bool restored = SetRenderTarget(context, state->target);
  restored = SDL_SetRenderViewport(
      backend->renderer, state->viewport_set ? &viewport : NULL) && restored;
  restored = SDL_SetRenderClipRect(
      backend->renderer, state->clip_enabled ? &clip : NULL) && restored;
  return restored;
}

static uint8_t ColorChannel(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 1.0f) return UINT8_MAX;
  return (uint8_t)(value * 255.0f + 0.5f);
}

static bool Clear(void *context, ArRenderColorF color) {
  ArSdlRenderBackend *backend = context;
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  if (!SDL_GetRenderDrawColor(
          backend->renderer, &old_r, &old_g, &old_b, &old_a))
    return false;
  const bool cleared = SDL_SetRenderDrawColor(
                           backend->renderer,
                           ColorChannel(color.r), ColorChannel(color.g),
                           ColorChannel(color.b), ColorChannel(color.a)) &&
      SDL_RenderClear(backend->renderer);
  const bool restored = SDL_SetRenderDrawColor(
      backend->renderer, old_r, old_g, old_b, old_a);
  return cleared && restored;
}

typedef struct SavedTextureDrawState {
  SDL_BlendMode blend;
  float tint_r;
  float tint_g;
  float tint_b;
  float tint_a;
  bool blend_saved;
  bool tint_saved;
} SavedTextureDrawState;

typedef struct SavedTextureAddressState {
  SDL_TextureAddressMode u;
  SDL_TextureAddressMode v;
  bool saved;
} SavedTextureAddressState;

static bool ApplyTextureAddressState(
    SDL_Renderer *renderer, const ArRenderDrawState *state,
    SavedTextureAddressState *saved) {
  if (!(state->flags & kArRenderDrawState_Address)) return true;
  if (!SDL_GetRenderTextureAddressMode(renderer, &saved->u, &saved->v))
    return false;
  const SDL_TextureAddressMode desired_u =
      ToSdlTextureAddressMode(state->address_u);
  const SDL_TextureAddressMode desired_v =
      ToSdlTextureAddressMode(state->address_v);
  if (saved->u == desired_u && saved->v == desired_v) return true;
  saved->saved = true;
  return SDL_SetRenderTextureAddressMode(renderer, desired_u, desired_v);
}

static bool RestoreTextureAddressState(
    SDL_Renderer *renderer, const SavedTextureAddressState *saved) {
  return !saved->saved ||
      SDL_SetRenderTextureAddressMode(renderer, saved->u, saved->v);
}

static bool ApplyTextureDrawState(SDL_Texture *texture,
                                  const ArRenderDrawState *state,
                                  SavedTextureDrawState *saved) {
  if (state->flags & kArRenderDrawState_Tint) {
    if (!SDL_GetTextureColorModFloat(
            texture, &saved->tint_r, &saved->tint_g, &saved->tint_b) ||
        !SDL_GetTextureAlphaModFloat(texture, &saved->tint_a))
      return false;
    if (saved->tint_r != state->tint.r ||
        saved->tint_g != state->tint.g ||
        saved->tint_b != state->tint.b ||
        saved->tint_a != state->tint.a)
      saved->tint_saved = true;
  }
  if (state->flags & kArRenderDrawState_Blend) {
    if (!SDL_GetTextureBlendMode(texture, &saved->blend)) return false;
    if (saved->blend != ToSdlBlendMode(state->blend))
      saved->blend_saved = true;
  }

  if (saved->tint_saved &&
      (!SDL_SetTextureColorModFloat(
          texture, state->tint.r, state->tint.g, state->tint.b) ||
       !SDL_SetTextureAlphaModFloat(texture, state->tint.a)))
    return false;
  if (saved->blend_saved &&
      !SDL_SetTextureBlendMode(texture, ToSdlBlendMode(state->blend)))
    return false;
  return true;
}

static bool RestoreTextureDrawState(SDL_Texture *texture,
                                    const SavedTextureDrawState *saved) {
  bool success = true;
  if (saved->tint_saved) {
    if (!SDL_SetTextureColorModFloat(
            texture, saved->tint_r, saved->tint_g, saved->tint_b))
      success = false;
    if (!SDL_SetTextureAlphaModFloat(texture, saved->tint_a)) success = false;
  }
  if (saved->blend_saved &&
      !SDL_SetTextureBlendMode(texture, saved->blend))
    success = false;
  return success;
}

static bool DrawTexture(void *context, ArRenderTexture texture,
                        const ArRenderRectF *source,
                        const ArRenderRectF *destination,
                        const ArRenderDrawState *state) {
  ArSdlRenderBackend *backend = context;
  SDL_Texture *native_texture = ArSdlRenderBackend_UnwrapTexture(texture);
  SDL_FRect converted_source;
  SDL_FRect converted_destination;
  const SDL_FRect *source_rect = NULL;
  const SDL_FRect *destination_rect = NULL;
  if (source) {
    converted_source = (SDL_FRect){source->x, source->y, source->w, source->h};
    source_rect = &converted_source;
  }
  if (destination) {
    converted_destination = (SDL_FRect){destination->x, destination->y,
                                       destination->w, destination->h};
    destination_rect = &converted_destination;
  }
  if (!state || state->flags == 0)
    return SDL_RenderTexture(
        backend->renderer, native_texture, source_rect, destination_rect);
  SavedTextureAddressState saved_address = {0};
  SavedTextureDrawState saved = {0};
  const bool address_applied = ApplyTextureAddressState(
      backend->renderer, state, &saved_address);
  const bool applied = address_applied &&
      ApplyTextureDrawState(native_texture, state, &saved);
  const bool rendered = applied && SDL_RenderTexture(
      backend->renderer, native_texture, source_rect, destination_rect);
  const bool restored = RestoreTextureDrawState(native_texture, &saved);
  const bool address_restored = RestoreTextureAddressState(
      backend->renderer, &saved_address);
  return rendered && restored && address_restored;
}

static bool DrawGeometry(void *context, ArRenderTexture texture,
                         const ArRenderVertex2D *vertices, int vertex_count,
                         const int32_t *indices, int index_count,
                         const ArRenderDrawState *state) {
  ArSdlRenderBackend *backend = context;
  SDL_Texture *native_texture = ArSdlRenderBackend_UnwrapTexture(texture);
  if (!state || state->flags == 0)
    return SDL_RenderGeometry(
        backend->renderer, native_texture, (const SDL_Vertex *)vertices,
        vertex_count, (const int *)indices, index_count);

  /* SDL sources untextured-geometry blending from renderer draw state and
   * textured-geometry blending from the texture. Preserve the appropriate
   * native owner so the portable per-batch contract works for both forms. */
  if (!native_texture) {
    SDL_BlendMode saved_blend = SDL_BLENDMODE_INVALID;
    const bool saved = SDL_GetRenderDrawBlendMode(
        backend->renderer, &saved_blend);
    const SDL_BlendMode desired = ToSdlBlendMode(state->blend);
    const bool changed = saved && saved_blend != desired;
    const bool applied = saved && (!changed || SDL_SetRenderDrawBlendMode(
        backend->renderer, desired));
    const bool rendered = applied && SDL_RenderGeometry(
        backend->renderer, NULL, (const SDL_Vertex *)vertices,
        vertex_count, (const int *)indices, index_count);
    const bool restored = saved && (!changed || SDL_SetRenderDrawBlendMode(
        backend->renderer, saved_blend));
    return rendered && restored;
  }

  SavedTextureAddressState saved_address = {0};
  SavedTextureDrawState saved = {0};
  const bool address_applied = ApplyTextureAddressState(
      backend->renderer, state, &saved_address);
  const bool applied = address_applied &&
      ApplyTextureDrawState(native_texture, state, &saved);
  const bool rendered = applied && SDL_RenderGeometry(
      backend->renderer, native_texture, (const SDL_Vertex *)vertices,
      vertex_count, (const int *)indices, index_count);
  const bool restored = RestoreTextureDrawState(native_texture, &saved);
  const bool address_restored = RestoreTextureAddressState(
      backend->renderer, &saved_address);
  return rendered && restored && address_restored;
}

static bool Present(void *context) {
  ArSdlRenderBackend *backend = context;
  if (!backend->output_window) return SDL_RenderPresent(backend->renderer);
  if (SDL_GetRenderTarget(backend->renderer) != backend->output_target)
    return SDL_SetError("ordered GPU present requires the default output target");
  if (!SDL_RenderPresent(backend->renderer)) return false;
  SDL_GPUTexture *source = SDL_GetPointerProperty(SDL_GetTextureProperties(backend->output_target),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
  if (!source) return SDL_SetError("ordered GPU output target has no native texture");
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(backend->gpu_device);
  if (!commands) return false;
  SDL_GPUTexture *swapchain = NULL;
  Uint32 width = 0, height = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, backend->output_window,
          &swapchain, &width, &height)) {
    SDL_CancelGPUCommandBuffer(commands);
    return false;
  }
  /* A hidden/minimized window can acquire successfully without an image.
   * Still submit: SDL's Vulkan backend uses this swapchain-requesting buffer
   * to retire preceding offscreen work and recycle its resources. Cancelling
   * it indefinitely leaves completed work retained and forces buffer growth.
   * This adds neither a blit nor a fence/readback wait when no image exists. */
  if (!swapchain) return SDL_SubmitGPUCommandBuffer(commands);
  const SDL_GPUBlitInfo blit = {
    .source = {.texture = source, .w = (Uint32)backend->output_width,
        .h = (Uint32)backend->output_height},
    .destination = {.texture = swapchain, .w = width, .h = height},
    .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_LINEAR,
  };
  SDL_BlitGPUTexture(commands, &blit);
  return SDL_SubmitGPUCommandBuffer(commands);
}

static const char *LastError(void *context) {
  (void)context;
  return SDL_GetError();
}

static const ArRenderBackendOps kSdlRenderOps = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = CreateTexture,
  .destroy_texture = DestroyTexture,
  .update_texture = UpdateTexture,
  .set_render_target = SetRenderTarget,
  .use_output_coordinates = UseOutputCoordinates,
  .get_output_size = GetOutputSize,
  .set_viewport = SetViewport,
  .set_clip_rect = SetClipRect,
  .capture_render_target_state = CaptureRenderTargetState,
  .restore_render_target_state = RestoreRenderTargetState,
  .clear = Clear,
  .draw_texture = DrawTexture,
  .draw_geometry = DrawGeometry,
  .present = Present,
  .last_error = LastError,
};

SDL_Renderer *ArSdlRenderBackend_Renderer(const ArRenderDevice *device) {
  if (!device || device->ops != &kSdlRenderOps || !device->context)
    return NULL;
  return ((ArSdlRenderBackend *)device->context)->renderer;
}

bool ArSdlRenderBackend_SubmitPending(const ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer) return false;
  const ArSdlRenderBackend *backend = device->context;
  /* Preserve the explicit recording-failure check before submission: SDL's
   * RenderPresent does not propagate its internal command-flush result. */
  if (!SDL_FlushRenderer(renderer)) return false;
  return !backend->output_window || SDL_RenderPresent(renderer);
}

bool ArSdlRenderBackend_WindowOutputSize(const ArRenderDevice *device,
    int *width, int *height) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || !width || !height) return false;
  const ArSdlRenderBackend *backend = device->context;
  return backend->output_window ? SDL_GetWindowSizeInPixels(backend->output_window, width, height)
      : SDL_GetRenderOutputSize(renderer, width, height);
}

SDL_GPUShader *ArSdlRenderBackend_FragmentShader(ArRenderDevice *device,
    const GpuShaderBlobs *blobs, const char *label,
    Uint32 samplers, Uint32 uniform_buffers) {
  if (!device || device->ops != &kSdlRenderOps || !device->context || !blobs)
    return NULL;
  ArSdlRenderBackend *backend = device->context;
  if (!backend->gpu_device) return NULL;
  for (ArSdlFragmentShaderEntry *entry = backend->fragment_shaders; entry; entry = entry->next)
    if (entry->blobs == blobs && entry->samplers == samplers &&
        entry->uniform_buffers == uniform_buffers) return entry->shader;
  ArSdlFragmentShaderEntry *entry = calloc(1, sizeof(*entry));
  if (!entry) { SDL_OutOfMemory(); return NULL; }
  entry->shader = GpuShaderBlob_CreateFragment(backend->gpu_device, blobs, label,
      samplers, uniform_buffers);
  if (!entry->shader) { free(entry); return NULL; }
  entry->blobs = blobs;
  entry->samplers = samplers;
  entry->uniform_buffers = uniform_buffers;
  entry->next = backend->fragment_shaders;
  backend->fragment_shaders = entry;
  return entry->shader;
}

bool ArSdlRenderBackend_Bind(ArRenderDevice *device,
                             ArSdlRenderBackend *backend,
                             SDL_Renderer *renderer) {
  if (!device || !backend || !renderer) return false;
  if (ArRenderDevice_IsReady(device)) {
    SDL_SetError("render device already has a backend");
    return false;
  }
  memset(backend, 0, sizeof(*backend));
  backend->renderer = renderer;

  SDL_PropertiesID properties = SDL_GetRendererProperties(renderer);
  Sint64 maximum_texture_size = properties
      ? SDL_GetNumberProperty(
            properties, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0)
      : 0;
  if (maximum_texture_size < 0 || maximum_texture_size > INT_MAX)
    maximum_texture_size = 0;
  ArRenderCapabilities capabilities = {
    .flags = kArRenderCapability_StreamingTextures |
             kArRenderCapability_RenderTargets |
             kArRenderCapability_ScopedRenderTargets |
             kArRenderCapability_Geometry |
             kArRenderCapability_BlendAdd |
             kArRenderCapability_BlendModulate |
             kArRenderCapability_BlendMultiply,
    .maximum_texture_width = (int)maximum_texture_size,
    .maximum_texture_height = (int)maximum_texture_size,
    .maximum_render_target_width = (int)maximum_texture_size,
    .maximum_render_target_height = (int)maximum_texture_size,
  };
  if (properties && SDL_GetBooleanProperty(
          properties, SDL_PROP_RENDERER_TEXTURE_WRAPPING_BOOLEAN, false))
    capabilities.flags |= kArRenderCapability_TextureWrap;

  SDL_GPUDevice *gpu = properties ? SDL_GetPointerProperty(
      properties, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL) : NULL;
  backend->gpu_device = gpu;
  if (gpu) {
    const SDL_PropertiesID gpu_properties = SDL_GetGPUDeviceProperties(gpu);
    SDL_Log("[graphics-capabilities] backend=%s device=%s driver=%s version=%s shader-formats=$%x",
        SDL_GetGPUDeviceDriver(gpu),
        SDL_GetStringProperty(gpu_properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unreported"),
        SDL_GetStringProperty(gpu_properties, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "unreported"),
        SDL_GetStringProperty(gpu_properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unreported"),
        (unsigned)SDL_GetGPUShaderFormats(gpu));
    capabilities.flags |= kArRenderCapability_CustomShaders;
    if (SDL_GPUTextureSupportsFormat(
            gpu, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
      capabilities.flags |= kArRenderCapability_Depth;
  }
  return ArRenderDevice_Init(
      device, &kSdlRenderOps, backend, capabilities);
}

static SDL_GPUDevice *CreateOutputGpuDevice(SDL_Window *window) {
  /* Match the SDL GPU renderer's compatibility baseline. Taking ownership of
   * submission must not silently require unused clip-distance, anisotropic
   * sampling or indirect-draw features on lower-end Vulkan/D3D12 devices.
   * Hardware frustum clipping does not require shader clip-distance outputs. */
  SDL_PropertiesID props = SDL_CreateProperties();
  if (!props) return NULL;
  bool configured =
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
          SDL_GetHintBoolean(SDL_HINT_RENDER_GPU_DEBUG, false)) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN,
          SDL_GetHintBoolean(SDL_HINT_RENDER_GPU_LOW_POWER, false)) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN, true) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false) &&
      SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN, false);
  if (configured && (SDL_GetWindowFlags(window) & SDL_WINDOW_VULKAN))
    configured = SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
  SDL_GPUDevice *gpu = configured ? SDL_CreateGPUDeviceWithProperties(props) : NULL;
  SDL_DestroyProperties(props);
  return gpu;
}

bool ArSdlRenderBackend_CreateForWindow(ArRenderDevice *device,
                                        SDL_Window *window) {
  if (!device || !window) {
    SDL_SetError("invalid SDL render backend creation request");
    return false;
  }
  if (ArRenderDevice_IsReady(device)) {
    SDL_SetError("render device already has a backend");
    return false;
  }

  ArSdlRenderBackend *backend = calloc(1, sizeof(*backend));
  if (!backend) {
    SDL_SetError("out of memory creating SDL render backend");
    return false;
  }

  const char *ordered = getenv("AR_SDL_GPU_ORDERED");
  if (!ordered || strcmp(ordered, "0") != 0) {
    SDL_GPUDevice *gpu = CreateOutputGpuDevice(window);
    if (!gpu) { free(backend); return false; }
    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
      SDL_DestroyGPUDevice(gpu); free(backend); return false;
    }
    if (!SDL_SetGPUSwapchainParameters(gpu, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC)) {
      SDL_ReleaseWindowFromGPUDevice(gpu, window);
      SDL_DestroyGPUDevice(gpu); free(backend); return false;
    }
    SDL_Renderer *renderer = SDL_CreateGPURenderer(gpu, NULL);
    if (!renderer || !ArSdlRenderBackend_Bind(device, backend, renderer)) {
      if (renderer) SDL_DestroyRenderer(renderer);
      SDL_ReleaseWindowFromGPUDevice(gpu, window);
      SDL_DestroyGPUDevice(gpu); free(backend); return false;
    }
    backend->output_window = window;
    backend->output_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    backend->owns_renderer = backend->owns_context = backend->owns_gpu_device = true;
    if (!EnsureOutputTarget(backend) || !SetRenderTarget(backend, ArRenderTexture_Invalid())) {
      ArSdlRenderBackend_Destroy(device);
      return false;
    }
    SDL_Log("[render] ordered GPU interop enabled (offscreen SDL + one swapchain present)");
    return true;
  }

  SDL_PropertiesID properties = SDL_CreateProperties();
  if (!properties) {
    free(backend);
    return false;
  }
  bool configured =
      SDL_SetStringProperty(properties,
          SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER) &&
      SDL_SetPointerProperty(properties,
          SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window) &&
      SDL_SetBooleanProperty(properties,
          SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true) &&
      SDL_SetBooleanProperty(properties,
          SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true) &&
      SDL_SetBooleanProperty(properties,
          SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
  SDL_Renderer *renderer =
      configured ? SDL_CreateRendererWithProperties(properties) : NULL;
  SDL_DestroyProperties(properties);
  if (!renderer) {
    free(backend);
    return false;
  }

  if (!ArSdlRenderBackend_Bind(device, backend, renderer)) {
    SDL_DestroyRenderer(renderer);
    free(backend);
    return false;
  }
  backend->owns_renderer = true;
  backend->owns_context = true;
  return true;
}

void ArSdlRenderBackend_Destroy(ArRenderDevice *device) {
  if (!device) return;
  if (device->ops != &kSdlRenderOps || !device->context) {
    if (ArRenderDevice_IsReady(device))
      SDL_SetError("render device is not owned by the SDL backend");
    return;
  }
  ArSdlRenderBackend *backend = device->context;
  SDL_Renderer *renderer = backend->renderer;
  const bool owns_renderer = backend->owns_renderer;
  const bool owns_context = backend->owns_context;
  SDL_GPUDevice *gpu = backend->owns_gpu_device ? backend->gpu_device : NULL;
  SDL_Window *window = backend->output_window;
  if (backend->output_target) SDL_DestroyTexture(backend->output_target);
  /* No more draw states may be used after backend teardown. */
  while (backend->fragment_shaders) {
    ArSdlFragmentShaderEntry *entry = backend->fragment_shaders;
    backend->fragment_shaders = entry->next;
    SDL_ReleaseGPUShader(backend->gpu_device, entry->shader);
    free(entry);
  }
  ArRenderDevice_Reset(device);
  memset(backend, 0, sizeof(*backend));
  if (owns_renderer) SDL_DestroyRenderer(renderer);
  if (gpu) {
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
  }
  if (owns_context) free(backend);
}

bool ArSdlRenderBackend_SetVSync(ArRenderDevice *device, int requested,
                                 bool *active) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (active) *active = false;
  if (!renderer) {
    SDL_SetError("SDL render backend is not ready");
    return false;
  }
  ArSdlRenderBackend *backend = device->context;
  if (backend->output_window) {
    if (requested != 0 && requested != 1)
      return SDL_SetError("ordered GPU output supports vsync 0 or 1");
    SDL_GPUPresentMode mode = requested ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
    if (!requested && !SDL_WindowSupportsGPUPresentMode(backend->gpu_device, backend->output_window, mode))
      mode = SDL_GPU_PRESENTMODE_MAILBOX;
    const bool applied = SDL_SetGPUSwapchainParameters(backend->gpu_device, backend->output_window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    if (applied) backend->output_present_mode = mode;
    if (active) *active = backend->output_present_mode == SDL_GPU_PRESENTMODE_VSYNC;
    return applied;
  }
  const bool applied = SDL_SetRenderVSync(renderer, requested);
  int actual = 0;
  const bool queried = SDL_GetRenderVSync(renderer, &actual);
  if (active) *active = queried && actual != 0;
  return applied && queried;
}

bool ArSdlRenderBackend_SetAllowedFramesInFlight(ArRenderDevice *device,
                                                 uint32_t requested,
                                                 bool *changed) {
  if (changed) *changed = false;
  if (!device || device->ops != &kSdlRenderOps || !device->context) {
    SDL_SetError("SDL render backend is not ready");
    return false;
  }
  ArSdlRenderBackend *backend = device->context;
  if (!backend->gpu_device) {
    SDL_SetError("SDL GPU renderer did not expose its device");
    return false;
  }
  if (backend->frames_in_flight_applied &&
      backend->applied_frames_in_flight == requested)
    return true;
  if (!SDL_SetGPUAllowedFramesInFlight(backend->gpu_device, requested))
    return false;
  backend->applied_frames_in_flight = requested;
  backend->frames_in_flight_applied = true;
  if (changed) *changed = true;
  return true;
}
