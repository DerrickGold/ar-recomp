#include <SDL3/SDL.h>

#include <stdint.h>
#include <math.h>
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
static uint64_t geometry_upload_bytes;
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
  ArRenderDevice_Reset(&render_device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("SIM3D depth GPU integration test: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
