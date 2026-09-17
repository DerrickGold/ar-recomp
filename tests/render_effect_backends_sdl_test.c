#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "crt_post.h"
#include "diorama/diorama_effect_backend.h"
#include "platform/sdl/render_sdl_internal.h"
#include "session_fatal.h"
#include "sim/sim_shadow_effect_backend.h"
#include "sim/sim_cloud_effect_backend.h"

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

static SDL_Surface *CloudImage(ArRenderDevice *device, SDL_Renderer *renderer,
    ArRenderTexture texture, const SimCloudEffectParams *params, bool gpu, float coverage) {
  const int32_t indices[6] = {0, 1, 2, 0, 2, 3};
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Address,
    .address_u = kArRenderTextureAddressMode_Wrap,
    .address_v = kArRenderTextureAddressMode_Wrap,
  };
  const ArRenderVertex2D base[4] = {
    {{0, 0}, {1, 1, 1, coverage}, {-0.12f, 0.04f}},
    {{32, 0}, {1, 1, 1, 0}, {0.87f, 0.17f}},
    {{32, 32}, {1, 1, 1, 0.73f * coverage}, {0.94f, 0.91f}},
    {{0, 32}, {1, 1, 1, coverage}, {-0.06f, 0.85f}},
  };
  CHECK(ArRenderDevice_Clear(device, (ArRenderColorF){0.05f, 0.34f, 0.7f, 1}));
  if (gpu) {
    CHECK(SimCloudEffectBackend_Bind(device, params));
    CHECK(ArRenderDevice_DrawGeometryWithState(device, texture, base, 4, indices, 6, &state));
    CHECK(SimCloudEffectBackend_Unbind(device));
  } else {
    for (int layer = 0; layer < kSimCloudEffectSamples; layer++) {
      ArRenderVertex2D vertices[4];
      memcpy(vertices, base, sizeof(vertices));
      for (int p = 0; p < 4; p++) {
        vertices[p].tex_coord.x = base[p].tex_coord.x * params->samples[layer].scale +
            params->samples[layer].offset_x;
        vertices[p].tex_coord.y = base[p].tex_coord.y * params->samples[layer].scale +
            params->samples[layer].offset_y;
        vertices[p].color.a *= params->samples[layer].opacity;
      }
      CHECK(ArRenderDevice_DrawGeometryWithState(device, texture, vertices, 4, indices, 6, &state));
    }
  }
  SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
  SDL_Surface *rgba = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32) : NULL;
  SDL_DestroySurface(raw);
  CHECK(rgba != NULL);
  return rgba;
}

static void TestSimCloudEffect(ArRenderDevice *device, SDL_Renderer *renderer) {
  SimCloudEffectParams params = {.samples = {
    {4, 0.17f, 0.32f, 1}, {2.7f, 0.68f, 0.81f, 0.85f}, {6.3f, 0.74f, 0.29f, 0.7f},
  }};
  CHECK(SimCloudEffectBackend_IsAvailable(device));
  CHECK(SimCloudEffectBackend_Bind(device, &params));
  CHECK(SimCloudEffectBackend_Unbind(device));
  CHECK(!SimCloudEffectBackend_Bind(device, NULL));
  SimCloudEffectParams invalid = params;
  invalid.samples[2].scale = NAN;
  CHECK(!SimCloudEffectBackend_Bind(device, &invalid));
  CHECK(SimCloudEffectBackend_Unbind(device));
  invalid = params;
  invalid.samples[0].opacity = -1;
  CHECK(!SimCloudEffectBackend_Bind(device, &invalid));
  invalid.samples[0].opacity = 2;
  CHECK(!SimCloudEffectBackend_Bind(device, &invalid));
  invalid = params;
  invalid.samples[1].offset_y = INFINITY;
  CHECK(!SimCloudEffectBackend_Bind(device, &invalid));
  CHECK(SimCloudEffectBackend_Unbind(device));
  ArRenderDevice unsupported = {0};
  CHECK(!SimCloudEffectBackend_IsAvailable(&unsupported));
  CHECK(!SimCloudEffectBackend_Unbind(&unsupported));
  SimCloudEffectBackend_Reset(&unsupported);
  CHECK(SimCloudEffectBackend_IsAvailable(device));
  SimCloudEffectBackend_Reset(device);
  CHECK(SimCloudEffectBackend_IsAvailable(device));

  uint32_t pixels[16 * 16];
  for (int i = 0; i < 16 * 16; i++)
    pixels[i] = ((uint32_t)(i * 43 % 256) << 24) | 0x00f0f5ffu;
  const ArRenderTextureDesc desc = {
    .width = 16, .height = 16, .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Static, .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture texture = ArRenderTexture_Invalid();
  CHECK(ArRenderDevice_CreateTexture(device, &desc, &texture));
  CHECK(ArRenderDevice_UpdateTexture(device, texture, NULL, pixels, 16 * sizeof(uint32_t)));
  for (int phase = 0; phase < 6; phase++) {
    params.samples[0].offset_x += 0.173f;
    params.samples[1].offset_y -= 0.247f;
    const float coverage = (float)phase / 5.0f;
    SDL_Surface *a = CloudImage(device, renderer, texture, &params, false, coverage);
    SDL_Surface *b = CloudImage(device, renderer, texture, &params, true, coverage);
    int maximum = 0;
    if (a && b) {
      for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w * 4; x++) {
          int difference = abs(((uint8_t *)a->pixels)[y * a->pitch + x] -
              ((uint8_t *)b->pixels)[y * b->pitch + x]);
          if (difference > maximum) maximum = difference;
        }
      /* Three 8-bit target writes round between banks; one shader rounds
       * once. Allow that bounded difference, never a changed bank/coverage. */
      CHECK(maximum <= 3);
      if (!phase) CHECK(maximum == 0);
    }
    printf("cloud composition phase=%d max-channel-delta=%d\n", phase, maximum);
    SDL_DestroySurface(a);
    SDL_DestroySurface(b);
  }
  ArRenderDevice_DestroyTexture(device, texture);
  SimCloudEffectBackend_Reset(device);
  CHECK(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "AR_SIM3D_CLOUD_GPU", "0", true));
  CHECK(!SimCloudEffectBackend_IsAvailable(device));
  SimCloudEffectBackend_Reset(device);
  CHECK(SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "AR_SIM3D_CLOUD_GPU"));
  CHECK(SimCloudEffectBackend_IsAvailable(device));
  SimCloudEffectBackend_Reset(device);
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
  TestSimCloudEffect(&device, renderer);
  TestCrtPost(&device);
  /* Reset/recreate other effects first: shader pointer reuse must never select
   * a stale pipeline for the cloud composite. Repeat to cover both orders. */
  TestSimCloudEffect(&device, renderer);

  ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyRenderer(renderer);
  /* The shipping adapter submits through an offscreen renderer, unlike the
   * externally owned renderer above. Exercise warm-up and pixel parity there
   * too, including restoration of its adapter-owned default target. */
  CHECK(ArSdlRenderBackend_CreateForWindow(&device, window, NULL));
  if (ArRenderDevice_IsReady(&device)) {
    renderer = ArSdlRenderBackend_Renderer(&device);
    TestDioramaEffects(&device);
    TestSimShadowEffect(&device);
    TestSimCloudEffect(&device, renderer);
    TestCrtPost(&device);
    TestSimCloudEffect(&device, renderer);
    const char *driver = ArSdlRenderBackend_GpuDriver(&device);
    CHECK(driver && ArSdlRenderBackend_HasGpuDriver(driver));
    ArSdlRenderBackend_Destroy(&device);
  }
  CHECK(!ArSdlRenderBackend_HasGpuDriver(NULL));
  CHECK(!ArSdlRenderBackend_HasGpuDriver("not-a-gpu-driver"));
  CHECK(ArSdlRenderBackend_GpuDriver(&device) == NULL);
  /* A Graphics API choice that cannot start must still produce a working
   * output through SDL's own order, on the same window. */
  CHECK(ArSdlRenderBackend_CreateForWindow(&device, window, "not-a-gpu-driver"));
  if (ArRenderDevice_IsReady(&device)) {
    const char *driver = ArSdlRenderBackend_GpuDriver(&device);
    CHECK(driver && ArSdlRenderBackend_HasGpuDriver(driver));
    TestCrtPost(&device);
    ArSdlRenderBackend_Destroy(&device);
  }
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
