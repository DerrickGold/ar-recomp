#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diorama/diorama_frame_generation.h"
#include "present.h"

static int failures;
#define CHECK(expression) do {                                           \
  if (!(expression)) {                                                   \
    fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__,             \
            #expression, SDL_GetError());                                \
    failures++;                                                          \
  }                                                                      \
} while (0)

enum {
  kDisplayWidth = 32,
  kApron = 4,
  kSurfaceWidth = kDisplayWidth + kApron * 2,
  kHeight = 16,
  kSkipNoGpuRenderer = 77,
};

static bool RequireProductionGpuRenderer(void) {
  const char *value = getenv("AR_REQUIRE_GPU_RENDERER");
  return value && value[0] && strcmp(value, "0") != 0;
}

static uint32_t PatternPixel(int x, int y) {
  uint32_t hash = (uint32_t)(x + 1) * 73856093u ^
      (uint32_t)(y + 1) * 19349663u;
  hash ^= hash >> 13;
  return 0xff000000u | (hash & 0x00ffffffu);
}

static void FillFrame(uint32_t *pixels, int shift, bool change_marker) {
  for (int y = 0; y < kHeight; y++) {
    uint32_t *row = &pixels[y * kSurfaceWidth];
    for (int x = 0; x < kSurfaceWidth; x++) {
      if (x < kApron || x >= kApron + kDisplayWidth) {
        row[x] = 0xffff00ffu;
        continue;
      }
      const int source_x = x - kApron - shift;
      row[x] = source_x >= 0 && source_x < kDisplayWidth
          ? PatternPixel(source_x, y) : 0;
    }
  }
  for (int y = 7; y <= 9; y++) {
    for (int x = 8; x <= 11; x++) {
      const int shifted_x = x + (change_marker ? 2 : 0);
      pixels[y * kSurfaceWidth + kApron + shifted_x] =
          change_marker ? 0xff0000ffu : 0xffff0000u;
    }
  }
  }

/* Exercise the renderer implementation used by release builds. A GPU-less
 * test host falls back to SDL's default renderer so the ROM-free suite still
 * covers state restoration and synthesis math instead of becoming a skip. */
static SDL_Renderer *CreateTestRenderer(SDL_Window *window) {
  const char *override = getenv("AR_TEST_RENDERER");
  if (override && override[0])
    return SDL_CreateRenderer(window, override);
  SDL_Renderer *renderer = NULL;
  SDL_PropertiesID properties = SDL_CreateProperties();
  if (properties) {
    SDL_SetStringProperty(properties,
        SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
    SDL_SetPointerProperty(properties,
        SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetBooleanProperty(properties,
        SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties,
        SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(properties,
        SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
    renderer = SDL_CreateRendererWithProperties(properties);
    SDL_DestroyProperties(properties);
  }
  if (!renderer) {
    if (RequireProductionGpuRenderer())
      return NULL;
    fprintf(stderr, "GPU renderer unavailable; testing SDL fallback: %s\n",
            SDL_GetError());
    renderer = SDL_CreateRenderer(window, NULL);
  }
  return renderer;
}

int main(void) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Texture *current = NULL;
  SDL_Texture *scene = NULL;
  DioramaPlaneCaptureRegion region;
  CHECK(DioramaPlaneCaptureRegion_Resolve(
      kDioramaPlane_Backdrop, kSurfaceWidth, kHeight, kApron, &region));
  CHECK(region.x == kApron && region.width == kDisplayWidth &&
        region.height == kHeight);
  CHECK(DioramaPlaneCaptureRegion_Resolve(
      SR_PPU_OVERLAY_OBJ, kSurfaceWidth, kHeight, kApron, &region));
  CHECK(region.x == 0 && region.width == kSurfaceWidth &&
        region.height == kHeight);
  if (!SDL_Init(SDL_INIT_VIDEO) && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    return kSkipNoGpuRenderer;
  }
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) != 0);
  window = SDL_CreateWindow("frame generation test", 64, 64, 0);
  if (!window && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  CHECK(window != NULL);
  renderer = window ? CreateTestRenderer(window) : NULL;
  if (!renderer && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  CHECK(renderer != NULL);
  if (!renderer) goto done;
  SDL_GPUDevice *device = SDL_GetGPURendererDevice(renderer);
  printf("diorama frame-generation renderer=%s gpu=%s\n",
         SDL_GetRendererName(renderer),
         device ? SDL_GetGPUDeviceDriver(device) : "fallback");

  current = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kFrameSlotLayerTextureWidth, kFrameSlotLayerTextureHeight);
  scene = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 64, 64);
  CHECK(current != NULL && scene != NULL);
  if (!current || !scene) goto done;

  static uint32_t previous_pixels[kSurfaceWidth * kHeight];
  static uint32_t current_pixels[kSurfaceWidth * kHeight];
  FillFrame(previous_pixels, 0, false);
  FillFrame(current_pixels, 2, true);
  CHECK(current_pixels[8 * kSurfaceWidth + kApron + 11] == 0xff0000ffu);
  const uint8_t *planes[kDioramaPlane_Count] = {0};
  size_t plane_pitches[kDioramaPlane_Count] = {0};
  planes[kDioramaPlane_Backdrop] = (uint8_t *)previous_pixels;
  plane_pitches[kDioramaPlane_Backdrop] =
      kSurfaceWidth * sizeof(uint32_t);

  static FrameSlot slot;
  slot.diorama_active = true;
  slot.interp_setting_enabled = true;
  slot.snes_width = kDisplayWidth;
  slot.snes_height = kHeight;
  slot.obj_apron = kApron;
  slot.capture_ticks = 1;
  slot.bg_mode = 1;
  slot.diorama_plane_request_mask = 1u << kDioramaPlane_Backdrop;
  slot.diorama_plane_content_mask = slot.diorama_plane_request_mask;
  slot.timestamp_ns = 1000000;
  DioramaFrameGeneration_Capture(
      renderer, &slot, planes, plane_pitches,
      1u << kDioramaPlane_Backdrop);

  planes[kDioramaPlane_Backdrop] = (uint8_t *)current_pixels;
  slot.timestamp_ns += 16666667;
  DioramaFrameGeneration_Capture(
      renderer, &slot, planes, plane_pitches,
      1u << kDioramaPlane_Backdrop);
  SDL_Rect endpoint_rect = {kApron, 0, kDisplayWidth, kHeight};
  CHECK(SDL_UpdateTexture(
      current, &endpoint_rect, &current_pixels[kApron],
      kSurfaceWidth * sizeof(uint32_t)));

  CHECK(SDL_SetRenderTarget(renderer, scene));
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, 32, 32, SDL_LOGICAL_PRESENTATION_STRETCH));
  const SDL_Rect viewport = {1, 2, 30, 28};
  const SDL_Rect clip = {3, 4, 20, 18};
  CHECK(SDL_SetRenderViewport(renderer, &viewport));
  CHECK(SDL_SetRenderClipRect(renderer, &clip));
  CHECK(SDL_SetRenderDrawColor(renderer, 11, 22, 33, 44));

  SDL_Texture *raw[kDioramaPlane_Count] = {0};
  SDL_Texture *resolved[kDioramaPlane_Count] = {0};
  raw[kDioramaPlane_Backdrop] = current;
  const uint32_t generated = DioramaFrameGeneration_Prepare(
      renderer, &slot, 0.5f, raw,
      1u << kDioramaPlane_Backdrop, resolved);
  CHECK(generated == (1u << kDioramaPlane_Backdrop));
  CHECK(resolved[kDioramaPlane_Backdrop] != current);

  CHECK(SDL_GetRenderTarget(renderer) == scene);
  int logical_width = 0, logical_height = 0;
  SDL_RendererLogicalPresentation logical_mode =
      SDL_LOGICAL_PRESENTATION_DISABLED;
  CHECK(SDL_GetRenderLogicalPresentation(
      renderer, &logical_width, &logical_height, &logical_mode));
  CHECK(logical_width == 32 && logical_height == 32 &&
        logical_mode == SDL_LOGICAL_PRESENTATION_STRETCH);
  SDL_Rect restored;
  CHECK(SDL_GetRenderViewport(renderer, &restored));
  CHECK(SDL_RectsEqual(&viewport, &restored));
  CHECK(SDL_GetRenderClipRect(renderer, &restored));
  CHECK(SDL_RectsEqual(&clip, &restored));
  Uint8 r = 0, g = 0, b = 0, a = 0;
  CHECK(SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a));
  CHECK(r == 11 && g == 22 && b == 33 && a == 44);

  /* At phase 0.5 the nearer (current) endpoint warps back by one pixel. The
   * marker therefore lands at x=10 as exact opaque blue. Drawing/blending the
   * farther red endpoint would turn this purple and recreate sprite ghosting. */
  CHECK(SDL_SetRenderTarget(renderer, resolved[kDioramaPlane_Backdrop]));
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
  CHECK(SDL_SetRenderViewport(renderer, NULL));
  CHECK(SDL_SetRenderClipRect(renderer, NULL));
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, &endpoint_rect);
  CHECK(readback != NULL);
  SDL_Surface *argb = readback
      ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
  CHECK(argb != NULL);
  if (argb) {
    const uint32_t *row = (const uint32_t *)(
        (const uint8_t *)argb->pixels + (size_t)8 * argb->pitch);
    const uint32_t marker = row[10];
    if ((marker >> 24) < 250u || ((marker >> 16) & 0xffu) > 4u ||
        (marker & 0xffu) < 250u)
      fprintf(stderr, "generated marker=%08x\n", marker);
    CHECK((marker >> 24) >= 250u);
    CHECK(((marker >> 16) & 0xffu) <= 4u);
    CHECK((marker & 0xffu) >= 250u);
  }
  SDL_DestroySurface(argb);
  SDL_DestroySurface(readback);

  /* The non-OBJ backdrop excludes the apron entirely. Sentinel magenta in the
   * source apron must therefore leave the fixed compositor padding clear. */
  static const SDL_Rect apron_rects[] = {
    {0, 0, kApron, kHeight},
    {kApron + kDisplayWidth, 0, kApron, kHeight},
  };
  for (size_t side = 0;
       side < sizeof(apron_rects) / sizeof(apron_rects[0]); side++) {
    readback = SDL_RenderReadPixels(renderer, &apron_rects[side]);
    argb = readback
        ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(argb != NULL);
    if (argb) {
      const uint32_t *row = argb->pixels;
      CHECK((row[0] >> 24) == 0);
    }
    SDL_DestroySurface(argb);
    SDL_DestroySurface(readback);
  }

  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      renderer, &slot, kPresentationFrameGenerationPhaseNone, raw,
      1u << kDioramaPlane_Backdrop, resolved) == 0);
  CHECK(resolved[kDioramaPlane_Backdrop] == current);

  /* A synchronized but byte-identical raw endpoint is already authoritative.
   * It must not create a redundant private pair or generated plane. */
  slot.timestamp_ns += 16666667;
  DioramaFrameGeneration_Capture(
      renderer, &slot, planes, plane_pitches, 0);
  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      renderer, &slot, 0.5f, raw,
      1u << kDioramaPlane_Backdrop, resolved) == 0);
  CHECK(resolved[kDioramaPlane_Backdrop] == current);

  /* A room-key discontinuity seeds a new endpoint but never blends across the
   * transition. */
  slot.timestamp_ns += 16666667;
  slot.diorama_map_number++;
  DioramaFrameGeneration_Capture(
      renderer, &slot, planes, plane_pitches, 0);
  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      renderer, &slot, 0.5f, raw,
      1u << kDioramaPlane_Backdrop, resolved) == 0);
  CHECK(resolved[kDioramaPlane_Backdrop] == current);

done:
  DioramaFrameGeneration_Shutdown();
  SDL_DestroyTexture(scene);
  SDL_DestroyTexture(current);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("diorama frame-generation test: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
