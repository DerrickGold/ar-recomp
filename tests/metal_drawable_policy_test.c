/* Real window/layer lifetime test. No Metal method interception or profiler. */
#include "platform/sdl/render_sdl_interop.h"
#include "platform/sdl/metal_drawable_sdl.h"
#include "support/test_assert.h"
#include "support/test_sdl_environment.h"
#include <stdio.h>

#if defined(SDL_PLATFORM_MACOS)
static void Present(ArRenderDevice *device, SDL_Window *window, unsigned count) {
  SDL_PumpEvents();
  assert(ArRenderDevice_SetRenderTarget(device, ArRenderTexture_Invalid()));
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  assert(SDL_SetRenderDrawColor(renderer, 31, 79, 127, 255));
  assert(SDL_RenderClear(renderer));
  assert(ArRenderDevice_Present(device));
  assert(ArSdlMetalDrawable_Configure(window, 0) == count);
}

static void CheckWindow(SDL_Window *window, const char *override, bool transitions) {
  assert(Test_SDLSetEnv("AR_METAL_DRAWABLE_COUNT", override, 1) == 0);
  ArRenderDevice device = {0};
  assert(ArSdlRenderBackend_CreateForWindow(&device, window, "metal"));
  const unsigned vsync_count = !SDL_strcmp(override, "3") ? 3 : 2;
  const unsigned immediate_count = !SDL_strcmp(override, "2") ? 2 : 3;
  bool active = false;
  assert(ArSdlRenderBackend_SetVSync(&device, 1, &active) && active);
  Present(&device, window, vsync_count);
  assert(ArSdlRenderBackend_SetVSync(&device, 0, &active) && !active);
  Present(&device, window, immediate_count);
  assert(ArSdlRenderBackend_SetVSync(&device, 1, &active) && active);
  if (transitions) {
    for (unsigned i = 0; i < 4; ++i) {
      assert(SDL_SetWindowSize(window, 480 + (int)i * 80, 300 + (int)i * 50));
      assert(SDL_SyncWindow(window));
      Present(&device, window, vsync_count);
      assert(SDL_HideWindow(window));
      Present(&device, window, vsync_count);
      assert(SDL_ShowWindow(window));
      Present(&device, window, vsync_count);
    }
    assert(SDL_MinimizeWindow(window));
    assert(SDL_SyncWindow(window));
    Present(&device, window, vsync_count);
    assert(SDL_RestoreWindow(window));
    assert(SDL_SyncWindow(window));
    Present(&device, window, vsync_count);
    assert(SDL_SetWindowFullscreen(window, true));
    assert(SDL_SyncWindow(window));
    Present(&device, window, vsync_count);
    assert(SDL_SetWindowFullscreen(window, false));
    assert(SDL_SyncWindow(window));
    Present(&device, window, vsync_count);
    int display_count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&display_count);
    assert(displays && display_count > 0);
    for (int i = 0; i < display_count; ++i) {
      SDL_Rect bounds;
      assert(SDL_GetDisplayUsableBounds(displays[i], &bounds));
      assert(SDL_SetWindowPosition(window, bounds.x + 40, bounds.y + 40));
      assert(SDL_SyncWindow(window));
      Present(&device, window, vsync_count);
    }
    SDL_free(displays);
    printf("Metal drawable lifetime: resize, hide/show, minimize/restore, "
        "fullscreen, %d displays\n", display_count);
  }
  Present(&device, window, vsync_count);
  ArSdlRenderBackend_Destroy(&device);
}
#endif

int main(void) {
#if defined(SDL_PLATFORM_MACOS)
  assert(Test_SDLSetEnv("AR_SDL_GPU_ORDERED", "1", 1) == 0);
  assert(Test_SDLSetEnv("SDL_GPU_DRIVER", "metal", 1) == 0);
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_Window *window = SDL_CreateWindow(
      "Metal drawable policy test", 480, 300, SDL_WINDOW_RESIZABLE);
  assert(window);
  /* Reclaiming the same window recreates SDL's native view/layer. */
  CheckWindow(window, "auto", true);
  CheckWindow(window, "3", false);
  CheckWindow(window, "2", false);
  CheckWindow(window, "auto", false);
  assert(ArSdlMetalDrawable_Configure(window, 1) == 0);
  SDL_DestroyWindow(window);
  SDL_Quit();
  puts("metal_drawable_policy_test: PASS");
  return 0;
#else
  return 77;
#endif
}
