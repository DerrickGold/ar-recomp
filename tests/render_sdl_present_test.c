/* Deterministic adapter protocol test. A hidden/minimized GPU window can
 * acquire successfully without a swapchain image even on a healthy device.
 * Intercept only the public SDL calls at that boundary, not driver internals. */
#ifdef NDEBUG
#undef NDEBUG /* Protocol checks must also execute in optimized test builds. */
#endif
#include <SDL3/SDL.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Opaque identity tokens, never dereferenced as SDL objects. */
static max_align_t s_renderer, s_window, s_target, s_source, s_swapchain, s_device, s_commands;
static struct {
  bool wrong_target, fail_producer, no_source, no_commands;
  bool fail_acquire, no_swapchain, fail_submit;
  unsigned producers, acquired, cancelled, blitted, submitted;
} s_test;

static SDL_Texture *TestGetRenderTarget(SDL_Renderer *renderer) {
  assert(renderer == (SDL_Renderer *)&s_renderer);
  return s_test.wrong_target ? NULL : (SDL_Texture *)&s_target;
}
static bool TestRenderPresent(SDL_Renderer *renderer) {
  assert(renderer == (SDL_Renderer *)&s_renderer);
  ++s_test.producers;
  return !s_test.fail_producer;
}
static SDL_PropertiesID TestGetTextureProperties(SDL_Texture *texture) {
  assert(texture == (SDL_Texture *)&s_target);
  return 1;
}
static void *TestGetPointerProperty(SDL_PropertiesID props, const char *name, void *fallback) {
  assert(props == 1 && !strcmp(name, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER) && !fallback);
  return s_test.no_source ? NULL : &s_source;
}
static SDL_GPUCommandBuffer *TestAcquireCommands(SDL_GPUDevice *device) {
  assert(device == (SDL_GPUDevice *)&s_device && s_test.producers == 1);
  ++s_test.acquired;
  return s_test.no_commands ? NULL : (SDL_GPUCommandBuffer *)&s_commands;
}
static bool TestAcquireSwapchain(SDL_GPUCommandBuffer *commands, SDL_Window *window,
    SDL_GPUTexture **texture, Uint32 *width, Uint32 *height) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands && window == (SDL_Window *)&s_window);
  *texture = s_test.no_swapchain ? NULL : (SDL_GPUTexture *)&s_swapchain;
  *width = 800; *height = 600;
  return !s_test.fail_acquire;
}
static bool TestCancel(SDL_GPUCommandBuffer *commands) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands);
  ++s_test.cancelled;
  return true;
}
static bool TestSubmit(SDL_GPUCommandBuffer *commands) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands && !s_test.cancelled);
  ++s_test.submitted;
  return !s_test.fail_submit;
}
static void TestBlit(SDL_GPUCommandBuffer *commands, const SDL_GPUBlitInfo *blit) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands && !s_test.no_swapchain);
  assert(blit->source.texture == (SDL_GPUTexture *)&s_source);
  assert(blit->source.w == 640 && blit->source.h == 480);
  assert(blit->destination.texture == (SDL_GPUTexture *)&s_swapchain);
  assert(blit->destination.w == 800 && blit->destination.h == 600);
  assert(blit->load_op == SDL_GPU_LOADOP_DONT_CARE && blit->filter == SDL_GPU_FILTER_LINEAR);
  ++s_test.blitted;
}

#define SDL_GetRenderTarget TestGetRenderTarget
#define SDL_RenderPresent TestRenderPresent
#define SDL_GetTextureProperties TestGetTextureProperties
#define SDL_GetPointerProperty TestGetPointerProperty
#define SDL_AcquireGPUCommandBuffer TestAcquireCommands
#define SDL_WaitAndAcquireGPUSwapchainTexture TestAcquireSwapchain
#define SDL_CancelGPUCommandBuffer TestCancel
#define SDL_SubmitGPUCommandBuffer TestSubmit
#define SDL_BlitGPUTexture TestBlit
#include "../src/platform/sdl/render_sdl.c"

int main(void) {
  ArSdlRenderBackend backend = {
    .renderer = (SDL_Renderer *)&s_renderer, .gpu_device = (SDL_GPUDevice *)&s_device,
    .output_window = (SDL_Window *)&s_window, .output_target = (SDL_Texture *)&s_target,
    .output_width = 640, .output_height = 480,
  };
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device, &kSdlRenderOps, &backend, (ArRenderCapabilities){0}));
  /* Repeated hidden/visible transitions always submit the terminal buffer.
   * No blit is permitted without an image; no wait/readback API is introduced. */
  for (unsigned frame = 0; frame < 512; ++frame) {
    memset(&s_test, 0, sizeof(s_test));
    s_test.no_swapchain = (frame & 1) != 0;
    assert(ArRenderDevice_Present(&device));
    assert(s_test.producers == 1 && s_test.acquired == 1);
    assert(s_test.submitted == 1 && s_test.cancelled == 0);
    assert(s_test.blitted == (s_test.no_swapchain ? 0u : 1u));
  }
  for (unsigned hidden = 0; hidden < 2; ++hidden) {
    memset(&s_test, 0, sizeof(s_test));
    s_test.no_swapchain = hidden != 0;
    s_test.fail_submit = true;
    assert(!ArRenderDevice_Present(&device));
    assert(s_test.submitted == 1 && s_test.cancelled == 0);
  }
  memset(&s_test, 0, sizeof(s_test));
  s_test.fail_acquire = true;
  assert(!ArRenderDevice_Present(&device));
  assert(s_test.cancelled == 1 && !s_test.submitted && !s_test.blitted);
  for (unsigned failure = 0; failure < 4; ++failure) {
    memset(&s_test, 0, sizeof(s_test));
    s_test.wrong_target = failure == 0;
    s_test.fail_producer = failure == 1;
    s_test.no_source = failure == 2;
    s_test.no_commands = failure == 3;
    assert(!ArRenderDevice_Present(&device));
    assert(!s_test.cancelled && !s_test.submitted && !s_test.blitted);
    assert(s_test.acquired == (failure == 3 ? 1u : 0u));
  }
  memset(&s_test, 0, sizeof(s_test));
  backend.output_window = NULL; /* Externally bound legacy renderer. */
  assert(ArRenderDevice_Present(&device));
  assert(s_test.producers == 1 && !s_test.acquired && !s_test.submitted);
  puts("render_sdl_present_test: PASS");
  return 0;
}
