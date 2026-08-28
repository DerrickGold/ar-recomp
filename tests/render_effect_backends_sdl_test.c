#include <SDL3/SDL.h>

#include <stdio.h>

#include "crt_post.h"
#include "diorama/diorama_effect_backend.h"
#include "platform/sdl/render_sdl.h"
#include "session_fatal.h"
#include "sim/sim_shadow_effect_backend.h"

enum { kSkip = 77 };

static int s_failures;
#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s (%s)\n",                   \
            __FILE__, __LINE__, #expression, SDL_GetError());              \
    s_failures++;                                                          \
  }                                                                        \
} while (0)

static SDL_Renderer *CreateGpuRenderer(SDL_Window *window) {
  SDL_PropertiesID properties = SDL_CreateProperties();
  if (!properties) return NULL;
  SDL_SetStringProperty(
      properties, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
  SDL_SetPointerProperty(
      properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
  SDL_SetBooleanProperty(
      properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
  SDL_SetBooleanProperty(
      properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
  SDL_SetBooleanProperty(
      properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
  SDL_Renderer *renderer = SDL_CreateRendererWithProperties(properties);
  SDL_DestroyProperties(properties);
  return renderer;
}

static void TestDioramaEffects(ArRenderDevice *device) {
  CHECK(DioramaEffectBackend_IsAvailable(
      device, kDioramaEffect_Blur));
  CHECK(DioramaEffectBackend_IsAvailable(
      device, kDioramaEffect_RimLight));
  CHECK(DioramaEffectBackend_IsAvailable(
      device, kDioramaEffect_DofEdge));

  const DioramaBlurEffectParams blur = {
    .texel_width = 1.0f / 256.0f,
    .texel_height = 1.0f / 224.0f,
    .radius = 2.0f,
  };
  const DioramaRimLightEffectParams rim = {
    .texel_width = 1.0f / 256.0f,
    .texel_height = 1.0f / 224.0f,
    .strength = 0.33f,
  };
  const DioramaDofEdgeEffectParams dof_edge = {
    .texel_width = 1.0f / 256.0f,
    .texel_height = 1.0f / 224.0f,
    .blur_radius = 1.5f,
    .u_min = 0.1f,
    .u_max = 0.9f,
    .v_min = 0.0f,
    .v_max = 0.875f,
    .edge_feather = 2.0f,
    .lower_content_v_max = 0.0f,
  };
  CHECK(DioramaEffectBackend_BindBlur(device, &blur));
  CHECK(DioramaEffectBackend_Unbind(device));
  CHECK(DioramaEffectBackend_BindRimLight(device, &rim));
  CHECK(DioramaEffectBackend_Unbind(device));
  CHECK(DioramaEffectBackend_BindDofEdge(device, &dof_edge));
  CHECK(DioramaEffectBackend_Unbind(device));

  DioramaEffectBackend_Reset(device);
  CHECK(DioramaEffectBackend_IsAvailable(
      device, kDioramaEffect_Blur));
  CHECK(DioramaEffectBackend_BindBlur(device, &blur));
  CHECK(DioramaEffectBackend_Unbind(device));
  DioramaEffectBackend_Reset(device);
}

static void TestSimShadowEffect(ArRenderDevice *device) {
  const SimShadowBlurEffectParams horizontal = {
    .texel_x = 1.0f / 320.0f,
    .texel_y = 0.0f,
    .radius = 5.0f,
  };
  const SimShadowBlurEffectParams vertical = {
    .texel_x = 0.0f,
    .texel_y = 1.0f / 240.0f,
    .radius = 5.0f,
  };
  CHECK(SimShadowEffectBackend_IsAvailable(device));
  CHECK(SimShadowEffectBackend_BindBlur(device, &horizontal));
  CHECK(SimShadowEffectBackend_Unbind(device));
  CHECK(SimShadowEffectBackend_BindBlur(device, &vertical));
  CHECK(SimShadowEffectBackend_Unbind(device));

  SimShadowEffectBackend_Reset(device);
  CHECK(SimShadowEffectBackend_IsAvailable(device));
  CHECK(SimShadowEffectBackend_BindBlur(device, &horizontal));
  CHECK(SimShadowEffectBackend_Unbind(device));
  SimShadowEffectBackend_Reset(device);
}

static void TestCrtPost(ArRenderDevice *device) {
  const CrtPostConfig config = {
    .enabled = true,
    .curvature = 0.12f,
    .scanline_depth = 0.30f,
    .mask_strength = 0.18f,
    .aberration = 0.10f,
    .bandwidth = 0.25f,
    .vignette = 0.20f,
    .brightness = 1.10f,
  };
  const ArRenderRectI image = {0, 0, 32, 32};

  CHECK(CrtPost_Begin(device, &config));
  CHECK(ArRenderTexture_IsValid(CrtPost_BaseTarget()));
  CHECK(ArRenderDevice_Clear(
      device, (ArRenderColorF){0.1f, 0.2f, 0.3f, 1.0f}));
  ArRenderRectI resolved = CrtPost_End(device, 256, 224, image);
  CHECK(resolved.x == image.x && resolved.y == image.y &&
        resolved.w == image.w && resolved.h == image.h);
  CHECK(!ArRenderTexture_IsValid(CrtPost_BaseTarget()));
  CHECK(!SessionFatal_Requested());

  CrtPost_Shutdown(device);
  CHECK(CrtPost_Begin(device, &config));
  (void)CrtPost_End(device, 256, 224, image);
  CHECK(!SessionFatal_Requested());
  CrtPost_Shutdown(device);
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "render_effect_backends_sdl_test: SKIP - %s\n",
            SDL_GetError());
    return kSkip;
  }
  SDL_Window *window = SDL_CreateWindow(
      "render effect backends test", 32, 32, SDL_WINDOW_HIDDEN);
  SDL_Renderer *renderer = window ? CreateGpuRenderer(window) : NULL;
  if (!window || !renderer) {
    fprintf(stderr,
            "render_effect_backends_sdl_test: SKIP - no GPU renderer (%s)\n",
            SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return kSkip;
  }

  ArRenderDevice device = {0};
  ArSdlRenderBackend backend = {0};
  CHECK(ArSdlRenderBackend_Bind(&device, &backend, renderer));
  TestDioramaEffects(&device);
  TestSimShadowEffect(&device);
  TestCrtPost(&device);

  ArRenderDevice_Reset(&device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "render_effect_backends_sdl_test: %d failure(s)\n",
            s_failures);
    return 1;
  }
  printf("render_effect_backends_sdl_test: OK\n");
  return 0;
}
