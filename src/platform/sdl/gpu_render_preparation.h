#ifndef AR_GPU_RENDER_PREPARATION_H
#define AR_GPU_RENDER_PREPARATION_H

#include "platform/sdl/render_sdl_internal.h"
#include "session_fatal.h"

/* Creating an SDL custom fragment state alone need not materialize its draw
 * pipelines. Exercise the ordinary blend variants against scratch targets
 * before publishing the state as usable. No window present or readback. */
static inline bool GpuRenderPreparation_WarmTarget(ArRenderDevice *device,
    SDL_GPURenderState *shader, const void *uniforms, Uint32 uniform_bytes,
    bool geometry, ArRenderPixelFormat format) {
  if (SessionFatal_Requested()) return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  ArRenderTexture source = {0}, target = {0};
  ArRenderTextureDesc desc = {1, 1, kArRenderPixelFormat_Argb8888,
      kArRenderTextureUsage_Static, kArRenderFilter_Linear, kArRenderBlendMode_Alpha};
  const uint32_t pixel = UINT32_C(0xffffffff);
  bool ok = renderer && shader && ArRenderDevice_CreateTexture(device, &desc, &source) &&
      ArRenderDevice_UpdateTexture(device, source, NULL, &pixel, sizeof(pixel));
  desc.width = desc.height = 8;
  desc.usage = kArRenderTextureUsage_Target;
  desc.format = format;
  if (ok) ok = ArRenderDevice_CreateTexture(device, &desc, &target);
  ArRenderTargetState saved = {0};
  ArRenderTargetBeginResult begin = kArRenderTargetBegin_Omitted;
  if (ok) begin = ArRenderDevice_BeginTarget(device, target, &saved);
  ok = ok && begin == kArRenderTargetBegin_Ready;
  if (ok) {
    ok = ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 0}) &&
        SDL_SetGPURenderStateFragmentUniforms(shader, 0, uniforms, uniform_bytes) &&
        SDL_SetGPURenderState(renderer, shader);
    const ArRenderBlendMode blends[] = {kArRenderBlendMode_Opaque,
        kArRenderBlendMode_Alpha, kArRenderBlendMode_Add,
        kArRenderBlendMode_AlphaPremultiplied};
    for (size_t i = 0; ok && i < sizeof(blends) / sizeof(blends[0]); ++i) {
      const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend, .blend = blends[i]};
      if (geometry) {
        const ArRenderVertex2D vertices[] = {
          {{0, 0}, {1, 1, 1, 1}, {0, 0}}, {{8, 0}, {1, 1, 1, 1}, {1, 0}},
          {{0, 8}, {1, 1, 1, 1}, {0, 1}},
        };
        const int32_t indices[] = {0, 1, 2};
        ok = ArRenderDevice_DrawGeometryWithState(device, source, vertices, 3, indices, 3, &state);
      } else {
        ok = ArRenderDevice_DrawTextureWithState(device, source, NULL, NULL, &state);
      }
    }
    /* Force queued SDL commands to materialize their pipelines now. */
    if (ok) ok = ArSdlRenderBackend_SubmitPending(device);
    if (!SDL_SetGPURenderState(renderer, NULL)) {
      SessionFatal_Request("Shader preparation could not restore its render state: %s", SDL_GetError());
      ok = false;
    }
  }
  if (begin == kArRenderTargetBegin_StateLost ||
      (begin == kArRenderTargetBegin_Ready && !ArRenderDevice_EndTarget(device, &saved))) {
    SessionFatal_Request("Shader preparation could not restore its scratch target: %s", SDL_GetError());
    ok = false;
  }
  ArRenderDevice_DestroyTexture(device, source);
  ArRenderDevice_DestroyTexture(device, target);
  return ok;
}

static inline bool GpuRenderPreparation_Warm(ArRenderDevice *device,
    SDL_GPURenderState *shader, const void *uniforms, Uint32 uniform_bytes, bool geometry) {
  /* Both byte orders are used by the renderer: captured layers are ARGB,
   * while the output/CRT target may be RGBA on a different GPU backend. */
  return GpuRenderPreparation_WarmTarget(device, shader, uniforms, uniform_bytes,
             geometry, kArRenderPixelFormat_Argb8888) &&
      GpuRenderPreparation_WarmTarget(device, shader, uniforms, uniform_bytes,
             geometry, kArRenderPixelFormat_Abgr8888);
}

#endif
