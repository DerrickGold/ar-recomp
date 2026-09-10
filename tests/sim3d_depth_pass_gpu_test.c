#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/render_sdl_internal.h"
#include "sim/sim3d_depth_pass.h"

enum {
  kTestWidth = 32,
  kTestHeight = 16,
  kSkipNoGpuRenderer = 77,
};

static int failures;
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
  (void)vertices;
  (void)indices;
}

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
  ArRenderDevice_Reset(&render_device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("SIM3D depth GPU integration test: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
