#include <SDL3/SDL.h>

#include <stdint.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/render_sdl_internal.h"
#include "sim/sim3d_depth_pass.h"
#include "present_world_nav_geometry.h"

enum {
  kTestWidth = 32,
  kTestHeight = 16,
  kSkipNoGpuRenderer = 77,
};

static int failures;
static uint64_t geometry_upload_bytes, draw_calls;
#define CHECK(expression) do {                                           \
  if (!(expression)) {                                                   \
    fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__,             \
            #expression, SDL_GetError());                                \
    failures++;                                                          \
  }                                                                      \
} while (0)

/* sim3d_depth_pass.c reports production work to this dormant profiler hook.
 * The integration test deliberately links no global profiling state. */
void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  if (vertices && indices) ++draw_calls;
}
void Sim3DPerformance_AddGeometryUpload(uint64_t bytes) { geometry_upload_bytes += bytes; }

static SDL_Renderer *CreateProductionRenderer(SDL_Window *window) {
  SDL_PropertiesID properties = SDL_CreateProperties();
  if (!properties) return NULL;
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
  SDL_Renderer *renderer = SDL_CreateRendererWithProperties(properties);
  SDL_DestroyProperties(properties);
  return renderer;
}

static void MakeRect(Sim3DDepthVertex vertices[4],
                     float x0, float y0, float x1, float y1,
                     float depth, ArRenderColorF color) {
  const Sim3DDepthVertex resolved[4] = {
    {x0, y0, depth, color, {0.0f, 0.0f}},
    {x1, y0, depth, color, {1.0f, 0.0f}},
    {x1, y1, depth, color, {1.0f, 1.0f}},
    {x0, y1, depth, color, {0.0f, 1.0f}},
  };
  memcpy(vertices, resolved, sizeof(resolved));
}

static bool AppendRect(Sim3DDepthPassLayer layer,
                       float x0, float y0, float x1, float y1,
                       float depth, ArRenderColorF color) {
  Sim3DDepthVertex vertices[4];
  MakeRect(vertices, x0, y0, x1, y1, depth, color);
  return Sim3DDepthPass_AppendQuad(layer, vertices);
}

static uint32_t ReadArgb(const SDL_Surface *surface, int x, int y) {
  const uint8_t *row =
      (const uint8_t *)surface->pixels + (size_t)y * surface->pitch;
  uint32_t pixel;
  memcpy(&pixel, row + (size_t)x * sizeof(pixel), sizeof(pixel));
  return pixel;
}

static SDL_Surface *ReadPassWithShadow(ArRenderDevice *device, SDL_Renderer *renderer,
    ArRenderTexture shadow) {
  SDL_Texture *texture = ArSdlRenderBackend_UnwrapTexture(
      Sim3DDepthPass_Submit(device, shadow));
  if (!texture || !SDL_SetRenderTarget(renderer, texture)) return NULL;
  SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
  SDL_Surface *rgba = surface ? SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32) : NULL;
  SDL_DestroySurface(surface);
  CHECK(ArRenderDevice_SetRenderTarget(device, ArRenderTexture_Invalid()));
  return rgba;
}

static SDL_Surface *ReadPass(ArRenderDevice *device, SDL_Renderer *renderer) {
  return ReadPassWithShadow(device, renderer, ArRenderTexture_Invalid());
}

static void TestOrderedSubmission(SDL_Window *window) {
  SDL_unsetenv_unsafe("AR_SDL_GPU_ORDERED");
  ArRenderDevice device = {0};
  CHECK(ArSdlRenderBackend_CreateForWindow(&device, window));
  if (!ArRenderDevice_IsReady(&device)) return;
  CHECK(((ArSdlRenderBackend *)device.context)->output_window == window);
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(&device);
  bool vsync = true;
  CHECK(ArSdlRenderBackend_SetVSync(&device, 0, &vsync) && !vsync);
  CHECK(ArSdlRenderBackend_SetVSync(&device, 1, &vsync) && vsync);
  CHECK(ArSdlRenderBackend_SetVSync(&device, 0, &vsync) && !vsync);
  bool changed = false;
  CHECK(ArSdlRenderBackend_SetAllowedFramesInFlight(&device, 2, &changed) && changed);
  ArRenderTargetState saved;
  CHECK(device.ops->capture_render_target_state(device.context, &saved));
  CHECK(!ArRenderTexture_IsValid(saved.target));
  const ArRenderTextureDesc desc = {.width = 2, .height = 2,
    .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Alpha};
  ArRenderTexture shadow = ArRenderTexture_Invalid();
  CHECK(ArRenderDevice_CreateTexture(&device, &desc, &shadow));
  const ArRenderRectI viewport = {3, 2, 20, 10}, clip = {1, 1, 12, 8};
  CHECK(ArRenderDevice_SetViewport(&device, &viewport));
  CHECK(ArRenderDevice_SetClipRect(&device, &clip));
  ArRenderTargetState scoped, restored;
  CHECK(ArRenderDevice_BeginTarget(&device, shadow, &scoped) == kArRenderTargetBegin_Ready);
  CHECK(!ArRenderTexture_IsValid(scoped.target));
  CHECK(ArRenderDevice_EndTarget(&device, &scoped));
  CHECK(device.ops->capture_render_target_state(device.context, &restored));
  CHECK(!ArRenderTexture_IsValid(restored.target));
  CHECK(restored.viewport_set && restored.clip_enabled);
  CHECK(restored.viewport.x == viewport.x && restored.viewport.y == viewport.y &&
      restored.viewport.w == viewport.w && restored.viewport.h == viewport.h);
  CHECK(restored.clip.x == clip.x && restored.clip.y == clip.y &&
      restored.clip.w == clip.w && restored.clip.h == clip.h);
  CHECK(device.ops->restore_render_target_state(device.context, &saved));
  CHECK(Sim3DDepthPass_Require(&device));
  Sim3DDepthMesh *mesh = NULL;
  for (int frame = 0; frame < 48; ++frame) {
    const bool red = (frame & 1) == 0;
    CHECK(ArRenderDevice_SetRenderTarget(&device, shadow));
    CHECK(ArRenderDevice_Clear(&device, (ArRenderColorF){red, !red, 0, 1}));
    CHECK(device.ops->restore_render_target_state(device.context, &saved));
    CHECK(Sim3DDepthPass_Begin(&device, 32, 16, kArRenderFilter_Nearest));
    if (!mesh) {
      mesh = Sim3DDepthPass_CreateGeometryMesh(); CHECK(mesh);
      Sim3DDepthVertex vertices[4];
      MakeRect(vertices, 0, 0, 32, 16, .5f, (ArRenderColorF){1,1,1,1});
      CHECK(Sim3DDepthPass_UpdateGeometryMesh(mesh, vertices, 1));
    }
    CHECK(Sim3DDepthPass_AppendGeometryMesh(kSim3DDepthPass_ShadowReceiver, mesh));
    /* No readback/fence/window present between producing the mask and the
     * custom consumer. Alternating colors catches even one-frame latency. */
    SDL_Surface *actual = ReadPassWithShadow(&device, renderer, shadow);
    CHECK(actual);
    if (actual) {
      const uint8_t *pixel = (uint8_t *)actual->pixels + 8 * actual->pitch + 16 * 4;
      CHECK(pixel[0] == (red ? 255 : 0) && pixel[1] == (red ? 0 : 255) && pixel[3] == 255);
      SDL_DestroySurface(actual);
    }
    /* The logical default target stays usable after a custom pass, and only
     * this terminal call owns the actual swapchain present. */
    CHECK(ArRenderDevice_Clear(&device, (ArRenderColorF){0,0,0,1}));
    CHECK(ArRenderDevice_Present(&device));
  }
  const ArRenderTextureDesc composite_desc = {.width = 64, .height = 16,
    .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
  ArRenderTexture composite = ArRenderTexture_Invalid();
  CHECK(ArRenderDevice_CreateTexture(&device, &composite_desc, &composite));
  for (int frame = 0; frame < 4; ++frame) {
    /* Two custom passes share the depth target with a queued SDL consumer
     * between them. No readback/present splits this producer/consumer chain. */
    for (int side = 0; side < 2; ++side) {
      CHECK(ArRenderDevice_SetRenderTarget(&device, shadow));
      CHECK(ArRenderDevice_Clear(&device, (ArRenderColorF){side == 0, side == 1, 0, 1}));
      CHECK(ArRenderDevice_SetRenderTarget(&device, composite));
      CHECK(Sim3DDepthPass_Begin(&device, 32, 16, kArRenderFilter_Nearest));
      CHECK(Sim3DDepthPass_AppendGeometryMesh(kSim3DDepthPass_ShadowReceiver, mesh));
      ArRenderTexture texture = Sim3DDepthPass_Submit(&device, shadow);
      const ArRenderRectF rect = {(float)side * 32, 0, 32, 16};
      CHECK(ArRenderDevice_DrawTexture(&device, texture, NULL, &rect));
    }
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *pixels = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32) : NULL;
    CHECK(pixels);
    if (pixels) for (int side = 0; side < 2; ++side) {
      const uint8_t *pixel = (uint8_t *)pixels->pixels + 8 * pixels->pitch + (16 + side * 32) * 4;
      CHECK(pixel[0] == (side ? 0 : 255) && pixel[1] == (side ? 255 : 0));
    }
    SDL_DestroySurface(raw); SDL_DestroySurface(pixels);
  }
  CHECK(ArRenderDevice_SetRenderTarget(&device, ArRenderTexture_Invalid()));
  ArRenderDevice_DestroyTexture(&device, composite);
  Sim3DDepthPass_DestroyMesh(mesh);
  Sim3DDepthPass_Reset(&device);
  ArRenderDevice_DestroyTexture(&device, shadow);
  CHECK(ArRenderDevice_SetRenderTarget(&device, ArRenderTexture_Invalid()));
  CHECK(SDL_SetWindowSize(window, 96, 80));
  SDL_SyncWindow(window);
  int width, height, expected_width, expected_height;
  CHECK(SDL_GetWindowSizeInPixels(window, &expected_width, &expected_height));
  CHECK(ArRenderDevice_GetOutputSize(&device, &width, &height));
  CHECK(width == expected_width && height == expected_height);
  CHECK(device.ops->restore_render_target_state(device.context, &saved));
  CHECK(ArRenderDevice_Clear(&device, (ArRenderColorF){0,0,0,1}));
  CHECK(ArRenderDevice_Present(&device));
  ArSdlRenderBackend_Destroy(&device);
  CHECK(!ArRenderDevice_IsReady(&device));
  /* Only an explicit diagnostic zero selects the former window renderer. */
  CHECK(SDL_setenv_unsafe("AR_SDL_GPU_ORDERED", "0", 1) == 0);
  CHECK(ArSdlRenderBackend_CreateForWindow(&device, window));
  SDL_unsetenv_unsafe("AR_SDL_GPU_ORDERED");
  CHECK(ArRenderDevice_IsReady(&device));
  if (ArRenderDevice_IsReady(&device)) {
    CHECK(((ArSdlRenderBackend *)device.context)->output_window == NULL);
    ArSdlRenderBackend_Destroy(&device);
  }
}

static void TestGroupedTownLayers(ArRenderDevice *device, SDL_Renderer *renderer) {
  Sim3DDepthPass_Reset(device);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *mesh = Sim3DDepthPass_CreateGeometryMesh();
  CHECK(mesh);
  if (!mesh) return;
  const ArRenderTextureDesc desc = {
    .width = 2, .height = 2, .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target, .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture shadow = ArRenderTexture_Invalid();
  CHECK(ArRenderDevice_CreateTexture(device, &desc, &shadow));
  const Sim3DDepthPassLayer layers[] = {kSim3DDepthPass_DepthOccluder,
    kSim3DDepthPass_Solid, kSim3DDepthPass_Mountain, kSim3DDepthPass_ShadowReceiver};
  Sim3DDepthGeometryRange ranges[4] = {{0}}, untouched[4] = {{0}};
  CHECK(!Sim3DDepthPass_CaptureGeometryLayers(mesh, layers, 4, ranges)); /* all empty */
  CHECK(!memcmp(ranges, untouched, sizeof(ranges)));
  Sim3DDepthVertex source[12];
  MakeRect(source, 0, 0, 16, 16, .25f, (ArRenderColorF){1,1,1,1});
  MakeRect(source + 4, 0, 0, 32, 16, .75f, (ArRenderColorF){.3f,.8f,.2f,1});
  MakeRect(source + 8, 0, 0, 32, 16, .6f, (ArRenderColorF){1,1,1,.5f});
  SDL_Surface *previous = NULL;
  for (int phase = 0; phase < 2; ++phase) {
    const uint32_t texels[] = {phase ? 0xFF4010FF : 0xFFFF3010,
        0xFF102030, 0x804080FF, phase ? 0xFF808010 : 0xFF208040};
    /* Production shadows are renderer-produced targets. Publish the producer
     * before comparing retained versus ordinary consumers: SDL 3.4's Flush
     * records but does not submit the renderer's GPU command buffer. This
     * test isolates geometry retention, not same-frame renderer interop. */
    CHECK(SDL_SetRenderTarget(renderer, ArSdlRenderBackend_UnwrapTexture(shadow)));
    CHECK(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE));
    for (int i = 0; i < 4; ++i) {
      CHECK(SDL_SetRenderDrawColor(renderer, (texels[i] >> 16) & 255,
          (texels[i] >> 8) & 255, texels[i] & 255, texels[i] >> 24));
      const SDL_FRect rect = {(float)(i % 2), (float)(i / 2), 1, 1};
      CHECK(SDL_RenderFillRect(renderer, &rect));
    }
    CHECK(SDL_SetRenderTarget(renderer, NULL));
    CHECK(SDL_RenderPresent(renderer));
    SDL_Surface *reference = NULL;
    for (int frame = 0; frame < 3; ++frame) {
      CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
      if (!frame) {
        CHECK(Sim3DDepthPass_AppendQuad(layers[0], source));
        CHECK(Sim3DDepthPass_AppendQuad(layers[1], source + 4));
        CHECK(Sim3DDepthPass_AppendQuad(layers[3], source + 8));
        if (!phase) {
          const Sim3DDepthPassLayer duplicate[] = {layers[0], layers[0]};
          CHECK(!Sim3DDepthPass_CaptureGeometryLayers(mesh, duplicate, 2, ranges));
          CHECK(!memcmp(ranges, untouched, sizeof(ranges)));
          CHECK(Sim3DDepthPass_CaptureGeometryLayers(mesh, layers, 4, ranges));
          CHECK(ranges[0].first_quad == 0 && ranges[1].first_quad == 1 &&
              ranges[2].quad_count == 0 && ranges[3].first_quad == 2);
        }
      } else {
        Sim3DDepthGeometryRange invalid[4];
        memcpy(invalid, ranges, sizeof(invalid));
        invalid[3].first_quad = SIZE_MAX;
        CHECK(!Sim3DDepthPass_AppendGeometryRanges(mesh, invalid, 4));
        invalid[3] = (Sim3DDepthGeometryRange){kSim3DDepthPass_Effect, 0, 1};
        CHECK(!Sim3DDepthPass_AppendGeometryRanges(mesh, invalid, 4));
        CHECK(Sim3DDepthPass_AppendGeometryRanges(mesh, ranges, 4));
        CHECK(!Sim3DDepthPass_CaptureGeometryLayers(mesh, layers, 4, ranges));
      }
      /* Ordinary tails must keep the same invisible/depth/blend policy when
       * interleaved with retained spans. Effects behind a shadow receiver but
       * in front of the solid also verify that shadows never write depth. */
      CHECK(AppendRect(layers[0], 0, 14, 32, 16, .1f, (ArRenderColorF){1,1,1,1}));
      CHECK(AppendRect(layers[3], 22, 4, 28, 12, .55f, (ArRenderColorF){.5f,1,1,.3f}));
      CHECK(AppendRect(kSim3DDepthPass_Effect, 18, 2, 20, 14, .7f,
          (ArRenderColorF){1,0,0,1}));
      const uint64_t before_draws = draw_calls, before_upload = geometry_upload_bytes;
      SDL_Surface *actual = ReadPassWithShadow(device, renderer, shadow);
      CHECK(actual && draw_calls - before_draws == (frame ? 6 : 4));
      if (frame == 2) CHECK(geometry_upload_bytes - before_upload == 3 * 4 * 40);
      if (actual) {
        const uint8_t *left = (uint8_t *)actual->pixels + 8 * actual->pitch + 8 * 4;
        const uint8_t *effect = (uint8_t *)actual->pixels + 8 * actual->pitch + 19 * 4;
        CHECK(left[3] == 0 && effect[0] == 255 && effect[1] == 0 && effect[3] == 255);
      }
      if (!frame) reference = actual;
      else {
        if (actual && reference) for (int y = 0; y < 16; ++y)
          CHECK(!memcmp((uint8_t *)actual->pixels + y * actual->pitch,
              (uint8_t *)reference->pixels + y * reference->pitch, 32 * 4));
        SDL_DestroySurface(actual);
      }
    }
    if (phase && previous && reference) {
      bool changed = false;
      for (int y = 0; y < 16; ++y) changed |= memcmp(
          (uint8_t *)previous->pixels + y * previous->pitch,
          (uint8_t *)reference->pixels + y * reference->pitch, 32 * 4) != 0;
      CHECK(changed); /* Cached receivers sample the current mask. */
    }
    SDL_DestroySurface(previous); previous = reference;
  }
  SDL_DestroySurface(previous);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  /* With only two slots left, a three-range append must queue NONE of them. */
  for (int i = 0; i < 62; ++i)
    CHECK(Sim3DDepthPass_AppendGeometryMeshRange(layers[0], mesh, 0, 1));
  CHECK(!Sim3DDepthPass_AppendGeometryRanges(mesh, ranges, 4));
  CHECK(Sim3DDepthPass_AppendGeometryMeshRange(layers[0], mesh, 0, 1));
  CHECK(Sim3DDepthPass_AppendGeometryMeshRange(layers[0], mesh, 0, 1));
  SDL_Surface *budget = ReadPass(device, renderer);
  CHECK(budget); SDL_DestroySurface(budget);
  Sim3DDepthPass_Reset(device);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  CHECK(!Sim3DDepthPass_AppendGeometryRanges(mesh, ranges, 4));
  Sim3DDepthPass_DestroyMesh(mesh);
  ArRenderDevice_DestroyTexture(device, shadow);
  Sim3DDepthPass_Reset(device);
}

static void TestIndependentRetainedBudgets(ArRenderDevice *device, SDL_Renderer *renderer) {
  Sim3DDepthPass_Reset(device);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *opaque[32] = {0}, *effects[32] = {0};
  unsigned opaque_count = 0, effect_count = 0;
  while (opaque_count < 32 && (opaque[opaque_count] = Sim3DDepthPass_CreateGeometryMesh()))
    ++opaque_count;
  while (effect_count < 32 && (effects[effect_count] = Sim3DDepthPass_CreateMesh()))
    ++effect_count;
  CHECK(opaque_count == 4 && effect_count == 16);
  if (!opaque_count || !effect_count) goto cleanup;
  Sim3DDepthVertex solid[4];
  MakeRect(solid, 0, 0, 32, 16, .5f, (ArRenderColorF){.2f,.7f,.4f,1});
  const Sim3DDepthPosition positions[4] = {{8,2,.4f}, {24,2,.4f}, {24,14,.4f}, {8,14,.4f}};
  const ArRenderPointF uv[4] = {{-1,-1}, {-1,-1}, {-1,-1}, {-1,-1}};
  CHECK(Sim3DDepthPass_UpdateGeometryMesh(opaque[0], solid, 1));
  CHECK(Sim3DDepthPass_UpdateMesh(effects[0], positions, 1));
  SDL_Surface *reference = NULL;
  for (int order = 0; order < 2; ++order) {
    CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
    unsigned accepted[2] = {0};
    const uint64_t before = draw_calls;
    for (int at = 0; at < 2; ++at) {
      const int effect = at ^ order;
      bool ok;
      do {
        ok = effect ? Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_Effect,
            effects[0], uv, 1, (ArRenderColorF){.8f,.1f,.6f,.03f})
            : Sim3DDepthPass_AppendGeometryMesh(kSim3DDepthPass_Solid, opaque[0]);
        if (ok) ++accepted[effect];
      } while (ok && accepted[effect] < 1024);
    }
    CHECK(accepted[0] == 64 && accepted[1] == 64);
    /* Exhaustion leaves the ordinary opaque fallback usable and all effect
     * commands intact, irrespective of which domain was filled first. */
    CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, solid));
    SDL_Surface *actual = ReadPass(device, renderer);
    CHECK(actual && draw_calls - before == 129);
    if (!order) reference = actual;
    else {
      if (actual && reference) for (int y = 0; y < 16; ++y)
        CHECK(!memcmp((uint8_t *)actual->pixels + y * actual->pitch,
            (uint8_t *)reference->pixels + y * reference->pitch, 32 * 4));
      SDL_DestroySurface(actual);
    }
  }
  SDL_DestroySurface(reference);
cleanup:
  for (unsigned i = 0; i < opaque_count; ++i) Sim3DDepthPass_DestroyMesh(opaque[i]);
  for (unsigned i = 0; i < effect_count; ++i) Sim3DDepthPass_DestroyMesh(effects[i]);
  Sim3DDepthPass_Reset(device);
}

static void TestGeometryInFlight(ArRenderDevice *device, SDL_Renderer *renderer) {
  /* Most image tests drain the GPU every frame. Exercise transfer/buffer
   * cycling and captured publications with many commands between readbacks. */
  Sim3DDepthPass_Reset(device);
  Sim3DDepthMesh *mesh = NULL;
  const ArRenderRectI region = {0, 0, 2, 2};
  const ArRenderColorF white = {1,1,1,1};
  for (int frame = 0; frame < 192; ++frame) {
    const uint32_t texels[4] = {0xFF8080FFu + (uint32_t)(frame * 256),
        0xFFFF8040, 0xFF40FF80, 0xFF8040FF};
    CHECK(Sim3DDepthPass_UploadAtlasRegions(device, kSim3DDepthPass_Ground,
        texels, 2, 2, 8, &region, 1));
    CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
    if (!mesh) mesh = Sim3DDepthPass_CreateGeometryMesh();
    CHECK(mesh);
    Sim3DDepthVertex source[4];
    MakeRect(source, frame % 8, 1, 24 + frame % 8, 15, .5f, white);
    if (frame % 3 == 0) {
      CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Ground, source));
      CHECK(Sim3DDepthPass_CaptureGeometryMesh(kSim3DDepthPass_Ground, mesh));
    } else {
      if (frame % 3 == 1) CHECK(Sim3DDepthPass_UpdateGeometryMesh(mesh, source, 1));
      CHECK(Sim3DDepthPass_AppendGeometryMesh(kSim3DDepthPass_Ground, mesh));
    }
    if (frame % 16 != 15) {
      CHECK(ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
      continue;
    }
    SDL_Surface *retained = ReadPass(device, renderer);
    CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
    const int source_frame = frame % 3 == 2 ? frame - 1 : frame;
    MakeRect(source, source_frame % 8, 1, 24 + source_frame % 8, 15, .5f, white);
    CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Ground, source));
    SDL_Surface *ordinary = ReadPass(device, renderer);
    CHECK(ordinary && retained);
    if (ordinary && retained) for (int y = 0; y < 16; ++y)
      CHECK(!memcmp((uint8_t *)ordinary->pixels + y * ordinary->pitch,
          (uint8_t *)retained->pixels + y * retained->pitch, 32 * 4));
    SDL_DestroySurface(ordinary); SDL_DestroySurface(retained);
  }
  Sim3DDepthPass_DestroyMesh(mesh);
  Sim3DDepthPass_Reset(device);
}

static void TestCapturedGround(ArRenderDevice *device, SDL_Renderer *renderer) {
  Sim3DDepthPass_Reset(device);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *mesh = Sim3DDepthPass_CreateGeometryMesh();
  Sim3DDepthMesh *other = Sim3DDepthPass_CreateGeometryMesh();
  CHECK(mesh && other);
  if (!mesh || !other) {
    Sim3DDepthPass_DestroyMesh(mesh); Sim3DDepthPass_DestroyMesh(other); return;
  }
  CHECK(!Sim3DDepthPass_CaptureGeometryMesh(kSim3DDepthPass_Ground, mesh)); /* empty */
  Sim3DDepthVertex source[8];
  MakeRect(source, 1, 1, 31, 15, .6f, (ArRenderColorF){1,1,1,1});
  MakeRect(source + 4, 8, 3, 24, 13, .4f, (ArRenderColorF){.5f,1,.75f,1});
  SDL_Surface *previous = NULL;
  const ArRenderRectI full = {0, 0, 2, 2};
  for (int phase = 0; phase < 2; ++phase) {
    const uint32_t texels[4] = {phase ? 0xFF80FF40 : 0xFFFF8040,
        0xFF4080FF, 0xFF40FF80, phase ? 0xFFFF4080 : 0xFF8040FF};
    CHECK(Sim3DDepthPass_UploadAtlasRegions(device, kSim3DDepthPass_Ground,
        texels, 2, 2, 2 * sizeof(uint32_t), &full, 1));
    SDL_Surface *reference = NULL;
    for (int frame = 0; frame < 3; ++frame) {
      CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
      const uint64_t before_upload = geometry_upload_bytes, before_draws = draw_calls;
      if (!frame) {
        CHECK(Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Ground, source, 2));
        if (!phase) CHECK(Sim3DDepthPass_CaptureGeometryMesh(kSim3DDepthPass_Ground, mesh));
      } else {
        CHECK(Sim3DDepthPass_AppendGeometryMesh(kSim3DDepthPass_Ground, mesh));
        CHECK(!Sim3DDepthPass_CaptureGeometryMesh(kSim3DDepthPass_Ground, mesh)); /* queued */
        CHECK(!Sim3DDepthPass_CaptureGeometryMesh(kSim3DDepthPass_Ground, other)); /* sampled */
      }
      CHECK(!Sim3DDepthPass_AppendGeometryMesh(kSim3DDepthPass_Cloud, mesh));
      CHECK(!Sim3DDepthPass_CaptureGeometryMesh(kSim3DDepthPass_Cloud, mesh));
      CHECK(!Sim3DDepthPass_CaptureGeometryMesh((Sim3DDepthPassLayer)-1, mesh));
      SDL_Surface *actual = ReadPass(device, renderer);
      CHECK(actual);
      CHECK(draw_calls - before_draws == 1);
      CHECK(geometry_upload_bytes - before_upload ==
          (!frame || (!phase && frame == 1) ? 8 * 40 : 0));
      if (!frame) reference = actual;
      else {
        if (reference && actual) for (int y = 0; y < 16; ++y)
          CHECK(!memcmp((uint8_t *)reference->pixels + y * reference->pitch,
              (uint8_t *)actual->pixels + y * actual->pitch, 32 * 4));
        SDL_DestroySurface(actual);
      }
    }
    if (phase && previous && reference) {
      bool changed = false;
      for (int y = 0; y < 16; ++y)
        changed |= memcmp((uint8_t *)previous->pixels + y * previous->pitch,
            (uint8_t *)reference->pixels + y * reference->pitch, 32 * 4) != 0;
      CHECK(changed); /* Warm geometry still samples the current, animated atlas. */
    }
    SDL_DestroySurface(previous); previous = reference;
  }
  SDL_DestroySurface(previous);
  Sim3DDepthPass_DestroyMesh(mesh); Sim3DDepthPass_DestroyMesh(other);
  Sim3DDepthPass_Reset(device);
}

static void TestSolidRanges(ArRenderDevice *device, SDL_Renderer *renderer, bool hardware_clipping) {
  Sim3DDepthPass_Reset(device);
  CHECK(!Sim3DDepthPass_CreateGeometryMesh());
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *solid = Sim3DDepthPass_CreateGeometryMesh();
  Sim3DDepthMesh *model = hardware_clipping ? Sim3DDepthPass_CreateHardwareClippedModelMesh()
                                         : Sim3DDepthPass_CreateModelMesh();
  CHECK(solid && model);
  if (!solid || !model) {
    Sim3DDepthPass_DestroyMesh(solid); Sim3DDepthPass_DestroyMesh(model); return;
  }
  Sim3DDepthVertex source[16];
  MakeRect(source, 4, 2, 28, 14, .5f, (ArRenderColorF){1,0,0,.5f});
  MakeRect(source + 4, 2, 4, 20, 12, .5f, (ArRenderColorF){0,1,0,.5f});
  MakeRect(source + 8, 12, 2, 30, 14, .5f, (ArRenderColorF){0,0,1,.5f});
  MakeRect(source + 12, 8, 4, 24, 12, .5f, (ArRenderColorF){1,1,0,.5f});
  Sim3DDepthModelVertex model_source[4];
  for (int p = 0; p < 4; ++p) model_source[p] = (Sim3DDepthModelVertex){
    .position = {source[12+p].x / 16 - 1, 1 - source[12+p].y / 8, 0},
    .color = source[12+p].color};
  const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  CHECK(!Sim3DDepthPass_UpdateGeometryMesh(solid, source, SIZE_MAX));
  CHECK(!Sim3DDepthPass_UpdateGeometryMesh(solid, source, 0));
  CHECK(!Sim3DDepthPass_UpdateGeometryMesh(model, source, 4));
  CHECK(!Sim3DDepthPass_UpdateModelMesh(solid, model_source, 1));
  Sim3DDepthVertex upload[16]; memcpy(upload, source, sizeof(upload));
  CHECK(Sim3DDepthPass_UpdateGeometryMesh(solid, upload, 4));
  memset(upload, 0, sizeof(upload)); /* No caller storage survives Update. */
  CHECK(Sim3DDepthPass_UpdateModelMesh(model, model_source, 1));
  Sim3DDepthVertex invalid[4]; memcpy(invalid, source, sizeof(invalid));
  invalid[0].x = NAN;
  CHECK(!Sim3DDepthPass_UpdateGeometryMesh(solid, invalid, 1));
  SDL_Surface *reference = NULL;
  for (int mixed = 0; mixed < 2; ++mixed) {
    CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
    CHECK(!Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, SIZE_MAX, 1));
    CHECK(!Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, 0, SIZE_MAX));
    CHECK(!Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, 3, 2));
    CHECK(!Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, 0, 0));
    CHECK(!Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, model, 0, 1));
    /* Overlapping translucent quads at identical depth make every reorder
     * observable. Ordinary-first, model, range offsets and ordinary-last. */
    const int order[] = {0, 2, 3, 1, 2};
    const uint64_t before = draw_calls;
    for (int i = 0; i < 5; ++i) {
      if (mixed && i == 2) CHECK(Sim3DDepthPass_AppendModelMesh(model, identity));
      else if (mixed && (i == 1 || i == 3))
        CHECK(Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, (size_t)order[i], 1));
      else CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, source + order[i] * 4));
    }
    if (mixed) CHECK(!Sim3DDepthPass_UpdateGeometryMesh(solid, source, 4));
    SDL_Surface *actual = ReadPass(device, renderer);
    CHECK(actual);
    CHECK(draw_calls - before == (mixed ? 5 : 1));
    if (mixed && actual && reference) {
      for (int y = 0; y < 16; ++y)
        CHECK(!memcmp((uint8_t *)actual->pixels + y * actual->pitch,
            (uint8_t *)reference->pixels + y * reference->pitch, 32 * 4));
      SDL_DestroySurface(actual);
    } else if (!mixed) reference = actual;
    else SDL_DestroySurface(actual);
  }
  SDL_DestroySurface(reference);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  const uint64_t warm = geometry_upload_bytes;
  unsigned accepted = 0;
  while (accepted < 1024 && Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, 1, 1)) ++accepted;
  CHECK(accepted > 0 && accepted < 1024);
  /* Exhaustion rejects only the optional append; ordinary tail still draws. */
  CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, source));
  SDL_Surface *bounded = ReadPass(device, renderer);
  CHECK(bounded); SDL_DestroySurface(bounded);
  CHECK(geometry_upload_bytes - warm == 4 * 40);
  CHECK(Sim3DDepthPass_Begin(device, 64, 16, kArRenderFilter_Nearest));
  CHECK(!Sim3DDepthPass_MeshReady(solid));
  CHECK(Sim3DDepthPass_UpdateGeometryMesh(solid, source, 4));
  Sim3DDepthPass_Reset(device);
  CHECK(!Sim3DDepthPass_MeshReady(solid));
  CHECK(!Sim3DDepthPass_UpdateGeometryMesh(solid, source, 4));
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  CHECK(!Sim3DDepthPass_MeshReady(solid));
  CHECK(Sim3DDepthPass_UpdateGeometryMesh(solid, source, 4));
  CHECK(Sim3DDepthPass_AppendGeometryMeshRange(kSim3DDepthPass_Solid, solid, 1, 1));
  Sim3DDepthPass_DestroyMesh(solid);
  CHECK(!ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
  Sim3DDepthPass_DestroyMesh(model);
  Sim3DDepthPass_Reset(device);
}

static void TestModelMesh(ArRenderDevice *device, SDL_Renderer *renderer) {
  Sim3DDepthPass_Reset(device);
  CHECK(Sim3DDepthPass_CreateModelMesh() == NULL); /* active pass required */
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *mesh = Sim3DDepthPass_CreateModelMesh();
  CHECK(mesh != NULL);
  if (!mesh) return;
  const Sim3DDepthModelVertex source[4] = {
    {{-.75f, -.5f, 0}, {1, 0, 0, 1}}, {{.75f, -.5f, 0}, {1, 0, 0, 1}},
    {{.75f, .5f, 0}, {1, 0, 0, 1}}, {{-.75f, .5f, 0}, {1, 0, 0, 1}},
  };
  const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  Sim3DDepthModelVertex vertices[4];
  memcpy(vertices, source, sizeof(vertices));
  CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, NULL, 1));
  CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, vertices, 0));
  CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, vertices, SIZE_MAX));
  CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, identity));
  for (int i = 0; i < 4; ++i) vertices[i].position[0] = vertices[i].position[1] = 2;
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, vertices, 1));
  float cancellation[16]; memcpy(cancellation, identity, sizeof(cancellation));
  cancellation[0] = FLT_MAX; cancellation[4] = -FLT_MAX; cancellation[15] = FLT_MAX;
  CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, cancellation)); /* finite double, overflowing float intermediates */
  for (int i = 0; i < 4; ++i) vertices[i].position[0] = vertices[i].position[1] = 1.7f;
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, vertices, 1));
  memcpy(cancellation, identity, sizeof(cancellation));
  cancellation[0] = 1e12f; cancellation[4] = -1e12f;
  CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, cancellation)); /* double cancellation hides float/FMA residual */
  memcpy(vertices, source, sizeof(vertices));
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, vertices, 1));
  memset(vertices, 0, sizeof(vertices)); /* Upload owns its copy. */
  for (int frame = 0; frame < 4; ++frame) {
    const int width = frame & 1 ? 64 : 32;
    if (frame == 3) {
      Sim3DDepthPass_Reset(device);
      CHECK(!Sim3DDepthPass_MeshReady(mesh));
    }
    CHECK(Sim3DDepthPass_Begin(device, width, 16, kArRenderFilter_Nearest));
    if (frame == 3) CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, source, 1));
    CHECK(Sim3DDepthPass_MeshReady(mesh));
    Sim3DDepthPosition wrong[4] = {0};
    CHECK(!Sim3DDepthPass_UpdateMesh(mesh, wrong, 1));
    memcpy(vertices, source, sizeof(vertices));
    vertices[0].position[0] = NAN;
    CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, vertices, 1));
    memcpy(vertices, source, sizeof(vertices));
    vertices[0].color.a = 2;
    CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, vertices, 1));
    float bad[16];
    for (int axis = 0; axis < 3; ++axis) for (int sign = -1; sign <= 1; sign += 2) {
      memcpy(bad, identity, sizeof(bad)); bad[12 + axis] = 2.0f * sign;
      CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad));
    }
    memcpy(bad, identity, sizeof(bad)); bad[15] = -1;
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad));
    memcpy(bad, identity, sizeof(bad)); bad[12] = 1;
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad)); /* clipped side */
    memcpy(bad, identity, sizeof(bad)); bad[14] = 1;
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad)); /* far plane */
    bad[14] = -1;
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad)); /* near plane */
    bad[0] = NAN;
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad));
    memcpy(bad, identity, sizeof(bad));
    bad[0] = bad[1] = bad[2] = FLT_MAX;
    bad[3] = bad[7] = FLT_MAX; bad[15] = FLT_MAX;
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad));
    CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, NULL));
    CHECK(AppendRect(kSim3DDepthPass_DepthOccluder, 0, 0, width / 2, 16,
        .25f, (ArRenderColorF){1,1,1,1}));
    float camera[16]; memcpy(camera, identity, sizeof(camera));
    const uint64_t before = geometry_upload_bytes;
    CHECK(Sim3DDepthPass_AppendModelMesh(mesh, camera));
    memset(camera, 0, sizeof(camera)); /* Append owns its transform. */
    CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, source, 1));
    CHECK(AppendRect(kSim3DDepthPass_Solid, 0, 0, 2, 2,
        .5f, (ArRenderColorF){1,1,1,1})); /* ordered ordinary tail */
    SDL_Texture *output = ArSdlRenderBackend_UnwrapTexture(
        Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid()));
    CHECK(output != NULL);
    CHECK(geometry_upload_bytes - before ==
        8 * 40 + ((frame == 0 || frame == 3) ? sizeof(source) : 0));
    if (!output) continue;
    CHECK(SDL_SetRenderTarget(renderer, output));
    SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *argb = readback ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(argb != NULL);
    if (argb) {
      CHECK(ReadArgb(argb, width / 4, 8) == 0); /* shared ground depth */
      CHECK(ReadArgb(argb, width * 3 / 4, 8) == 0xffff0000u);
      CHECK(ReadArgb(argb, width - 1, 8) == 0);
    }
    SDL_DestroySurface(argb); SDL_DestroySurface(readback);
    CHECK(SDL_SetRenderTarget(renderer, NULL));
  }
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  CHECK(Sim3DDepthPass_AppendModelMesh(mesh, identity));
  Sim3DDepthPass_DestroyMesh(mesh);
  CHECK(!ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
  Sim3DDepthPass_Reset(device);
}

static bool AppendCpuClippedModel(const Sim3DDepthModelVertex source[4],
    const float matrix[16], int width, int height) {
  WorldNavigationProjection projection = {.clip_frustum = true};
  memcpy(projection.matrix, matrix, sizeof(projection.matrix));
  const ArRenderRectI viewport = {0,0,width,height};
  Sim3DDepthVertex vertices[4];
  Scene3DClipPoint clip[4];
  for (int i = 0; i < 4; ++i) {
    Scene3DPoint point;
    float depth;
    if (!WorldNavigationProjectClippedPoint(&projection, viewport,
        source[i].position, &point, &depth, &clip[i])) return false;
    vertices[i] = (Sim3DDepthVertex){point.x, point.y, depth, source[i].color, {-1,-1}};
  }
  return WorldNavigationAppendClippedQuad(kSim3DDepthPass_Solid, vertices, clip, viewport);
}

static void TestHardwareClippedModels(ArRenderDevice *device, SDL_Renderer *renderer) {
  Sim3DDepthPass_Reset(device);
  CHECK(!Sim3DDepthPass_CreateHardwareClippedModelMesh());
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *mesh = Sim3DDepthPass_CreateHardwareClippedModelMesh();
  CHECK(mesh);
  if (!mesh) return;
  const Sim3DDepthModelVertex source[4] = {
    {{-.5f,-.5f,0},{1,0,0,1}}, {{.5f,-.5f,0},{1,0,0,1}},
    {{.5f,.5f,0},{1,0,0,1}}, {{-.5f,.5f,0},{1,0,0,1}},
  };
  const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  Sim3DDepthModelVertex upload[4]; memcpy(upload, source, sizeof(upload));
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, upload, 1));
  memset(upload, 0, sizeof(upload));
  unsigned visible_cases = 0, empty_cases = 0;
  for (int phase = 0; phase < 3; ++phase) {
    const int width = phase ? 64 : 32, height = 16;
    if (phase == 2) Sim3DDepthPass_Reset(device);
    for (int test = 0; test < 18; ++test) {
      float matrix[16]; memcpy(matrix, identity, sizeof(matrix));
      if (test < 12) {
        const int axis = test / 2 % 3;
        matrix[12 + axis] = (test & 1 ? 1.0f : -1.0f) * (test < 6 ? .75f : 2.0f);
        if (axis == 2) matrix[2] = 1; /* Sloping depth crosses near/far planes. */
      } else if (test == 12 || test == 13) {
        matrix[3] = 1; matrix[15] = test == 12 ? .25f : .5f; /* Negative/zero W corners. */
      } else if (test == 14) matrix[15] = -1; /* Entirely behind the eye. */
      else if (test == 15) matrix[0] = matrix[5] = 8; /* All corners outside, visible center. */
      else if (test == 16) matrix[0] = 0; /* Degenerate, no fragments. */

      SDL_Surface *reference = NULL;
      for (int gpu = 0; gpu < 2; ++gpu) {
        CHECK(Sim3DDepthPass_Begin(device, width, height, kArRenderFilter_Nearest));
        CHECK(AppendRect(kSim3DDepthPass_DepthOccluder, 0, 0, width / 2, height,
            .25f, (ArRenderColorF){1,1,1,1}));
        const uint64_t before_bytes = geometry_upload_bytes, before_draws = draw_calls;
        if (gpu) {
          const bool publish = !Sim3DDepthPass_MeshReady(mesh);
          if (publish) CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, source, 1));
          float camera[16]; memcpy(camera, matrix, sizeof(camera));
          CHECK(Sim3DDepthPass_AppendModelMesh(mesh, camera));
          memset(camera, 0, sizeof(camera));
          CHECK(!Sim3DDepthPass_UpdateModelMesh(mesh, source, 1));
          SDL_Surface *actual = ReadPass(device, renderer);
          CHECK(actual && reference);
          /* One whole-object model draw even if every corner is clipped.
           * Camera/viewport changes require no source re-upload. */
          CHECK(draw_calls - before_draws == 2);
          CHECK(geometry_upload_bytes - before_bytes == 4 * 40 +
              ((publish || (!phase && !test)) ? sizeof(source) : 0));
          if (actual && reference) {
            unsigned different = 0, painted = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
              const uint8_t *a = (const uint8_t *)actual->pixels + y * actual->pitch + x * 4;
              const uint8_t *b = (const uint8_t *)reference->pixels + y * reference->pitch + x * 4;
              different += memcmp(a, b, 4) != 0; painted += a[3] != 0;
            }
            if (different) fprintf(stderr, "hardware clip phase=%d case=%d different=%u\n", phase, test, different);
            CHECK(!different);
            visible_cases += painted != 0; empty_cases += painted == 0;
          }
          SDL_DestroySurface(actual);
        } else {
          CHECK(AppendCpuClippedModel(source, matrix, width, height));
          reference = ReadPass(device, renderer); CHECK(reference);
        }
      }
      SDL_DestroySurface(reference);
    }
  }
  CHECK(visible_cases && empty_cases);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  float bad[16]; memcpy(bad, identity, sizeof(bad)); bad[0] = NAN;
  CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad));
  for (int i = 0; i < 4; ++i) upload[i] = (Sim3DDepthModelVertex){{2,2,0},{1,0,0,1}};
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, upload, 1));
  memcpy(bad, identity, sizeof(bad)); bad[0] = FLT_MAX; bad[4] = -FLT_MAX;
  CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad)); /* Hardware still requires finite arithmetic. */
  for (int i = 0; i < 4; ++i) upload[i].position[0] = upload[i].position[1] = 1;
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, upload, 1));
  bad[4] = 0;
  CHECK(!Sim3DDepthPass_AppendModelMesh(mesh, bad)); /* Includes an intermediate-rounding margin. */
  CHECK(Sim3DDepthPass_UpdateModelMesh(mesh, source, 1));
  unsigned accepted = 0;
  while (accepted < 1024 && Sim3DDepthPass_AppendModelMesh(mesh, identity)) ++accepted;
  CHECK(accepted == 64); /* Same opaque budget, not the weather budget. */
  CHECK(AppendRect(kSim3DDepthPass_Solid, 0, 0, 32, 16, .25f, (ArRenderColorF){1,1,1,1}));
  SDL_Surface *bounded = ReadPass(device, renderer);
  CHECK(bounded); SDL_DestroySurface(bounded);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  CHECK(Sim3DDepthPass_AppendModelMesh(mesh, identity));
  Sim3DDepthPass_DestroyMesh(mesh);
  CHECK(!ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
  Sim3DDepthPass_Reset(device);
}

static void TestRadialModels(ArRenderDevice *device, SDL_Renderer *renderer) {
  Sim3DDepthPass_Reset(device);
  CHECK(!Sim3DDepthPass_CreateRadialMesh());
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthMesh *mesh = Sim3DDepthPass_CreateRadialMesh();
  Sim3DDepthMesh *reference_mesh = Sim3DDepthPass_CreateHardwareClippedModelMesh();
  CHECK(mesh && reference_mesh);
  if (!mesh || !reference_mesh) {
    Sim3DDepthPass_DestroyMesh(mesh); Sim3DDepthPass_DestroyMesh(reference_mesh); return;
  }
  Sim3DDepthRadialVertex source[16];
  const ArRenderColorF colors[] = {{1,1,0,.5f}, {1,0,0,1}, {0,1,0,1}, {0,0,1,1}};
  for (unsigned q = 0; q < 4; ++q) for (unsigned p = 0; p < 4; ++p) {
    source[q * 4 + p] = (Sim3DDepthRadialVertex){
      .normal = {p == 1 || p == 2 ? .3f : -.3f, p >= 2 ? .4f : -.4f, sqrtf(.75f)},
      .elevation = {.25f, .125f}, .color = colors[q], .variant = (float)q,
    };
  }
  Sim3DDepthRadialVertex upload[16]; memcpy(upload, source, sizeof(upload));
  CHECK(!Sim3DDepthPass_UpdateRadialMesh(mesh, NULL, 1));
  CHECK(!Sim3DDepthPass_UpdateRadialMesh(mesh, source, 0));
  CHECK(!Sim3DDepthPass_UpdateRadialMesh(mesh, source, SIZE_MAX));
  CHECK(Sim3DDepthPass_UpdateRadialMesh(mesh, upload, 4));
  memset(upload, 0, sizeof(upload));
  for (unsigned bad = 0; bad < 6; ++bad) {
    memcpy(upload, source, sizeof(upload));
    if (bad == 0) upload[0].normal[0] = NAN;
    if (bad == 1) upload[0].normal[2] = 0;
    if (bad == 2) upload[0].elevation[0] = INFINITY;
    if (bad == 3) upload[0].color.a = NAN;
    if (bad == 4) upload[0].variant = 1; /* Mixed tags within a quad. */
    if (bad == 5) upload[0].variant = .5f;
    CHECK(!Sim3DDepthPass_UpdateRadialMesh(mesh, upload, 4));
  }
  unsigned cases = 0;
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    const int width = cycle ? 64 : 32;
    if (cycle == 2) Sim3DDepthPass_Reset(device);
    for (unsigned test = 0; test < 12; ++test) for (unsigned pose = 0; pose < 4; ++pose) {
      Sim3DDepthRadialTransform transform = {
        .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        .basis = {{1,0,0}, {0,1,0}, {0,0,1}},
        .sphere_radius = 1, .reference_height = .125f, .height_scale = 2, .variant = pose,
      };
      if (test < 6) transform.matrix[12 + test / 2] = test & 1 ? .8f : -.8f;
      else if (test == 6) transform.matrix[15] = -1;
      else if (test == 7) { transform.matrix[3] = 2; transform.matrix[15] = .5f; }
      else if (test == 8) transform.matrix[0] = transform.matrix[5] = 8;
      else if (test == 9) {
        transform.basis[0][0] = transform.basis[1][1] = 0;
        transform.basis[0][1] = -1; transform.basis[1][0] = 1;
      } else if (test == 10) transform.height_scale = 0;
      const unsigned order[4] = {3,0,1,2};
      const Sim3DDepthMeshRange ranges[3] = {{3,1}, {0,1}, {1,1}};
      const Sim3DDepthMeshRange whole = {0,4};
      const bool selected = test >= 6;
      SDL_Surface *reference = NULL;
      for (unsigned gpu = 0; gpu < 2; ++gpu) {
        CHECK(Sim3DDepthPass_Begin(device, width, 16, kArRenderFilter_Nearest));
        /* Shared depth and ordinary/sample order are observable here. */
        CHECK(AppendRect(kSim3DDepthPass_DepthOccluder, 0, 0, width / 2, 16, .15f, colors[0]));
        CHECK(AppendRect(kSim3DDepthPass_Solid, 0, 0, width, 16, .95f, colors[0]));
        const uint64_t bytes = geometry_upload_bytes, draws = draw_calls;
        const bool republish = gpu && !Sim3DDepthPass_MeshReady(mesh);
        if (gpu) {
          if (republish) CHECK(Sim3DDepthPass_UpdateRadialMesh(mesh, source, 4));
          CHECK(!Sim3DDepthPass_SelectRadialMesh(mesh, NULL, 1));
          const Sim3DDepthMeshRange invalid[] = {{0,1}, {4,1}};
          CHECK(!Sim3DDepthPass_SelectRadialMesh(mesh, invalid, 2));
          if (!pose || republish) {
            Sim3DDepthMeshRange copied_ranges[3];
            memcpy(copied_ranges, ranges, sizeof(ranges));
            CHECK(Sim3DDepthPass_SelectRadialMesh(mesh, selected ? copied_ranges : &whole, selected ? 3 : 1));
            memset(copied_ranges, 0, sizeof(copied_ranges));
          }
          Sim3DDepthRadialTransform copied = transform;
          CHECK(Sim3DDepthPass_AppendRadialMesh(mesh, &copied));
          memset(&copied, 0, sizeof(copied));
          CHECK(!Sim3DDepthPass_UpdateRadialMesh(mesh, source, 4));
          CHECK(!Sim3DDepthPass_SelectRadialMesh(mesh, &whole, 1));
        } else {
          /* Isolate radial placement/selection from the already documented
           * CPU-vs-hardware clipped-edge interpolation difference. Reference
           * vertices are placed on the CPU, then use the SAME hardware
           * clipping/color policy. No relaxed pixel threshold. */
          Sim3DDepthModelVertex projected[16];
          size_t quads = 0;
          for (unsigned index = 0; index < (selected ? 3u : 4u); ++index) {
            const unsigned q = selected ? order[index] : index;
            if (q && q != pose) continue;
            for (unsigned p = 0; p < 4; ++p) {
              const Sim3DDepthRadialVertex *v = &source[q * 4 + p];
              const float radius = fmaf(transform.reference_height, transform.height_scale, transform.sphere_radius);
              const float base = (v->elevation[0] - transform.reference_height) * transform.height_scale;
              const float rise = base + v->elevation[1];
              for (unsigned axis = 0; axis < 3; ++axis) {
                const float *b = transform.basis[axis];
                const float n = fmaf(b[2], v->normal[2], fmaf(b[0], v->normal[0], b[1] * v->normal[1]));
                projected[quads * 4 + p].position[axis] = fmaf(n, rise, radius * (n - (axis == 2)));
              }
              projected[quads * 4 + p].color = v->color;
            }
            ++quads;
          }
          CHECK(Sim3DDepthPass_UpdateModelMesh(reference_mesh, projected, quads));
          CHECK(Sim3DDepthPass_AppendModelMesh(reference_mesh, transform.matrix));
        }
        CHECK(AppendRect(kSim3DDepthPass_Solid, width - 2, 0, width, 16, 0, colors[0]));
        SDL_Surface *actual = ReadPass(device, renderer); CHECK(actual);
        if (!gpu) reference = actual;
        else {
          CHECK(draw_calls - draws == 4); /* Still one draw for all poses. */
          CHECK(geometry_upload_bytes - bytes == 12 * 40 +
              (republish || (!cycle && !test && !pose) ? sizeof(source) : 0) +
              (!pose || republish ? (selected ? 18u : 24u) * sizeof(uint32_t) : 0));
          if (actual && reference) {
            unsigned different = 0, maximum = 0;
            for (int y = 0; y < 16; ++y) for (int x = 0; x < width * 4; ++x) {
              const uint8_t a = *((uint8_t *)actual->pixels + y * actual->pitch + x);
              const uint8_t b = *((uint8_t *)reference->pixels + y * reference->pitch + x);
              const unsigned error = (unsigned)abs((int)a - b);
              different += error != 0;
              if (error > maximum) maximum = error;
            }
            if (different) fprintf(stderr, "radial cycle=%u case=%u pose=%u bytes=%u max=%u\n",
                cycle, test, pose, different, maximum);
            CHECK(!different);
          }
          SDL_DestroySurface(actual); ++cases;
        }
      }
      SDL_DestroySurface(reference);
    }
  }
  CHECK(cases == 144);
  CHECK(Sim3DDepthPass_Begin(device, 32, 16, kArRenderFilter_Nearest));
  Sim3DDepthRadialTransform t = {
    .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .basis = {{1,0,0}, {0,1,0}, {0,0,1}}, .sphere_radius = 1,
  };
  CHECK(!Sim3DDepthPass_AppendRadialMesh(mesh, NULL));
  for (unsigned bad = 0; bad < 6; ++bad) {
    Sim3DDepthRadialTransform invalid = t;
    if (bad == 0) invalid.basis[0][0] = NAN;
    if (bad == 1) invalid.matrix[0] = INFINITY;
    if (bad == 2) invalid.variant = 65536;
    if (bad == 3) invalid.height_scale = -1;
    if (bad == 4) invalid.sphere_radius = FLT_MAX;
    if (bad == 5) { invalid.reference_height = FLT_MAX; invalid.height_scale = FLT_MAX; }
    CHECK(!Sim3DDepthPass_AppendRadialMesh(mesh, &invalid));
  }
  unsigned accepted = 0;
  while (accepted < 1024 && Sim3DDepthPass_AppendRadialMesh(mesh, &t)) ++accepted;
  CHECK(accepted == 64);
  Sim3DDepthPass_DestroyMesh(mesh);
  Sim3DDepthPass_DestroyMesh(reference_mesh);
  CHECK(!ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
  Sim3DDepthPass_Reset(device);
}

static void TestColoredTerrainOcclusion(
    ArRenderDevice *render_device, SDL_Renderer *renderer) {
  const ArRenderColorF white = {1, 1, 1, 1};
  const ArRenderRectI atlas_region = {0, 0, 1, 1};
  const uint32_t blue = 0xff0000ffu, yellow = 0xffffff00u;
  const uint32_t cloud = 0xffffffffu;
  const uint32_t world_mountain = 0xff00ffffu;
  /* Repeat across a resource reset and reverse terrain submission order.
   * Ground, ocean and buildings must share real depth, not painter order;
   * publishing world art must also leave the town mountain atlas intact. */
  for (int order = 0; order < 2; order++) {
    CHECK(Sim3DDepthPass_UploadMountainAtlasRegions(
        render_device, &yellow, 1, 1, 4, &atlas_region, 1));
    CHECK(Sim3DDepthPass_UploadAtlasRegions(
        render_device, kSim3DDepthPass_WorldMountain,
        &world_mountain, 1, 1, 4, &atlas_region, 1));
    CHECK(Sim3DDepthPass_UploadAtlasRegions(
        render_device, kSim3DDepthPass_Ground,
        &blue, 1, 1, 4, &atlas_region, 1));
    CHECK(Sim3DDepthPass_UploadAtlasRegions(
        render_device, kSim3DDepthPass_GroundBlur,
        &yellow, 1, 1, 4, &atlas_region, 1));
    CHECK(Sim3DDepthPass_UploadAtlasRegions(
        render_device, kSim3DDepthPass_Cloud,
        &cloud, 1, 1, 4, &atlas_region, 1));
    CHECK(Sim3DDepthPass_UploadAtlasRegions(
        render_device, kSim3DDepthPass_VolumeCloud,
        &yellow, 1, 1, 4, &atlas_region, 1));
    CHECK(!Sim3DDepthPass_UploadAtlasRegions(
        render_device, kSim3DDepthPass_Solid,
        &blue, 1, 1, 4, &atlas_region, 1));
    CHECK(Sim3DDepthPass_Begin(
        render_device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
    Sim3DDepthVertex near_ground[4], far_ground[4];
    MakeRect(near_ground, 0, 0, 16, 16, 0.2f,
             (ArRenderColorF){0, 1, 0, 1});
    for (int i = 0; i < 4; i++)
      near_ground[i].uv = (ArRenderPointF){-1, -1};
    MakeRect(far_ground, 0, 0, 32, 16, 0.8f, white);
    CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Ground,
        order ? far_ground : near_ground));
    CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Ground,
        order ? near_ground : far_ground));
    CHECK(AppendRect(kSim3DDepthPass_Solid,
        0, 0, 24, 16, 0.5f, (ArRenderColorF){1, 0, 0, 1}));
    CHECK(AppendRect(kSim3DDepthPass_Mountain,
        0, 0, 8, 8, 0.1f, white));
    CHECK(AppendRect(kSim3DDepthPass_WorldMountain,
        8, 0, 12, 4, 0.1f, white));
    CHECK(AppendRect(kSim3DDepthPass_WorldMountain,
        12, 0, 16, 4, 0.7f, white)); /* near ridge occludes it */
    /* Behind the near ridge: neither blur nor haze may bleed through it. */
    CHECK(AppendRect(kSim3DDepthPass_GroundBlur,
        0, 0, 32, 16, 0.9f, white));
    CHECK(AppendRect(kSim3DDepthPass_GroundHaze,
        0, 0, 16, 16, 0.8f, (ArRenderColorF){1, 0, 0, 0.5f}));
    /* Equal-depth haze must still work on its own visible ground surface. */
    CHECK(AppendRect(kSim3DDepthPass_GroundHaze,
        24, 0, 32, 4, 0.8f, (ArRenderColorF){1, 0, 0, 1}));
    CHECK(AppendRect(kSim3DDepthPass_CloudShadow,
        24, 4, 32, 8, 0.8f, (ArRenderColorF){0, 0, 0, 1}));
    CHECK(AppendRect(kSim3DDepthPass_Cloud,
        0, 0, 32, 16, 0.9f, white)); /* rear deck rejected by opaque sphere */
    CHECK(AppendRect(kSim3DDepthPass_Cloud,
        0, 12, 32, 16, 0.3f, white)); /* foreground cloud above town */
    CHECK(AppendRect(kSim3DDepthPass_VolumeCloud,
        0, 0, 32, 16, 0.95f, white)); /* globe rejects rear volume */
    CHECK(AppendRect(kSim3DDepthPass_VolumeCloud,
        16, 10, 32, 12, 0.1f, white));
    CHECK(AppendRect(kSim3DDepthPass_VolumeCloud,
        16, 10, 32, 12, 0.3f, (ArRenderColorF){1, 0, 0, 1}));
    /* Deliberately reversed slices: red survives, proving no depth writes. */
    ArRenderTexture result = Sim3DDepthPass_Submit(
        render_device, ArRenderTexture_Invalid());
    SDL_Texture *output = ArSdlRenderBackend_UnwrapTexture(result);
    CHECK(output != NULL);
    if (output) {
      CHECK(SDL_SetRenderTarget(renderer, output));
      SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
      CHECK(readback != NULL);
      SDL_Surface *argb = readback
          ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
      CHECK(argb != NULL);
      if (argb) {
        CHECK(ReadArgb(argb, 12, 8) == 0xff00ff00u); /* ridge hides building */
        CHECK(ReadArgb(argb, 20, 8) == 0xffff0000u); /* building above ground */
        CHECK(ReadArgb(argb, 28, 8) == blue);        /* distant ground */
        CHECK(ReadArgb(argb, 4, 4) == yellow);       /* independent town atlas */
        CHECK(ReadArgb(argb, 10, 2) == world_mountain);
        CHECK(ReadArgb(argb, 14, 2) == 0xff00ff00u);
        CHECK(ReadArgb(argb, 28, 2) == 0xffff0000u); /* visible haze */
        CHECK(ReadArgb(argb, 28, 6) == 0xff000000u); /* ground shadow */
        CHECK(ReadArgb(argb, 12, 14) == 0xff00ff00u); /* ridge hides cloud */
        CHECK(ReadArgb(argb, 20, 14) == cloud);
        CHECK(ReadArgb(argb, 28, 14) == cloud);
        CHECK(ReadArgb(argb, 28, 11) == 0xffff0000u);
      }
      SDL_DestroySurface(argb);
      SDL_DestroySurface(readback);
      CHECK(SDL_SetRenderTarget(renderer, NULL));
    }
    Sim3DDepthPass_Reset(render_device);
  }
}

static uint32_t AtlasTestPixel(int x, int y, unsigned seed) {
  return (((x * 47u + y * 19u + seed) & 255u) << 24) |
      (((x * 23u + y * 7u + seed) & 255u) << 16) |
      (((x * 13u + y * 61u + seed) & 255u) << 8) |
      ((x * 3u + y * 37u + seed) & 255u);
}

static void TestAtlasRegionPacking(
    ArRenderDevice *render_device, SDL_Renderer *renderer) {
  /* Integer ARGB input can have padded rows, including a byte pitch not
   * divisible by four. Pack only the requested texels and retain everything
   * outside them across transfer-buffer cycling and renderer reset. */
  enum { kWidth = 17, kHeight = 9, kPitch = kWidth * 4 + 3 };
  const ArRenderRectI full = {0, 0, kWidth, kHeight};
  const ArRenderRectI dirty[] = {{1, 1, 5, 3}, {9, 0, 1, 9}, {0, 8, 8, 1}};
  uint8_t *source = malloc(kPitch * kHeight);
  CHECK(source != NULL);
  if (!source) return;
  uint32_t reference[kWidth * kHeight];
  bool reference_ready = false;
  Sim3DDepthPass_Reset(render_device);
  for (int mode = 0; mode < 3; mode++) {
    if (mode == 2) Sim3DDepthPass_Reset(render_device);
    memset(source, 0xa5, kPitch * kHeight);
    for (int y = 0; y < kHeight; y++)
      for (int x = 0; x < kWidth; x++) {
        bool changed = false;
        for (size_t i = 0; i < sizeof(dirty) / sizeof(dirty[0]); i++)
          if (x >= dirty[i].x && x < dirty[i].x + dirty[i].w &&
              y >= dirty[i].y && y < dirty[i].y + dirty[i].h) changed = true;
        const uint32_t pixel = AtlasTestPixel(x, y, mode == 0 && changed ? 73 : 19);
        memcpy(source + y * kPitch + x * 4, &pixel, sizeof(pixel));
      }
    CHECK(!Sim3DDepthPass_UploadAtlasRegions(render_device,
        kSim3DDepthPass_WorldMountain, (const uint32_t *)source,
        kWidth, kHeight, kWidth * 4 - 1, &full, 1));
    CHECK(Sim3DDepthPass_UploadAtlasRegions(render_device,
        kSim3DDepthPass_WorldMountain, (const uint32_t *)source,
        kWidth, kHeight, kPitch, &full, 1));
    if (mode) {
      /* Unrequested source texels differ too: accidentally uploading a whole
       * row or the full atlas must not match the reference. */
      for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++) {
          const uint32_t pixel = AtlasTestPixel(x, y, 73);
          memcpy(source + y * kPitch + x * 4, &pixel, sizeof(pixel));
        }
      if (mode == 1) {
        CHECK(Sim3DDepthPass_UploadAtlasRegions(render_device,
            kSim3DDepthPass_WorldMountain, (const uint32_t *)source,
            kWidth, kHeight, kPitch, dirty, 3));
      } else {
        for (int i = 2; i >= 0; i--)
          CHECK(Sim3DDepthPass_UploadAtlasRegions(render_device,
              kSim3DDepthPass_WorldMountain, (const uint32_t *)source,
              kWidth, kHeight, kPitch, &dirty[i], 1));
      }
    }
    memset(source, 0, kPitch * kHeight);
    CHECK(Sim3DDepthPass_Begin(render_device, kWidth, kHeight, kArRenderFilter_Nearest));
    CHECK(AppendRect(kSim3DDepthPass_WorldMountain, 0, 0, kWidth, kHeight,
        0.5f, (ArRenderColorF){1, 1, 1, 1}));
    SDL_Texture *output = ArSdlRenderBackend_UnwrapTexture(
        Sim3DDepthPass_Submit(render_device, ArRenderTexture_Invalid()));
    CHECK(output != NULL);
    if (!output) continue;
    CHECK(SDL_SetRenderTarget(renderer, output));
    SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
    CHECK(readback != NULL);
    SDL_Surface *argb = readback
        ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(argb != NULL);
    if (argb) {
      CHECK(ReadArgb(argb, 3, 2) != ReadArgb(argb, 14, 7));
      for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++) {
          const uint32_t pixel = ReadArgb(argb, x, y);
          if (reference_ready) CHECK(pixel == reference[y * kWidth + x]);
          else reference[y * kWidth + x] = pixel;
        }
      reference_ready = true;
    }
    SDL_DestroySurface(argb);
    SDL_DestroySurface(readback);
    CHECK(SDL_SetRenderTarget(renderer, NULL));
  }
  free(source);
  Sim3DDepthPass_Reset(render_device);
}

static void TestBatchCopyAndGrowth(
    ArRenderDevice *render_device, SDL_Renderer *renderer) {
  /* Cross both the CPU and GPU initial capacities. Every attribute varies;
   * compare single quads, uneven chunks, and a complete batch across reuse
   * and reset. Caller storage is overwritten before the backend submits. */
  enum { kQuads = 2053, kVertices = kQuads * 4 };
  Sim3DDepthVertex *vertices = malloc(kVertices * sizeof(*vertices));
  CHECK(vertices != NULL);
  if (!vertices) return;
  uint32_t reference[kTestWidth * kTestHeight];
  bool reference_ready = false;
  const size_t batch_sizes[] = {1, 127, kQuads, kQuads};
  const uint32_t atlas[] = {0xc0ff4010, 0x8070ff30, 0xe03060ff, 0xa0ffffff};
  const ArRenderRectI region = {0, 0, 2, 2};
  Sim3DDepthPass_Reset(render_device);
  for (size_t mode = 0; mode < sizeof(batch_sizes) / sizeof(batch_sizes[0]); mode++) {
    if (mode == 3) Sim3DDepthPass_Reset(render_device);
    CHECK(Sim3DDepthPass_UploadAtlasRegions(render_device,
        kSim3DDepthPass_Cloud, atlas, 2, 2, 8, &region, 1));
    CHECK(Sim3DDepthPass_Begin(render_device,
        kTestWidth, kTestHeight, kArRenderFilter_Linear));
    CHECK(AppendRect(kSim3DDepthPass_Solid, 0, 0, kTestWidth, kTestHeight,
        0.95f, (ArRenderColorF){0.1f, 0.2f, 0.5f, 1}));
    CHECK(AppendRect(kSim3DDepthPass_Solid, 2, 2, 8, 8,
        0.1f, (ArRenderColorF){1, 1, 0, 1}));
    for (int i = 0; i < kQuads; i++) {
      const float x = (float)(i % kTestWidth) + 0.125f;
      const float y = (float)((i / kTestWidth) % kTestHeight) + 0.25f;
      MakeRect(vertices + i * 4, x, y, x + 1, y + 1,
          0.2f + (i % 7) * 0.08f, (ArRenderColorF){1, 1, 1, 1});
      for (int p = 0; p < 4; p++) {
        Sim3DDepthVertex *v = vertices + i * 4 + p;
        v->color = (ArRenderColorF){
          (float)((i + p * 43) % 256) / 255.0f,
          (float)((i * 17 + p * 13) % 256) / 255.0f,
          (float)((i * 3 + p * 61) % 256) / 255.0f,
          0.2f + (i % 5) * 0.15f,
        };
        v->uv.x = 0.125f + v->uv.x * 0.75f;
        v->uv.y = 0.0625f + v->uv.y * 0.875f;
      }
    }
    for (size_t first = 0; first < kQuads;) {
      const size_t count = kQuads - first < batch_sizes[mode]
          ? kQuads - first : batch_sizes[mode];
      CHECK(Sim3DDepthPass_AppendQuads(
          kSim3DDepthPass_Cloud, vertices + first * 4, count));
      first += count;
    }
    CHECK(!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Cloud, vertices, SIZE_MAX));
    memset(vertices, 0, kVertices * sizeof(*vertices));
    SDL_Texture *output = ArSdlRenderBackend_UnwrapTexture(
        Sim3DDepthPass_Submit(render_device, ArRenderTexture_Invalid()));
    CHECK(output != NULL);
    if (!output) continue;
    CHECK(SDL_SetRenderTarget(renderer, output));
    SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
    CHECK(readback != NULL);
    SDL_Surface *argb = readback
        ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(argb != NULL);
    if (argb) {
      CHECK(ReadArgb(argb, 4, 4) == 0xffffff00u);
      CHECK(ReadArgb(argb, 12, 8) != ReadArgb(argb, 20, 8));
      for (int y = 0; y < kTestHeight; y++)
        for (int x = 0; x < kTestWidth; x++) {
          const uint32_t pixel = ReadArgb(argb, x, y);
          if (reference_ready) CHECK(pixel == reference[y * kTestWidth + x]);
          else reference[y * kTestWidth + x] = pixel;
        }
      reference_ready = true;
    }
    SDL_DestroySurface(argb);
    SDL_DestroySurface(readback);
    CHECK(SDL_SetRenderTarget(renderer, NULL));
  }
  free(vertices);
  Sim3DDepthPass_Reset(render_device);
}


static void TestRetainedSamples(ArRenderDevice *device, SDL_Renderer *renderer) {
  const uint32_t texels[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
  const ArRenderRectI region = {0, 0, 2, 2};
  Sim3DDepthMesh *mesh = NULL;
  uint32_t reference[kTestWidth * kTestHeight];
  for (int variant = 0; variant < 4; ++variant) {
    if (variant == 3) Sim3DDepthPass_Reset(device); /* opaque handle survives reset */
    CHECK(Sim3DDepthPass_UploadAtlasRegions(device, kSim3DDepthPass_Cloud,
        texels, 2, 2, 8, &region, 1));
    CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
    geometry_upload_bytes = 0;
    CHECK(AppendRect(kSim3DDepthPass_DepthOccluder, 0, 0, 9, kTestHeight,
        .2f, (ArRenderColorF){1, 1, 1, 1}));
    Sim3DDepthVertex quad[4];
    MakeRect(quad, 0, 0, kTestWidth, kTestHeight, .65f, (ArRenderColorF){1, 1, 1, 1});
    Sim3DDepthPosition positions[4];
    for (int p = 0; p < 4; ++p) positions[p] = (Sim3DDepthPosition){quad[p].x, quad[p].y, quad[p].depth};
    if (variant) {
      if (!mesh) mesh = Sim3DDepthPass_CreateMesh();
      CHECK(mesh);
      CHECK(Sim3DDepthPass_MeshReady(mesh) == (variant == 2));
      if (variant != 2) CHECK(Sim3DDepthPass_UpdateMesh(mesh, positions, 1));
      memset(positions, 0, sizeof(positions)); /* update copied the caller's array */
      CHECK(Sim3DDepthPass_MeshReady(mesh));
    }
    for (int sample = 0; sample < 3; ++sample) {
      const ArRenderColorF color = {.3f + sample * .2f, .8f - sample * .15f, .6f,
          sample == 1 ? .57f : .21f};
      ArRenderPointF uv[4];
      for (int p = 0; p < 4; ++p) {
        uv[p] = (ArRenderPointF){quad[p].uv.x * .7f + sample * .15f, quad[p].uv.y};
      }
      if (variant) {
        CHECK(!Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_Solid, mesh, uv, 1, color));
        CHECK(!Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_CloudShadow, mesh, uv, 2, color));
        CHECK(Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_CloudShadow, mesh, uv, 1, color));
        CHECK(!Sim3DDepthPass_UpdateMesh(mesh, positions, 1)); /* queued geometry is immutable */
        CHECK(!Sim3DDepthPass_AppendQuad(kSim3DDepthPass_CloudShadow, quad));
      } else {
        Sim3DDepthVertex ordinary[4];
        memcpy(ordinary, quad, sizeof(ordinary));
        for (int p = 0; p < 4; ++p) { ordinary[p].uv = uv[p]; ordinary[p].color = color; }
        CHECK(Sim3DDepthPass_AppendQuad(kSim3DDepthPass_CloudShadow, ordinary));
      }
      memset(uv, 0, sizeof(uv)); /* queued UVs must also be copied */
    }
    ArRenderTexture result = Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid());
    CHECK(ArRenderTexture_IsValid(result));
    /* 4 ordinary vertices, three 4-UV streams/colors, positions only on publication. */
    CHECK(geometry_upload_bytes == (variant ? 160 + 3 * (32 + 16) + (variant == 2 ? 0 : 64) : 640));
    CHECK(SDL_SetRenderTarget(renderer, ArSdlRenderBackend_UnwrapTexture(result)));
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *pixels = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(pixels);
    if (pixels) for (int y = 0; y < kTestHeight; ++y) {
      const uint8_t *row = (const uint8_t *)pixels->pixels + y * pixels->pitch;
      if (!variant) memcpy(reference + y * kTestWidth, row, kTestWidth * 4);
      else CHECK(!memcmp(reference + y * kTestWidth, row, kTestWidth * 4));
    }
    SDL_DestroySurface(pixels); SDL_DestroySurface(raw);
    CHECK(SDL_SetRenderTarget(renderer, NULL));
  }
  CHECK(Sim3DDepthPass_Begin(device, kTestWidth + 8, kTestHeight, kArRenderFilter_Nearest));
  CHECK(!Sim3DDepthPass_MeshReady(mesh)); /* viewport changes need reprojection */
  Sim3DDepthPosition position[4] = {{0,0,.4f}, {4,0,.4f}, {4,4,.4f}, {0,4,.4f}};
  ArRenderPointF uv[4] = {{0}};
  CHECK(!Sim3DDepthPass_UpdateMesh(mesh, position, SIZE_MAX));
  CHECK(Sim3DDepthPass_UpdateMesh(mesh, position, 1));
  CHECK(Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_CloudShadow, mesh, uv, 1, (ArRenderColorF){1,1,1,1}));
  Sim3DDepthPass_DestroyMesh(mesh); /* queued destruction fails closed, never dereferences freed storage */
  CHECK(!ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
  /* Grow both position and UV storage beyond their initial capacities, then
   * shrink the mesh; a held draw uploads only the used UVs and one color. */
  CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
  mesh = Sim3DDepthPass_CreateMesh();
  enum { kLargeQuads = 4097, kLargeVertices = kLargeQuads * 4 };
  Sim3DDepthPosition *large = malloc(kLargeVertices * sizeof(*large));
  ArRenderPointF *large_uv = calloc(kLargeVertices, sizeof(*large_uv));
  CHECK(mesh && large && large_uv);
  if (mesh && large && large_uv) {
    for (int i = 0; i < kLargeVertices; ++i) large[i] = position[i % 4];
    for (int pass = 0; pass < 3; ++pass) {
      const size_t quads = pass == 0 ? kLargeQuads : 1;
      CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
      geometry_upload_bytes = 0;
      if (pass < 2) CHECK(Sim3DDepthPass_UpdateMesh(mesh, large, quads));
      CHECK(Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_CloudShadow, mesh, large_uv,
          quads, (ArRenderColorF){0, 0, 0, .5f}));
      CHECK(ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
      CHECK(geometry_upload_bytes == quads * 4 * (pass < 2 ? 24 : 8) + 16);
    }
  }
  free(large); free(large_uv);
  Sim3DDepthPass_DestroyMesh(mesh);
  Sim3DDepthPass_Reset(device);
}

/* Independent CPU oracle, including wrap of ALL original corners, clamp
 * before clip interpolation, and the old difference-form arithmetic. */
static void SphericalReference(const Sim3DDepthSphericalQuad *quad,
    const Sim3DDepthSphericalSample *sample, Sim3DDepthVertex out[4]) {
  const float pi = 3.14159265358979323846f;
  ArRenderPointF uv[4];
  float minimum = 1, maximum = 0;
  for (int p = 0; p < 4; ++p) {
    const float *n = quad->normals[p], *r = sample->rotation;
    const float x = n[0] * r[0] + n[2] * r[1];
    const float z = n[2] * r[0] - n[0] * r[1];
    const float y = n[1] * r[2] + z * r[3];
    const float zz = z * r[2] - n[1] * r[3];
    uv[p] = (ArRenderPointF){(atan2f(zz, x) + pi) / (2 * pi) + sample->offset.x,
      acosf(fminf(1, fmaxf(-1, y))) / pi + sample->offset.y};
    uv[p].x -= floorf(uv[p].x);
    minimum = fminf(minimum, uv[p].x); maximum = fmaxf(maximum, uv[p].x);
  }
  for (int p = 0; p < 4; ++p) {
    if (maximum - minimum > .5f && uv[p].x < .5f) uv[p].x += 1;
    uv[p].x = (sample->atlas.x + uv[p].x * (sample->atlas.w - 1) + .5f) / sample->texture_size.x;
    uv[p].y = (sample->atlas.y + fminf(1, fmaxf(0, uv[p].y)) * (sample->atlas.h - 1) + .5f) /
        sample->texture_size.y;
  }
  for (int p = 0; p < 4; ++p) {
    ArRenderPointF mapped = uv[p];
    if (quad->triangle) {
      mapped = uv[0];
      for (int j = 0; j < 2; ++j) {
        mapped.x += quad->weights[p][j] * (uv[quad->triangle + j].x - uv[0].x);
        mapped.y += quad->weights[p][j] * (uv[quad->triangle + j].y - uv[0].y);
      }
    }
    out[p] = (Sim3DDepthVertex){quad->positions[p].x, quad->positions[p].y,
      quad->positions[p].depth, sample->color, mapped};
  }
}

static void TestSphericalSamples(ArRenderDevice *device, SDL_Renderer *renderer) {
  enum { kQuads = 4, kChartWidth = 33, kChartHeight = 17, kWidth = kChartWidth * 2,
    kHeight = kChartHeight * 3 };
  uint32_t texels[kWidth * kHeight];
  for (int y = 0; y < kHeight; ++y) for (int x = 0; x < kWidth; ++x)
    texels[y * kWidth + x] = AtlasTestPixel(x % kChartWidth, y, 137);
  const ArRenderRectI full = {0, 0, kWidth, kHeight};
  Sim3DDepthSphericalQuad quads[kQuads] = {0};
  for (int i = 0; i < kQuads; ++i) {
    Sim3DDepthVertex rect[4];
    MakeRect(rect, (float)(i * 8), 0, (float)(i * 8 + 8), kTestHeight, .5f,
        (ArRenderColorF){1, 1, 1, 1});
    quads[i].triangle = i % 3;
    for (int p = 0; p < 4; ++p) {
      quads[i].positions[p] = (Sim3DDepthPosition){rect[p].x, rect[p].y, rect[p].depth};
      const float longitude = 3.10f + p * .04f - i * 2.13f;
      const float latitude = .13f + i * .86f + (p / 2) * .24f;
      quads[i].normals[p][0] = sinf(latitude) * cosf(longitude);
      quads[i].normals[p][1] = cosf(latitude);
      quads[i].normals[p][2] = sinf(latitude) * sinf(longitude);
      quads[i].weights[p][0] = .1f + (p & 1) * .35f;
      quads[i].weights[p][1] = .15f + (p / 2) * .3f;
    }
  }
  /* Exact pole and signed-zero input are kept off the longitude seam in the
   * test texture's repeated chart; GPU atan(0,0) must never produce NaNs. */
  quads[3].normals[0][0] = quads[3].normals[0][2] = 0;
  quads[3].normals[0][1] = 1;
  Sim3DDepthMesh *mesh = NULL;
  unsigned maximum_error = 0, changed = 0;
  for (int frame = 0; frame < 24; ++frame) {
    Sim3DDepthSphericalSample sample = {
      .rotation = {cosf(frame * .41f), sinf(frame * .41f), cosf(frame * .19f), sinf(frame * .19f)},
      .offset = {frame * .067f - .31f, (frame % 5 - 2) * .13f},
      .texture_size = {kWidth, kHeight}, .atlas = {0, 0, kChartWidth, kChartHeight},
      .color = {.7f, .9f, .8f, .6f},
    };
    uint32_t reference[kTestWidth * kTestHeight];
    for (int variant = 0; variant < 2; ++variant) {
      if (frame == 12 && variant == 0) Sim3DDepthPass_Reset(device);
      CHECK(Sim3DDepthPass_UploadAtlasRegions(device, kSim3DDepthPass_Cloud,
          texels, kWidth, kHeight, kWidth * 4, &full, 1));
      CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
      CHECK(AppendRect(kSim3DDepthPass_DepthOccluder, 0, 0, 3, kTestHeight, .1f,
          (ArRenderColorF){1, 1, 1, 1}));
      geometry_upload_bytes = 0;
      if (variant) {
        if (!mesh) mesh = Sim3DDepthPass_CreateSphericalMesh();
        CHECK(mesh);
        CHECK(Sim3DDepthPass_MeshReady(mesh) == (frame != 0 && frame != 12));
        if (!Sim3DDepthPass_MeshReady(mesh)) {
          CHECK(!Sim3DDepthPass_UpdateSphericalMesh(mesh, quads, SIZE_MAX));
          Sim3DDepthSphericalQuad copied[kQuads];
          memcpy(copied, quads, sizeof(copied));
          CHECK(Sim3DDepthPass_UpdateSphericalMesh(mesh, copied, kQuads));
          memset(copied, 0, sizeof(copied)); /* publication must copy */
        }
        CHECK(!Sim3DDepthPass_UpdateMesh(mesh, quads[0].positions, 1));
      }
      for (int bank = 0; bank < 3; ++bank) {
        sample.atlas.y = bank * kChartHeight;
        if (variant) {
          CHECK(!Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_Solid, mesh, &sample));
          CHECK(Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
          CHECK(!Sim3DDepthPass_UpdateSphericalMesh(mesh, quads, kQuads));
        } else {
          Sim3DDepthVertex ordinary[kQuads * 4];
          for (int i = 0; i < kQuads; ++i) SphericalReference(&quads[i], &sample, ordinary + i * 4);
          CHECK(Sim3DDepthPass_AppendQuads(kSim3DDepthPass_CloudShadow, ordinary, kQuads));
        }
      }
      ArRenderTexture result = Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid());
      CHECK(ArRenderTexture_IsValid(result));
      if (variant) CHECK(geometry_upload_bytes == 160 + 48 + ((frame == 0 || frame == 12) ? kQuads * 160 : 0));
      CHECK(SDL_SetRenderTarget(renderer, ArSdlRenderBackend_UnwrapTexture(result)));
      SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
      SDL_Surface *pixels = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
      CHECK(pixels);
      if (pixels) for (int y = 0; y < kTestHeight; ++y) for (int x = 0; x < kTestWidth; ++x) {
        const uint32_t pixel = ReadArgb(pixels, x, y);
        if (!variant) reference[y * kTestWidth + x] = pixel;
        else {
          const uint32_t expected = reference[y * kTestWidth + x];
          changed += pixel != expected;
          for (int shift = 0; shift < 32; shift += 8) {
            const unsigned difference = (unsigned)abs((int)((pixel >> shift) & 255) - (int)((expected >> shift) & 255));
            if (difference > maximum_error) maximum_error = difference;
          }
          if (x < 3) CHECK(pixel == 0); /* unchanged depth rejection */
        }
      }
      SDL_DestroySurface(pixels); SDL_DestroySurface(raw);
      CHECK(SDL_SetRenderTarget(renderer, NULL));
    }
  }
  printf("spherical samples: max channel error=%u/255, changed pixels=%u/12288\n", maximum_error, changed);
  CHECK(maximum_error <= 2); /* cross-device transcendental/UNORM rounding, not geometry changes */
  CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
  Sim3DDepthSphericalSample sample = {.rotation = {1,0,1,0}, .texture_size = {kWidth,kHeight},
    .atlas = {0,0,kChartWidth,kChartHeight}, .color = {1,1,1,.5f}};
  sample.offset.x = NAN;
  CHECK(!Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
  sample.offset.x = 0; sample.atlas.x = 1;
  CHECK(!Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
  sample.atlas.x = 0;
  sample.color.a = INFINITY;
  CHECK(!Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
  sample.color.a = .5f;
  CHECK(Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
  memset(&sample, 0, sizeof(sample));
  Sim3DDepthPass_DestroyMesh(mesh);
  CHECK(!ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
  /* Instance buffers use a different stride from ordinary retained positions.
   * Grow, shrink, reuse, invalidate viewport, and reject malformed provenance. */
  CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
  mesh = Sim3DDepthPass_CreateSphericalMesh();
  enum { kLargeQuads = 4097 };
  Sim3DDepthSphericalQuad *large = malloc(kLargeQuads * sizeof(*large));
  CHECK(mesh && large);
  if (mesh && large) {
    for (int i = 0; i < kLargeQuads; ++i) large[i] = quads[i % kQuads];
    sample = (Sim3DDepthSphericalSample){.rotation = {1,0,1,0}, .texture_size = {kWidth,kHeight},
      .atlas = {0,0,kChartWidth,kChartHeight}, .color = {1,1,1,.01f}};
    for (int pass = 0; pass < 3; ++pass) {
      const size_t count = pass ? kQuads : kLargeQuads;
      CHECK(Sim3DDepthPass_Begin(device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
      geometry_upload_bytes = 0;
      if (pass < 2) CHECK(Sim3DDepthPass_UpdateSphericalMesh(mesh, large, count));
      CHECK(Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
      CHECK(ArRenderTexture_IsValid(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid())));
      CHECK(geometry_upload_bytes == (pass < 2 ? count * 160 : 0) + 16);
    }
    CHECK(Sim3DDepthPass_Begin(device, kTestWidth + 8, kTestHeight, kArRenderFilter_Nearest));
    CHECK(!Sim3DDepthPass_MeshReady(mesh));
    large[0].triangle = 3;
    CHECK(!Sim3DDepthPass_UpdateSphericalMesh(mesh, large, 1));
    large[0].triangle = 0; large[0].normals[0][1] = NAN;
    CHECK(!Sim3DDepthPass_UpdateSphericalMesh(mesh, large, 1));
    large[0] = quads[0]; large[0].weights[0][1] = INFINITY;
    CHECK(!Sim3DDepthPass_UpdateSphericalMesh(mesh, large, 1));
    CHECK(!Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow, mesh, &sample));
  }
  free(large);
  Sim3DDepthPass_DestroyMesh(mesh);
  Sim3DDepthPass_Reset(device);
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SIM3D GPU test skipped: SDL video unavailable: %s\n",
            SDL_GetError());
    return kSkipNoGpuRenderer;
  }
  SDL_Window *window = SDL_CreateWindow(
      "SIM3D depth integration", 64, 64, SDL_WINDOW_HIDDEN);
  if (!window) {
    fprintf(stderr, "SIM3D GPU test skipped: window unavailable: %s\n",
            SDL_GetError());
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  SDL_Renderer *renderer = CreateProductionRenderer(window);
  if (!renderer) {
    fprintf(stderr, "SIM3D GPU test skipped: GPU renderer unavailable: %s\n",
            SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  SDL_GPUDevice *device = SDL_GetGPURendererDevice(renderer);
  CHECK(device != NULL);
  printf("SIM3D depth renderer=%s gpu=%s formats=0x%x\n",
         SDL_GetRendererName(renderer),
         device ? SDL_GetGPUDeviceDriver(device) : "none",
         device ? (unsigned)SDL_GetGPUShaderFormats(device) : 0u);

  ArRenderDevice render_device = {0};
  ArSdlRenderBackend render_backend = {0};
  CHECK(ArSdlRenderBackend_Bind(
      &render_device, &render_backend, renderer));
  CHECK(Sim3DDepthPass_Require(&render_device));
  CHECK(Sim3DDepthPass_Begin(
      &render_device, kTestWidth, kTestHeight, kArRenderFilter_Nearest));
  /* The invisible near quad writes only depth over the left half. The farther
   * red solid should survive on the right and be rejected on the left. This
   * exercises pipeline creation, transfer buffers, D32 comparison, resource
   * cycling, GPU submission, and the SDL_GPUTexture -> SDL_Texture wrapper. */
  CHECK(AppendRect(
      kSim3DDepthPass_DepthOccluder,
      0.0f, 0.0f, kTestWidth / 2.0f, (float)kTestHeight,
      0.25f, (ArRenderColorF){1.0f, 1.0f, 1.0f, 1.0f}));
  /* Two adjacent solids pin the generic batch seam used when the portable SIM
   * renderer replays retained projected geometry. The backend still owns its
   * GPU vertex representation and must make the pair indistinguishable from
   * two ordinary AppendQuad calls. */
  Sim3DDepthVertex solid_batch[8];
  MakeRect(&solid_batch[0],
           0.0f, 0.0f, kTestWidth / 2.0f, (float)kTestHeight,
           0.75f, (ArRenderColorF){1.0f, 0.0f, 0.0f, 1.0f});
  MakeRect(&solid_batch[4],
           kTestWidth / 2.0f, 0.0f,
           (float)kTestWidth, (float)kTestHeight,
           0.75f, (ArRenderColorF){1.0f, 0.0f, 0.0f, 1.0f});
  CHECK(Sim3DDepthPass_AppendQuads(
      kSim3DDepthPass_Solid, solid_batch, 2));
  ArRenderTexture output_handle = Sim3DDepthPass_Submit(
      &render_device, ArRenderTexture_Invalid());
  SDL_Texture *output = ArSdlRenderBackend_UnwrapTexture(output_handle);
  CHECK(output != NULL);

  SDL_Surface *readback = NULL;
  SDL_Surface *argb = NULL;
  if (output) {
    CHECK(SDL_GetRendererFromTexture(output) == renderer);
    CHECK(SDL_SetRenderTarget(renderer, output));
    readback = SDL_RenderReadPixels(renderer, NULL);
    CHECK(readback != NULL);
    argb = readback
        ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(argb != NULL);
  }
  if (argb) {
    const uint32_t occluded = ReadArgb(argb, kTestWidth / 4, kTestHeight / 2);
    const uint32_t visible = ReadArgb(
        argb, kTestWidth * 3 / 4, kTestHeight / 2);
    CHECK((occluded >> 24) <= 1u);
    CHECK((visible >> 24) >= 254u);
    CHECK(((visible >> 16) & 0xffu) >= 254u);
    CHECK((visible & 0x0000ffffu) == 0u);
  }

  SDL_DestroySurface(argb);
  SDL_DestroySurface(readback);
  CHECK(SDL_SetRenderTarget(renderer, NULL));
  Sim3DDepthPass_Reset(&render_device);
  TestColoredTerrainOcclusion(&render_device, renderer);
  TestAtlasRegionPacking(&render_device, renderer);
  TestBatchCopyAndGrowth(&render_device, renderer);
  TestRetainedSamples(&render_device, renderer);
  TestSphericalSamples(&render_device, renderer);
  TestModelMesh(&render_device, renderer);
  TestHardwareClippedModels(&render_device, renderer);
  TestRadialModels(&render_device, renderer);
  TestSolidRanges(&render_device, renderer, false);
  TestSolidRanges(&render_device, renderer, true);
  TestCapturedGround(&render_device, renderer);
  TestGroupedTownLayers(&render_device, renderer);
  TestIndependentRetainedBudgets(&render_device, renderer);
  TestGeometryInFlight(&render_device, renderer);
  ArRenderDevice_Reset(&render_device);
  SDL_DestroyRenderer(renderer);
  TestOrderedSubmission(window);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("SIM3D depth GPU integration test: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
