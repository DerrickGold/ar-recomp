#include "render_sdl.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

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
    case kArRenderBlendMode_Add: return SDL_BLENDMODE_ADD;
    case kArRenderBlendMode_Modulate: return SDL_BLENDMODE_MOD;
    case kArRenderBlendMode_Multiply: return SDL_BLENDMODE_MUL;
  }
  return SDL_BLENDMODE_INVALID;
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

static bool SetRenderTarget(void *context, ArRenderTexture target) {
  ArSdlRenderBackend *backend = context;
  return SDL_SetRenderTarget(
      backend->renderer, ArSdlRenderBackend_UnwrapTexture(target));
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

static uint8_t ColorChannel(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 1.0f) return UINT8_MAX;
  return (uint8_t)(value * 255.0f + 0.5f);
}

static bool Clear(void *context, ArRenderColorF color) {
  ArSdlRenderBackend *backend = context;
  return SDL_SetRenderDrawColor(
             backend->renderer, ColorChannel(color.r), ColorChannel(color.g),
             ColorChannel(color.b), ColorChannel(color.a)) &&
      SDL_RenderClear(backend->renderer);
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

static bool ApplyTextureDrawState(SDL_Texture *texture,
                                  const ArRenderDrawState *state,
                                  SavedTextureDrawState *saved) {
  if (state->flags & kArRenderDrawState_Tint) {
    if (!SDL_GetTextureColorModFloat(
            texture, &saved->tint_r, &saved->tint_g, &saved->tint_b) ||
        !SDL_GetTextureAlphaModFloat(texture, &saved->tint_a))
      return false;
    saved->tint_saved = true;
  }
  if (state->flags & kArRenderDrawState_Blend) {
    if (!SDL_GetTextureBlendMode(texture, &saved->blend)) return false;
    saved->blend_saved = true;
  }

  if ((state->flags & kArRenderDrawState_Tint) &&
      (!SDL_SetTextureColorModFloat(
          texture, state->tint.r, state->tint.g, state->tint.b) ||
       !SDL_SetTextureAlphaModFloat(texture, state->tint.a)))
    return false;
  if ((state->flags & kArRenderDrawState_Blend) &&
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

  SavedTextureDrawState saved = {0};
  const bool applied = ApplyTextureDrawState(native_texture, state, &saved);
  const bool rendered = applied && SDL_RenderTexture(
      backend->renderer, native_texture, source_rect, destination_rect);
  const bool restored = RestoreTextureDrawState(native_texture, &saved);
  return rendered && restored;
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
    const bool applied = saved && SDL_SetRenderDrawBlendMode(
        backend->renderer, ToSdlBlendMode(state->blend));
    const bool rendered = applied && SDL_RenderGeometry(
        backend->renderer, NULL, (const SDL_Vertex *)vertices,
        vertex_count, (const int *)indices, index_count);
    const bool restored = saved && SDL_SetRenderDrawBlendMode(
        backend->renderer, saved_blend);
    return rendered && restored;
  }

  SavedTextureDrawState saved = {0};
  const bool applied = ApplyTextureDrawState(native_texture, state, &saved);
  const bool rendered = applied && SDL_RenderGeometry(
      backend->renderer, native_texture, (const SDL_Vertex *)vertices,
      vertex_count, (const int *)indices, index_count);
  const bool restored = RestoreTextureDrawState(native_texture, &saved);
  return rendered && restored;
}

static bool Present(void *context) {
  ArSdlRenderBackend *backend = context;
  return SDL_RenderPresent(backend->renderer);
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
  .set_viewport = SetViewport,
  .set_clip_rect = SetClipRect,
  .clear = Clear,
  .draw_texture = DrawTexture,
  .draw_geometry = DrawGeometry,
  .present = Present,
  .last_error = LastError,
};

bool ArSdlRenderBackend_Bind(ArRenderDevice *device,
                             ArSdlRenderBackend *backend,
                             SDL_Renderer *renderer) {
  if (!device || !backend || !renderer) return false;
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
  if (gpu) {
    capabilities.flags |= kArRenderCapability_CustomShaders;
    if (SDL_GPUTextureSupportsFormat(
            gpu, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
      capabilities.flags |= kArRenderCapability_Depth;
  }
  return ArRenderDevice_Init(
      device, &kSdlRenderOps, backend, capabilities);
}
