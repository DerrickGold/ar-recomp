#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "platform/sdl/render_sdl_internal.h"
#include "render/gpu_compile_log.h"
#include "support/test_assert.h"

/* Run on the actual backend: startup warming must finish without presenting
 * scratch pixels, changing the caller's target/clip, or modifying its art. */
int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_Window *window = SDL_CreateWindow("pipeline preparation", 64, 64, SDL_WINDOW_HIDDEN);
  ArRenderDevice device = {0};
  if (!window || !ArSdlRenderBackend_CreateForWindow(&device, window, NULL)) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 77;
  }
  const ArRenderTextureDesc desc = {16,16,kArRenderPixelFormat_Argb8888,
      kArRenderTextureUsage_Target,kArRenderFilter_Nearest,kArRenderBlendMode_Opaque};
  ArRenderTexture target = {0};
  assert(ArRenderDevice_CreateTexture(&device, &desc, &target));
  assert(ArRenderDevice_SetRenderTarget(&device, target));
  assert(ArRenderDevice_Clear(&device, (ArRenderColorF){1,0,0,1}));
  assert(ArRenderDevice_SetViewport(&device, &(ArRenderRectI){2,3,11,10}));
  assert(ArRenderDevice_SetClipRect(&device, &(ArRenderRectI){1,2,5,4}));
  ArRenderTargetState before = {0}, after = {0};
  assert(ArRenderDevice_CaptureTargetState(&device, &before));
  GpuCompileLog_BeginPreparation(NULL, NULL);
  for (unsigned i = 0; i < 2; ++i) {
    assert(ArRenderDevice_FinishPreparation(&device));
    assert(ArRenderDevice_CaptureTargetState(&device, &after));
    assert(ArRenderTexture_Equals(before.target, after.target));
    assert(before.viewport_set == after.viewport_set && before.clip_enabled == after.clip_enabled);
    assert(!memcmp(&before.viewport, &after.viewport, sizeof(before.viewport)));
    assert(!memcmp(&before.clip, &after.clip, sizeof(before.clip)));
  }
  GpuCompileLog_EndPreparation();
  assert(ArRenderDevice_SetViewport(&device, NULL));
  assert(ArRenderDevice_SetClipRect(&device, NULL));
  SDL_Surface *raw = SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&device), NULL);
  assert(raw);
  SDL_Surface *pixels = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
  assert(pixels && pixels->w == 16 && pixels->h == 16);
  for (int y = 0; y < pixels->h; ++y)
    for (int x = 0; x < pixels->w; ++x)
      assert(((uint32_t *)((uint8_t *)pixels->pixels + y*pixels->pitch))[x] == UINT32_C(0xffff0000));
  SDL_DestroySurface(pixels);
  SDL_DestroySurface(raw);
  assert(ArRenderDevice_SetRenderTarget(&device, ArRenderTexture_Invalid()));
  ArRenderDevice_DestroyTexture(&device, target);
  ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  puts("render pipeline preparation: PASS");
  return 0;
}
