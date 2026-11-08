#include <math.h>
#include <inttypes.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_gpu_policy.h"
#include "action/action_voxel.h"
#include "action/action_trunk.h"
#include "action/action_voxel_internal.h"
#include "platform/sdl/action_effect_source_sdl.h"
#include "platform/sdl/render_sdl_internal.h"
#include "present/present.h"

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

static uint64_t SourceFixtureHash(uint64_t hash, const void *bytes, size_t size) {
  const unsigned char *data = bytes;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

/* Keep sky and effects as separate ordered scopes, as the compositor does.
 * In particular, never download intermediate vertices while a scope is open:
 * all ten bands must preserve uploaded bytes and ordered results until End. */
static void TestResidentSkyBatch(ArRenderDevice *device, SDL_Renderer *renderer,
    ArGpuEffectSource *effect, SDL_GPUBuffer *motion, DioramaProjection *view,
    const ActionEffectSourceBatch *batch, float phase, unsigned reference_case) {
  ActionEffectSourceBatch draw_batch = *batch;
  static ActionEffectSourceLightJob lights[2];
  SDL_GPUTransferBuffer *light_download = NULL;
  if (reference_case == 9) {
    /* Job zero is the existing fully lit, unoccluded kind-0 fixture. Job one
     * has an anchored kind-2 receiver whose clip excludes every sample. Its
     * scratch writes must neither erase job zero nor retain old illumination. */
    CHECK(batch->light_count == 1 && batch->count == 6);
    lights[0] = lights[1] = batch->lights[0];
    lights[1].data.meta[1] = 2;
    const float excluded[] = {1000,1000,1010,1010};
    memcpy(lights[1].data.clip, excluded, sizeof(excluded));
    draw_batch.lights = lights;
    draw_batch.light_count = draw_batch.light_capacity = 2;
    const SDL_GPUTransferBufferCreateInfo desc = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 12 * sizeof(float)};
    light_download = SDL_CreateGPUTransferBuffer(effect->device, &desc);
    CHECK(light_download != NULL);
    if (!light_download) return;
  }
  SDL_Surface *source = SDL_CreateSurface(256, 160, SDL_PIXELFORMAT_RGBA32);
  CHECK(source != NULL);
  if (!source) { SDL_ReleaseGPUTransferBuffer(effect->device, light_download); return; }
  for (int y = 0; y < source->h; ++y) for (int x = 0; x < source->w; ++x) {
    unsigned char *pixel = (unsigned char *)source->pixels + y * source->pitch + x * 4;
    pixel[0] = (unsigned char)(20 + ((x * 17 + y * 3) & 31));
    pixel[1] = (unsigned char)(30 + ((x * 5 + y * 11) & 31));
    pixel[2] = (unsigned char)(40 + ((x * 7 + y * 13) & 31));
    pixel[3] = 255;
  }
  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, source);
  SDL_DestroySurface(source);
  CHECK(texture && SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST));
  if (!texture) { SDL_ReleaseGPUTransferBuffer(effect->device, light_download); return; }
  const ArRenderVertex2D vertices[] = {
      {{0,0},{1,1,1,1},{0,0}}, {{64,0},{1,1,1,1},{1,0}},
      {{64,64},{1,1,1,1},{1,1}}, {{0,64},{1,1,1,1},{0,1}}};
  const int32_t indices[] = {0,1,2,0,2,3};
  DioramaSkyboxSourceDraw sky = {.vertices = vertices, .indices = indices,
      .vertex_count = 4, .index_count = 6, .blend = kArRenderBlendMode_Opaque,
      .texture_width = 256, .texture_height = 160,
      .mapping = view->bg2_skybox.resident};
  SDL_Surface *images[2] = {NULL, NULL}, *sky_image = NULL;
  const bool original_batching = effect->batching_enabled;
  uint64_t hashes[2] = {0};
  unsigned changed_pixels = 0;
  for (unsigned mode = 0; mode < 2; ++mode) {
    effect->batching_enabled = mode != 0;
    CHECK(!effect->commands && !effect->batch_open);
    CHECK(SDL_SetRenderDrawColor(renderer, 7, 11, 13, 255) && SDL_RenderClear(renderer));
    uint64_t submitted = effect->submissions, draws = effect->draws;
    bool began = ArGpuEffectSource_BeginBatch(effect);
    CHECK(began);
    if (!began) break;
    for (unsigned band = 0; band < 10; ++band) {
      const DioramaSkyboxRowShift shifts[] = {
          {band * 16, band * 16 + 8, -.375f},
          {band * 16 + 8, (band + 1) * 16, .625f}};
      sky.band = band;
      sky.row_shifts = shifts;
      sky.row_shift_count = 2;
      const bool ok = ArGpuEffectSource_Skybox(effect, device, motion, 6, phase,
          ArSdlRenderBackend_BorrowTexture(texture), &sky);
      CHECK(ok);
      if (!ok) break;
    }
    CHECK(effect->submissions == submitted + (mode ? 0 : 10));
    const bool sky_ended = ArGpuEffectSource_EndBatch(effect);
    CHECK(sky_ended && !effect->commands && !effect->batch_open && !effect->command_writes);
    CHECK(effect->submissions == submitted + (mode ? 1 : 10));
    CHECK(effect->draws == draws + 10);
    if (!sky_ended) break;
    if (mode == 0) {
      SDL_Surface *pixels = SDL_RenderReadPixels(renderer, NULL);
      sky_image = pixels ? SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_RGBA32) : NULL;
      CHECK(sky_image && sky_image->w == 64 && sky_image->h == 64);
      SDL_DestroySurface(pixels);
    }
    submitted = effect->submissions;
    draws = effect->draws;
    began = ArGpuEffectSource_BeginBatch(effect);
    CHECK(began);
    if (!began) break;
    for (unsigned band = 0; band < 10; ++band) {
      view->bg2_skybox.active_band = (int)band;
      const bool ok = ArGpuEffectSource_Draw(effect, device, motion, 1u << 6, phase,
          &draw_batch, view, NULL, kArRenderBlendMode_Alpha, 1);
      CHECK(ok);
      if (!ok) break;
    }
    CHECK(effect->submissions == submitted + (mode ? 0 : 9));
    const bool effects_ended = ArGpuEffectSource_EndBatch(effect);
    CHECK(effects_ended && !effect->commands && !effect->batch_open && !effect->command_writes);
    CHECK(effect->submissions == submitted + (mode ? 1 : 9));
    CHECK(effect->draws == draws + 9); /* Raw-empty band four stays empty. */
    if (!effects_ended) break;
    if (light_download) {
      SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(effect->device);
      SDL_GPUCopyPass *copy = command ? SDL_BeginGPUCopyPass(command) : NULL;
      CHECK(copy != NULL);
      if (!copy) { if (command) SDL_CancelGPUCommandBuffer(command); break; }
      for (unsigned job = 0; job < 2; ++job) {
        const SDL_GPUBufferRegion from = {.buffer = effect->moon_light,
            .offset = job * kActionSourceMaxLightPoints * sizeof(float), .size = 6 * sizeof(float)};
        const SDL_GPUTransferBufferLocation to = {
            .transfer_buffer = light_download, .offset = job * 6 * sizeof(float)};
        SDL_DownloadFromGPUBuffer(copy, &from, &to);
      }
      SDL_EndGPUCopyPass(copy);
      SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
      const bool ready = fence && SDL_WaitForGPUFences(effect->device, true, &fence, 1);
      CHECK(ready);
      if (fence) SDL_ReleaseGPUFence(effect->device, fence);
      if (!ready) break;
      const float *values = SDL_MapGPUTransferBuffer(effect->device, light_download, false);
      CHECK(values != NULL);
      if (!values) break;
      for (unsigned sample = 0; sample < 6; ++sample) {
        CHECK(values[sample] == 1);
        CHECK(values[6 + sample] == 0);
      }
      SDL_UnmapGPUTransferBuffer(effect->device, light_download);
      printf("[sky-batch-light] reference-case=%02u mode=%u jobs=2 expected=1,0\n", reference_case, mode);
    }
    SDL_Surface *pixels = SDL_RenderReadPixels(renderer, NULL);
    images[mode] = pixels ? SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(pixels);
    CHECK(images[mode] && images[mode]->w == 64 && images[mode]->h == 64);
    if (!images[mode]) break;
    hashes[mode] = UINT64_C(14695981039346656037);
    for (int y = 0; y < 64; ++y) {
      const unsigned char *row = (const unsigned char *)images[mode]->pixels + y * images[mode]->pitch;
      hashes[mode] = SourceFixtureHash(hashes[mode], row, 64 * 4);
      if (mode && images[0])
        CHECK(!memcmp(row, (const unsigned char *)images[0]->pixels + y * images[0]->pitch, 64 * 4));
      if (!mode && sky_image) {
        const unsigned char *background = (const unsigned char *)sky_image->pixels + y * sky_image->pitch;
        for (int x = 0; x < 64; ++x) changed_pixels += memcmp(row + x * 4, background + x * 4, 4) != 0;
      }
    }
  }
  CHECK(images[0] && images[1] && changed_pixels > 0);
  CHECK(hashes[0] == hashes[1]);
  printf("[sky-batch-case] reference-case=%02u phase=%.2f scalar=%016" PRIx64
         " batched=%016" PRIx64 " effect-pixels=%u sky-draws=10 effect-draws=9 scopes=2\n",
      reference_case, phase, hashes[0], hashes[1], changed_pixels);
  effect->batching_enabled = original_batching;
  SDL_DestroySurface(sky_image);
  for (unsigned i = 0; i < 2; ++i) SDL_DestroySurface(images[i]);
  SDL_DestroyTexture(texture);
  SDL_ReleaseGPUTransferBuffer(effect->device, light_download);
}

/* The same fixture source is linked with reference and candidate shader blobs.
 * Printed hashes compare exact downloaded vertex/light bytes and RGBA pixels;
 * the independent checks below only establish meaningful, finite output. */
static void TestResidentSkyWindow(ArRenderDevice *device, SDL_GPUDevice *gpu,
    SDL_Renderer *renderer, ArGpuEffectSource *effect, SDL_GPUBuffer *motion,
    SDL_GPUTransferBuffer *upload) {
  enum { kPrimitives = 6, kVertexBytes = kPrimitives * 15 * 32,
         kLightBytes = kPrimitives * sizeof(float) };
  const SDL_GPUTransferBufferCreateInfo desc = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = kVertexBytes + kLightBytes};
  SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu, &desc);
  CHECK(download != NULL);
  if (!download) return;
  int32_t *vectors = SDL_MapGPUTransferBuffer(gpu, upload, true);
  CHECK(vectors != NULL);
  if (!vectors) { SDL_ReleaseGPUTransferBuffer(gpu, download); return; }
  memset(vectors, 0, 256);
  const int32_t back[] = {3, -5, -2, 3, 1};
  memcpy(vectors + 6 * 8, back, sizeof(back));
  SDL_UnmapGPUTransferBuffer(gpu, upload);
  SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(gpu);
  CHECK(command != NULL);
  SDL_GPUCopyPass *copy = command ? SDL_BeginGPUCopyPass(command) : NULL;
  CHECK(copy != NULL);
  if (!copy) {
    if (command) SDL_CancelGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(gpu, download);
    return;
  }
  const SDL_GPUTransferBufferLocation motion_from = {.transfer_buffer = upload};
  const SDL_GPUBufferRegion motion_to = {.buffer = motion, .size = 256};
  SDL_UploadToGPUBuffer(copy, &motion_from, &motion_to, true);
  SDL_EndGPUCopyPass(copy);
  CHECK(SDL_SubmitGPUCommandBuffer(command));

  for (unsigned scenario = 0; scenario < 14; ++scenario) {
    const unsigned reference_case = scenario < 12 ? scenario : scenario == 12 ? 9 : 11;
    const bool moon = reference_case >= 8;
    const unsigned kind = reference_case < 10 ? 0 : reference_case - 9;
    const bool anchored = moon ? kind == 0 : (reference_case & 4) != 0;
    const bool narrow = moon ? reference_case == 9 || reference_case == 10 : (reference_case & 2) != 0;
    const float phase = reference_case & 1 ? .75f : .25f;
    DioramaProjection view = {.valid = true, .texture_width = 256, .texture_height = 160,
        .output_width = 64, .output_height = 64, .aspect_x = 2, .height_scale = 2,
        .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        .bg1_plane = {.valid = true, .u1 = 1, .v1 = 1},
        .bg2_skybox = {.count = 10, .active_band = 0, .motion_source = 0,
            .resident = {.meta = {10,256,160,1}, .output = {64,64},
                .vertical = {0,160,0,1}, .follow = {0,32,6,8}, .fit = {128}}}};
    for (unsigned band = 0; band < 10; ++band) {
      const float width = narrow ? (band == 7 ? 64 : 96 + 8 * (band % 3)) : 192 + 8 * (band % 3);
      const float left = (narrow ? 80 : 16) + 12 * ((int)(band % 3) - 1);
      float *raw = view.bg2_skybox.resident.bands[band];
      raw[0] = left / 256; raw[1] = (left + width) / 256;
      raw[2] = band * 16; raw[3] = (band + 1) * 16;
      if (band == 4) raw[1] = raw[0]; /* A genuinely empty source band. */
      view.bg2_skybox.bands[band] = (DioramaSkyboxBandProjection){
          left, band * 16, band == 4 ? left : left + width, (band + 1) * 16,
          band / 10.f, (band + 1) / 10.f};
    }
    ActionEffectSourcePrimitive primitives[kPrimitives];
    static ActionEffectSourceLightJob light;
    const ActionMoonlightOcclusion occlusion = {.valid = true};
    light = (ActionEffectSourceLightJob){.occlusion = &occlusion,
        .data = {.meta = {kPrimitives,kind,10}, .light = {128,24,2,6},
            .receiver = {128,24,2,anchored ? 6 : 4}, .clip = {-256,-160,256,320},
            .transport = {.5f,.5f,.5f}, .surface = {.5f,.5f},
            .rays = {{0,10,1},{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,1,0}}}};
    for (unsigned i = 0; i < kPrimitives; ++i) {
      const float x = -36 + (float)i * 12, y = 8 + (float)i * 16;
      light.data.points[i][0] = x;
      light.data.points[i][1] = y;
      primitives[i] = (ActionEffectSourcePrimitive){
          .meta = {moon ? kActionSourceLitTriangle : kActionSourceTriangle,2},
          .origin = {128,24,anchored ? 6 : 4}, .clip = {-256,-160,256,320},
          .points = {{x,y,moon ? i+1 : 0},{x+6,y,moon ? i+1 : 0},{x,y+8,moon ? i+1 : 0}},
          .colors = {{.8f,.3f,.2f,.6f},{.8f,.3f,.2f,.6f},{.8f,.3f,.2f,.6f}},
          .extra = {1}};
    }
    const ActionEffectSourceBatch batch = {.primitives = primitives,
        .count = kPrimitives, .capacity = kPrimitives,
        .lights = moon ? &light : NULL, .light_count = moon, .light_capacity = moon,
        .revision = UINT64_C(0x534b5900) + scenario, .packet_slot = 1};
    if (scenario >= 12) {
      TestResidentSkyBatch(device, renderer, effect, motion, &view, &batch, phase, reference_case);
      continue;
    }
    CHECK(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) && SDL_RenderClear(renderer));
    uint64_t vertex_hash = UINT64_C(14695981039346656037);
    uint64_t light_hash = UINT64_C(14695981039346656037);
    unsigned active_vertices = 0, drawn_bands = 0, skipped_bands = 0;
    unsigned light_zero = 0, light_positive = 0;
    for (unsigned band = 0; band < 10; ++band) {
      view.bg2_skybox.active_band = (int)band;
      const uint64_t previous_draws = effect->draws;
      const bool drawn = ArGpuEffectSource_Draw(effect, device, motion, 1u << 6, phase,
          &batch, &view, NULL, kArRenderBlendMode_Alpha, 1);
      CHECK(drawn);
      if (!drawn) break;
      CHECK(effect->draws == previous_draws + (band != 4));
      if (band == 4) { ++skipped_bands; continue; }
      ++drawn_bands;
      command = SDL_AcquireGPUCommandBuffer(gpu);
      CHECK(command != NULL);
      copy = command ? SDL_BeginGPUCopyPass(command) : NULL;
      CHECK(copy != NULL);
      if (!copy) { if (command) SDL_CancelGPUCommandBuffer(command); break; }
      const SDL_GPUBufferRegion vertices = {.buffer = effect->vertices, .size = kVertexBytes};
      const SDL_GPUTransferBufferLocation vertices_to = {.transfer_buffer = download};
      SDL_DownloadFromGPUBuffer(copy, &vertices, &vertices_to);
      if (moon) {
        const SDL_GPUBufferRegion lights = {.buffer = effect->moon_light, .size = kLightBytes};
        const SDL_GPUTransferBufferLocation lights_to = {.transfer_buffer = download, .offset = kVertexBytes};
        SDL_DownloadFromGPUBuffer(copy, &lights, &lights_to);
      }
      SDL_EndGPUCopyPass(copy);
      SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
      const bool ready = fence && SDL_WaitForGPUFences(gpu, true, &fence, 1);
      CHECK(ready);
      if (fence) SDL_ReleaseGPUFence(gpu, fence);
      if (!ready) break;
      const float *output = SDL_MapGPUTransferBuffer(gpu, download, false);
      CHECK(output != NULL);
      if (!output) break;
      vertex_hash = SourceFixtureHash(vertex_hash, output, kVertexBytes);
      for (unsigned i = 0; i < kVertexBytes / sizeof(float); ++i) CHECK(isfinite(output[i]));
      for (unsigned i = 0; i < kPrimitives * 15; ++i) {
        const float alpha = output[i * 8 + 7];
        if (alpha > 0) {
          ++active_vertices;
          if (!moon || kind == 0) CHECK(alpha == .6f);
        }
      }
      if (moon) {
        const float *values = output + kVertexBytes / sizeof(float);
        light_hash = SourceFixtureHash(light_hash, values, kLightBytes);
        for (unsigned i = 0; i < kPrimitives; ++i) {
          CHECK(isfinite(values[i]) && values[i] >= 0 && values[i] <= 1);
          light_zero += values[i] == 0;
          light_positive += values[i] > 0;
          /* No blockers: the anchored kind-0 transport is exactly lit. */
          if (kind == 0) CHECK(values[i] == 1);
        }
      }
      SDL_UnmapGPUTransferBuffer(gpu, download);
    }
    CHECK(drawn_bands == 9 && skipped_bands == 1 && active_vertices > 0);
    if (moon) CHECK(light_positive > 0);
    if (moon && kind > 0) CHECK(light_zero > 0);
    SDL_Surface *pixels = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *rgba = pixels ? SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_RGBA32) : NULL;
    CHECK(rgba && rgba->w == 64 && rgba->h == 64);
    uint64_t pixel_hash = UINT64_C(14695981039346656037);
    unsigned lit_pixels = 0;
    if (rgba) {
      for (int y = 0; y < rgba->h; ++y) {
        const unsigned char *row = (const unsigned char *)rgba->pixels + y * rgba->pitch;
        pixel_hash = SourceFixtureHash(pixel_hash, row, (size_t)rgba->w * 4);
        for (int x = 0; x < rgba->w; ++x) lit_pixels += row[x * 4] || row[x * 4 + 1] || row[x * 4 + 2];
      }
    }
    CHECK(lit_pixels > 0);
    printf("[sky-window-case] case=%02u kind=%s%u anchor=%u fit=%s phase=%.2f "
           "vertices=%016" PRIx64 " light=%016" PRIx64 " pixels=%016" PRIx64
           " active-vertices=%u lit-pixels=%u drawn-bands=%u empty-bands=%u\n",
        scenario, moon ? "moon" : "plain", moon ? kind : 0, (unsigned)anchored, narrow ? "clamped" : "unclamped",
        phase, vertex_hash, light_hash, pixel_hash, active_vertices, lit_pixels, drawn_bands, skipped_bands);
    SDL_DestroySurface(rgba);
    SDL_DestroySurface(pixels);
  }
  SDL_ReleaseGPUTransferBuffer(gpu, download);
}

/* Download only in this oracle: live source projection never reads vertices
 * or motion back. Exercise particle sizing, leaf geometry and rejected motion
 * at all interpolation phases against independently computed screen points. */
static void TestSourcePrimitives(ArRenderDevice *device, SDL_GPUDevice *gpu, SDL_Renderer *renderer) {
  SDL_Texture *previous_target = SDL_GetRenderTarget(renderer);
  ArGpuEffectSource effect = {0};
  CHECK(ArGpuEffectSource_Init(&effect, gpu));
  if (!effect.device) return;
  SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, 64, 64);
  CHECK(target && SDL_SetRenderTarget(renderer, target));
  const SDL_GPUBufferCreateInfo bi = {.size = 256,
      .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
  SDL_GPUBuffer *motion = SDL_CreateGPUBuffer(gpu, &bi);
  const SDL_GPUTransferBufferCreateInfo ui = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 256};
  const SDL_GPUTransferBufferCreateInfo di = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 90 * 32};
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &ui),
      *download = SDL_CreateGPUTransferBuffer(gpu, &di);
  CHECK(motion && upload && download);
  const DioramaProjection view = {.valid = true, .output_width = 64, .output_height = 64,
      .texture_width = 32, .texture_height = 16,
      .bg2_skybox = {.count = 1, .bands = {{0,0,32,16,0,1}}}};
  ActionEffectSourcePrimitive primitives[6] = {
    {.meta={kActionSourceTriangle,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{0,0},{3,0},{0,3}}, .colors={{1,.2f,.3f,.5f},{1,.2f,.3f,.5f},{1,.2f,.3f,.5f}}},
    {.meta={kActionSourceParticle,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{4,4},{4,3}}, .colors={{.7f,.4f,.2f,.6f}}, .extra={.4f,.7f}},
    {.meta={kActionSourceLeaf,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{8,4},{9,3},{11,3},{12,4},{11,5},{9,5}}, .colors={{.1f,.3f,.2f,.5f},{.6f,.7f,.3f,.2f}}},
    {.meta={kActionSourceQuad,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{2,3},{4,3},{4,5},{2,5}},
      .colors={{1,0,0,1},{0,1,0,1},{0,0,1,1}}, .extra={.1f,.2f,.3f,.4f}},
    {.meta={kActionSourceStar,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{3,4}}, .colors={{1,1,1,1}}, .extra={.5f}},
    {.meta={kActionSourceGlowTriangle,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{3,4},{.25f,2,3,2},{0,0,0,0},{1,0,0,0},{0,0,0,1}},
      .colors={{1,0,0,1},{0,1,0,.5f},{0,0,1,0}}}
  };
  ActionEffectSourceBatch batch = {.primitives=primitives,.count=6,.capacity=6};
  const unsigned particle = 15, leaf = effect.packing_enabled ? 21 : 30;
  const unsigned quad = effect.packing_enabled ? 36 : 45;
  const unsigned star = effect.packing_enabled ? 42 : 60;
  const unsigned glow = effect.packing_enabled ? 54 : 75;
  const float phases[]={0,.25f,.5f,.75f,1};
  const float brightnesses[]={1,.5f,.5f,0,1};
  for (unsigned valid=0; valid<2; ++valid) for (unsigned phase=0; phase<5; ++phase) {
    /* Repaints reuse one GPU packet while motion changes. The next capture
     * changes its revision/color; master brightness may change independently. */
    batch.revision = valid + 1;
    primitives[0].colors[0][0] = valid ? .6f : 1.0f;
    const float brightness = brightnesses[phase];
    ActionEffectSourcePrimitive original[6];
    memcpy(original, primitives, sizeof(original));
    int32_t *vectors = SDL_MapGPUTransferBuffer(gpu, upload, true);
    CHECK(vectors != NULL); if (!vectors) continue;
    memset(vectors,0,256); vectors[6*8]=3; vectors[6*8+1]=-3;
    vectors[6*8+2]=-2; vectors[6*8+3]=2; vectors[6*8+4]=valid;
    SDL_UnmapGPUTransferBuffer(gpu,upload);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation from={.transfer_buffer=upload};
    const SDL_GPUBufferRegion to={.buffer=motion,.size=256};
    SDL_UploadToGPUBuffer(copy,&from,&to,true); SDL_EndGPUCopyPass(copy);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,1u<<6,phases[phase],&batch,&view,NULL,
        kArRenderBlendMode_Add,brightness));
    CHECK(effect.packets[0].revision == batch.revision);
    CHECK(effect.packets[0].vertex_count == (effect.packing_enabled ? 57u : 90u));
    CHECK(memcmp(original, primitives, sizeof(original)) == 0);
    cmd=SDL_AcquireGPUCommandBuffer(gpu); copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion source={.buffer=effect.vertices,.size=90*32};
    const SDL_GPUTransferBufferLocation dest={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&source,&dest); SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1)); SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false); CHECK(out!=NULL);
    if (!out) continue;
    const float dx=valid?(phases[phase]<.5f?-2+3*phases[phase]:-2*(1-phases[phase])):0;
    const float dy=-dx;
    for(unsigned j=0;j<3;++j) {
      CHECK(fabsf(out[j*8]-(5+primitives[0].points[j][0]+dx)*2)<.0001f);
      CHECK(fabsf(out[j*8+1]-(2+primitives[0].points[j][1]+dy)*4)<.0001f);
      for (unsigned c=0;c<4;++c)
        CHECK(fabsf(out[j*8+4+c]-primitives[0].colors[j][c]*(c<3?brightness:1))<.0001f);
    }
    /* Unit X/Y project to 2/4 pixels, so billboard scale is their mean, 3. */
    CHECK(fabsf(out[particle*8]-(9+dx)*2)<.0001f);
    CHECK(fabsf(out[particle*8+1]-((6+dy)*4+2.1f))<.0001f);
    CHECK(fabsf(out[(particle+1)*8]-((9+dx)*2-1.2f))<.0001f);
    CHECK(fabsf(out[leaf*8]-(13+dx)*2)<.0001f);
    CHECK(fabsf(out[(leaf+14)*8]-((14+dx)*2*.72f+(14+dx)*2*.28f))<.0001f);
    CHECK(fabsf(out[quad*8]-(7+dx)*2)<.0001f);
    CHECK(fabsf(out[(quad+5)*8+1]-(7+dy)*4)<.0001f);
    for (unsigned c=0;c<4;++c)
      CHECK(fabsf(out[(quad+5)*8+4+c]-primitives[3].extra[c]*(c<3?brightness:1))<.0001f);
    CHECK(fabsf(out[star*8]-((8+dx)*2+1))<.0001f);
    CHECK(fabsf(out[(star+6)*8+1]-((6+dy)*4+2.7f))<.0001f);
    CHECK(fabsf(out[glow*8]-(8+dx)*2)<.0001f);
    CHECK(fabsf(out[(glow+1)*8]-((8+dx)*2+3))<.0001f); // screen minimum
    CHECK(fabsf(out[(glow+2)*8+1]-((6+dy)*4+8))<.0001f);
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* A retained billboard survives an offscreen anchor while its halo still
   * intersects the capture. Exercise all four edges, including screen-sized
   * radius minima, and reject only the completely outside cases. */
  const float centres[][2]={{-1,6},{33,6},{8,-1},{8,17},{-20,6},{52,6},{8,-20},{8,36}};
  ActionEffectSourcePrimitive halo=primitives[5];
  halo.points[5][0]=halo.points[5][3]=1;
  ActionEffectSourceBatch halo_batch={.primitives=&halo,.count=1,.capacity=1};
  for(unsigned sample=0;sample<8;++sample) for(unsigned phase=0;phase<5;++phase) {
    halo.origin[0]=centres[sample][0]-halo.points[0][0];
    halo.origin[1]=centres[sample][1]-halo.points[0][1];
    halo_batch.revision=1000+sample;
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,1u<<6,phases[phase],&halo_batch,&view,NULL,kArRenderBlendMode_Add,1));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion vertices={.buffer=effect.vertices,.size=15*32};
    const SDL_GPUTransferBufferLocation dest={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&vertices,&dest);SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence&&SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out);
    if(!out)continue;
    const float dx=phases[phase]<.5f?-2+3*phases[phase]:-2*(1-phases[phase]);
    const float x=centres[sample][0]+dx,y=centres[sample][1]-dx;
    const bool visible=x>=-1.5f&&x<=33.5f&&y>=-2&&y<=18;
    if(visible) {
      CHECK(fabsf(out[0]-x*2)<.0001f);
      CHECK(fabsf(out[1]-y*4)<.0001f);
      CHECK(out[7]==1&&out[15]==.5f);
    } else for(unsigned i=0;i<(effect.packing_enabled?3u:15u);++i)CHECK(out[i*8+7]==0);
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* World-space casters have independent camera/apron offsets. Verify both
   * their uploaded bounds and the resulting shadow, including retained
   * packets, fades, and removal of all casters after a populated upload. */
  const DioramaProjection shadow_view = {.valid=true,.texture_width=32,.texture_height=16,
      .output_width=64,.output_height=64,.aspect_x=2,.height_scale=2,
      .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
      .bg1_plane={.valid=true,.u1=1,.v1=1}};
  ActionEffectSourcePrimitive shadow_primitive = {.meta={kActionSourceTriangle,1,0,1},
      .origin={16,0},.clip={-100,-100,100,100},.points={{0,8},{1,8},{0,9}},
      .extra={0,0,1,0}, /* Full-strength shadow transport. */
      .colors={{1,.4f,.2f,.6f},{1,.4f,.2f,.6f},{1,.4f,.2f,.6f}}};
  const ActionEffectSourcePrimitive original_shadow = shadow_primitive;
  ActionMoonlightOcclusion scenery = {.valid=true,.rectangles={{5,8,19,16},{-3,4,1,6}}};
  ActionEffectSourceBatch shadow_batch = {.primitives=&shadow_primitive,.count=1,.capacity=1,
      .context={.ws_extra=9,.bg1_camera_x=4,.ws_extra_top=7,.bg1_camera_y=10}};
  const float shadow_brightness[]={1,1,.5f,.5f,0,1};
  for (unsigned sample=0;sample<6;++sample) {
    scenery.count = sample && sample<5 ? 2 : 0;
    shadow_batch.revision = 100 + scenery.count;
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&shadow_batch,&shadow_view,&scenery,
        kArRenderBlendMode_Add,shadow_brightness[sample]));
    CHECK(memcmp(&original_shadow,&shadow_primitive,sizeof(shadow_primitive)) == 0);
    CHECK(effect.packets[0].shadow_count == scenery.count);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion vertices={.buffer=effect.vertices,.size=15*32};
    const SDL_GPUBufferRegion casters={.buffer=effect.packets[0].occluders,.size=(scenery.count+1)*16};
    const SDL_GPUTransferBufferLocation vertex_to={.transfer_buffer=download},
        caster_to={.transfer_buffer=download,.offset=15*32};
    SDL_DownloadFromGPUBuffer(copy,&vertices,&vertex_to);
    SDL_DownloadFromGPUBuffer(copy,&casters,&caster_to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out!=NULL);
    if(!out)continue;
    const float *rects=out+15*8;
    if (scenery.count) {
      const float expected[]={2,1,24,13, 10,5,24,13, 2,1,6,3};
      for (unsigned i=0;i<12;++i) CHECK(rects[i] == expected[i]);
    }
    for (unsigned j=0;j<3;++j) for (unsigned c=0;c<4;++c) {
      const float factor=c<3?shadow_brightness[sample]*(scenery.count?.14f:1):1;
      CHECK(fabsf(out[j*8+4+c]-shadow_primitive.colors[j][c]*factor)<.001f);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* Consecutive packets must retain uploaded bytes and ordered pass results, blend order
   * and slot replacement until submission. A cancelled upload must never be
   * mistaken for valid retained data on the next scope. */
  SDL_Surface *batch_images[4] = {0};
  for (unsigned mode = 0; mode < 4; ++mode) {
    CHECK(SDL_SetRenderDrawColor(renderer, 17, 29, 41, 255) && SDL_RenderClear(renderer));
    effect.batching_enabled = mode != 0;
    /* Replacing an existing packet's output layout must invalidate its input
     * upload even when revision and brightness are unchanged. */
    effect.packing_enabled = mode >= 2;
    CHECK(ArGpuEffectSource_BeginBatch(&effect));
    CHECK(!ArGpuEffectSource_BeginBatch(&effect)); /* No accidental nesting. */
    const uint64_t submitted = effect.submissions;
    for (unsigned i = 0; i < 5; ++i) {
      ActionEffectSourcePrimitive quad = {.meta={kActionSourceQuad,1},
          .clip={-100,-100,100,100}, .points={{4+i,4},{24,4},{24,12},{4+i,12}},
          .colors={{.2f,.5f,.1f,.45f},{.2f,.5f,.1f,.45f},{.2f,.5f,.1f,.45f}},
          .extra={.2f,.5f,.1f,.45f}};
      ActionEffectSourceBatch draw_batch = {.primitives=&quad,.count=1,.capacity=1,
          .revision=1000+i, .packet_slot=i==4?0:i};
      CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,0,&draw_batch,&shadow_view,NULL,
          i%2?kArRenderBlendMode_Add:kArRenderBlendMode_Alpha,1.f-.1f*i));
    }
    CHECK(effect.submissions == submitted + (mode ? 0 : 5));
    CHECK(ArGpuEffectSource_EndBatch(&effect));
    CHECK(effect.submissions == submitted + (mode ? 1 : 5));
    CHECK(!effect.commands && !effect.batch_open);
    batch_images[mode] = SDL_RenderReadPixels(renderer,NULL);
    CHECK(batch_images[mode] != NULL);
    if (mode && batch_images[0] && batch_images[mode]) {
      CHECK(batch_images[0]->format == batch_images[mode]->format);
      for (int y=0;y<64;++y)
        CHECK(!memcmp((char*)batch_images[0]->pixels+y*batch_images[0]->pitch,
            (char*)batch_images[mode]->pixels+y*batch_images[mode]->pitch,64*4));
    }
    if (mode == 2) {
      CHECK(ArGpuEffectSource_BeginBatch(&effect));
      CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,0,&shadow_batch,&shadow_view,&scenery,
          kArRenderBlendMode_Add,1));
      const ActionEffectSourceBatch invalid = {.failed=true};
      CHECK(!ArGpuEffectSource_Draw(&effect,device,motion,0,0,&invalid,&shadow_view,NULL,
          kArRenderBlendMode_Add,1));
      CHECK(!ArGpuEffectSource_EndBatch(&effect));
      CHECK(!effect.commands && !effect.command_writes &&
          !effect.packets[0].revision && !effect.packets[0].input_revision);
    }
  }
  for (unsigned i=0;i<4;++i) SDL_DestroySurface(batch_images[i]);
  const uint64_t submitted = effect.submissions;
  CHECK(ArGpuEffectSource_BeginBatch(&effect));
  const ActionEffectSourceBatch empty = {0};
  CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,0,&empty,&shadow_view,NULL,kArRenderBlendMode_Add,1));
  CHECK(ArGpuEffectSource_EndBatch(&effect) && effect.submissions == submitted);
  /* The same world transform must cover OBJ priorities, high BG planes,
   * folded waterfalls, and static/dynamic anchors spanning raster bands. */
  for (unsigned scenario = 0; scenario < 15; ++scenario)
    for (unsigned phase = 0; phase < (scenario >= 11 ? 5u : 1u); ++phase) {
    DioramaProjection v = {.valid = true, .texture_width = 32, .texture_height = 16,
        .output_width = 64, .output_height = 64, .aspect_x = 2, .height_scale = 2,
        .matrix = {1,0,0,0, 0,1,0,0, .1f,.1f,1,.2f, 0,0,0,1}};
    const DioramaPlaneProjection plane = {.valid = true, .u1 = 1, .v1 = 1,
        .z_world = .1f, .rake = .1f, .bow = .05f, .world_y_offset = .03f};
    v.bg1_plane = v.bg2_plane = v.bg1_high_plane = v.bg2_high_plane = plane;
    for (unsigned i = 0; i < 4; ++i) {v.object_planes[i] = plane; v.object_planes[i].z_world += i*.1f;}
    ActionEffectSourcePrimitive p = {.meta = {kActionSourceQuad, scenario < 4 ?
        (scenario ? 5 + scenario : 0) : scenario < 6 ? scenario - 1 : 2},
        .origin = {5,2}, .clip = {-100,-100,100,100}, .points={{1,1},{3,1},{3,3},{1,3}},
        .colors={{1,1,1,1},{1,1,1,1},{1,1,1,1}},.extra={1,1,1,1}};
    if (scenario == 6) {
      v.bg2_plane.overflow_valid = true; v.bg2_plane.overflow_fold_t = 1;
      v.bg2_plane.overflow_height = 1; v.bg2_plane.overflow_overlap_t = .2f;
      v.bg2_plane.overflow_handoff_z = .5f; v.bg2_plane.overflow_front_z = .9f;
      v.bg2_plane.overflow_front_drop = .18f; p.origin[1] = 18;
    } else if (scenario >= 7 && scenario < 10) {
      /* These cases exercise the skybox fallback, selected in production only
       * when no finite BG2 plane is active. Do not leave the earlier plane
       * enabled while asking the independent CPU oracle to project a band. */
      v.bg2_plane.valid = false;
      v.bg2_skybox = (DioramaSkyboxProjection){.count=2,.active_band=scenario==7?1:-1,
          .bands={{0,0,32,8,0,.5f},{-8,8,40,16,.5f,1}}};
      p.origin[1] = scenario == 9 ? 2 : 9;
      if (scenario == 9) {p.origin[2] = 2; for (unsigned j=0;j<4;++j)p.points[j][1]+=7;}
    } else if (scenario == 10) {
      /* A finite UV edge that does not round-trip exactly through pixel
       * bounds must keep an attached primitive whose normalized point is on
       * the edge. This dropped Marahna's torch at the capture's left border. */
      v.texture_width=640;v.texture_x_origin=64;v.bg2_plane.u0=.1f;
      p.origin[0]=0;
      for(unsigned j=0;j<4;++j)p.points[j][0]-=1;
    }
    uint32_t motion_mask = 0;
    float dx = 0, dy = 0;
    if (scenario >= 11) {
      /* Atmospheric rays/dust use midpoint parallax, including both capture
       * offsets and GPU-only interpolated motion. Cover plane, skybox,
       * anchored raster bands and a world-projected skybox. */
      p.meta[1] = 5;
      v.bg2_plane.capture_offset = (ArRenderPointF){-.5f,.75f};
      v.between_backgrounds_capture_offset = (ArRenderPointF){1,-.25f};
      v.between_backgrounds_world_y_offset = -.02f;
      if (scenario >= 12) {
        v.bg2_skybox = (DioramaSkyboxProjection){.count=2,.active_band=-1,
            .motion_source=3,.bands={{0,0,32,8,0,.5f},{-8,8,40,16,.5f,1}}};
        if (scenario == 14) v.bg2_skybox.world_plane = v.bg2_plane;
        v.bg2_plane.valid = false;
        if (scenario == 13) p.origin[2] = 2;
      }
      int32_t *vectors = SDL_MapGPUTransferBuffer(gpu, upload, true);
      CHECK(vectors != NULL); if (!vectors) continue;
      memset(vectors,0,256);
      const int32_t front[] = {4,-8,-2,4,1}, back[] = {2,-4,-1,2,1};
      memcpy(vectors,front,sizeof(front)); memcpy(vectors+3*8,back,sizeof(back));
      SDL_UnmapGPUTransferBuffer(gpu,upload);
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
      SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
      const SDL_GPUTransferBufferLocation from = {.transfer_buffer=upload};
      const SDL_GPUBufferRegion to = {.buffer=motion,.size=256};
      SDL_UploadToGPUBuffer(copy,&from,&to,true); SDL_EndGPUCopyPass(copy);
      CHECK(SDL_SubmitGPUCommandBuffer(cmd));
      motion_mask = (1u<<0)|(1u<<3);
      dx = phases[phase] < .5f ? -1.5f + 3*phases[phase] : -1.5f*(1-phases[phase]);
      dy = -2*dx;
    }
    const ActionEffectSourceBatch b = {.primitives=&p,.count=1,.capacity=1};
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,motion_mask,phases[phase],&b,&v,NULL,kArRenderBlendMode_Add,1));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion from={.buffer=effect.vertices,.size=15*32};
    const SDL_GPUTransferBufferLocation to={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out!=NULL);
    if(!out)continue;
    const unsigned corners[]={0,1,2,0,2,3};
    for(unsigned i=0;i<6;++i) {
      const float x=p.origin[0]+p.points[corners[i]][0]+dx, y=p.origin[1]+p.points[corners[i]][1]+dy;
      ArRenderPointF expected;
      const bool ok=scenario>=11?Diorama_ProjectCapturedBetweenBackgroundsPoint(
          &v,x,y,p.origin[1]+dy,scenario==13,&expected):
          scenario<4?Diorama_ProjectCapturedPoint(&v,x,y,scenario,&expected,NULL,NULL):
          scenario==4?Diorama_ProjectCapturedBg1HighPoint(&v,x,y,&expected,NULL,NULL):
          scenario==5?Diorama_ProjectCapturedBg2HighPoint(&v,x,y,&expected,NULL,NULL):
          scenario==9?Diorama_ProjectSkyboxAnchorPoint(&v,p.origin[1],x,y,&expected):
          Diorama_ProjectCapturedBg2Point(&v,x,y,&expected,NULL,NULL);
      CHECK(ok);
      if (fabsf(out[i*8]-expected.x)>=.001f || fabsf(out[i*8+1]-expected.y)>=.001f)
        fprintf(stderr,"projection scenario=%u phase=%u corner=%u actual=(%g,%g) expected=(%g,%g)\n",
            scenario,phase,i,out[i*8],out[i*8+1],expected.x,expected.y);
      CHECK(fabsf(out[i*8]-expected.x)<.001f && fabsf(out[i*8+1]-expected.y)<.001f);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* Synthetic moon receiver: the finite platform shades y=8 at half depth,
   * while the near y=2 receiver remains lit. A glowing insect ignores platform
   * transport, but uses the same projected angular light distribution. */
  for(unsigned kind=0;kind<4;++kind)for(unsigned blocked=0;blocked<2;++blocked) {
    DioramaProjection v={.valid=true,.texture_width=32,.texture_height=16,
        .output_width=64,.output_height=64,.aspect_x=2,.height_scale=2,
        .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
        .bg1_plane={.valid=true,.u1=1,.v1=1},
        .bg2_skybox={.count=1,.active_band=-1,.bands={{0,0,32,16,0,1}}}};
    const ActionMoonlightOcclusion occlusion={.valid=true,.count=blocked,
        .rectangles={{0,3,32,5}}};
    static ActionEffectSourceLightJob light;
    light=(ActionEffectSourceLightJob){.occlusion=&occlusion,
        .data={.meta={3,kind,10},.light={16,0,2},.receiver={16,0,2},
            .clip={-100,-100,100,100},.transport={.5f,.5f,.5f},.surface={.5f,.5f},
            .rays={{0,10,1},{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,1,0}},
            .points={{0,8},{8,8},{0,2}}}};
    ActionEffectSourcePrimitive triangles[3];
    for(unsigned i=0;i<3;++i) {
      triangles[i]=(ActionEffectSourcePrimitive){.meta={kActionSourceLitTriangle,2},
          .origin={16,0},.clip={-100,-100,100,100},.extra={10},
          .colors={{1,1,1,1},{1,1,1,1},{1,1,1,1}}};
      for(unsigned j=0;j<3;++j) {
        triangles[i].points[j][0]=light.data.points[i][0];
        triangles[i].points[j][1]=light.data.points[i][1];
        triangles[i].points[j][2]=i+1;
      }
    }
    const ActionEffectSourceBatch b={.primitives=triangles,.count=3,.capacity=3,
        .lights=&light,.light_count=1,.light_capacity=1};
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&b,&v,NULL,kArRenderBlendMode_Add,1));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion from={.buffer=effect.vertices,.size=45*32};
    const SDL_GPUTransferBufferLocation to={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out!=NULL);
    if(!out)continue;
    for(unsigned i=0;i<3;++i) {
      float expected=kind==0||i!=1?1:.96059601f;
      if(blocked&&kind!=3&&i<2)expected=0;
      if(fabsf(out[i*15*8+7]-expected)>=.001f)
        fprintf(stderr,"moon kind=%u blocked=%u point=%u actual=%f expected=%f\n",kind,blocked,i,out[i*15*8+7],expected);
      CHECK(fabsf(out[i*15*8+7]-expected)<.001f);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* Empty capture bands need no triangle/light dispatch. Resident motion may
   * reveal a band whose CPU row interval is empty, so only raw empty spans
   * qualify. Weather still draws the union when the first band is empty. */
  for (unsigned scenario=0;scenario<10;++scenario) {
    DioramaProjection v={.valid=true,.texture_width=32,.texture_height=16,
        .output_width=64,.output_height=64,.aspect_x=2,.height_scale=2,
        .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
        .bg1_plane={.valid=true,.u1=1,.v1=1},
        .bg2_skybox={.count=2,.active_band=0,
            .bands={{0},{0,8,32,16,.5f,1}},
            .resident={.meta={2,32,16,1},.output={64,64},.vertical={0,16,0,1},
                .bands={{0,0,0,8},{0,1,8,16}}}}};
    ActionEffectSourcePrimitive triangles[2]={
      {.meta={kActionSourceTriangle,2},.origin={8,12,6},.clip={-100,-100,100,100},
       .points={{0,0},{4,0},{0,3}},.colors={{1,1,1,1},{1,1,1,1},{1,1,1,1}}},
      {.meta={kActionSourceTriangle,1},.origin={8,12},.clip={-100,-100,100,100},
       .points={{0,0},{4,0},{0,3}},.colors={{1,1,1,1},{1,1,1,1},{1,1,1,1}}}};
    ActionEffectSourceBatch b={.primitives=triangles,.count=1,.capacity=2};
    static ActionEffectSourceLightJob light;
    light=(ActionEffectSourceLightJob){.data={.meta={1,0},.light={8,12,2,6},
        .receiver={8,12,2,6},.clip={-100,-100,100,100}}};
    if (scenario==1) v.bg2_skybox.resident.meta[0]=0;
    if (scenario==2) {
      v.bg2_skybox.resident.bands[0][1]=1;
      triangles[0].origin[1]=2;
    }
    if (scenario==3) v.bg2_plane=v.bg1_plane;
    if (scenario==4) v.bg2_skybox.world_plane=v.bg1_plane;
    if (scenario==5) v.bg2_skybox.to_texture=true;
    if (scenario==6) b.count=2;
    if (scenario==7) {
      triangles[0].meta[0]=kActionSourceParticle;
      triangles[0].origin[2]=7;
      triangles[0].points[1][0]=triangles[0].points[1][1]=1;
      triangles[0].extra[0]=triangles[0].extra[1]=1;
    }
    if (scenario==8) b.weather_only=true;
    if (scenario==9) {
      triangles[0].meta[0]=kActionSourceLitTriangle;
      b.lights=&light;b.light_count=b.light_capacity=1;
    }
    CHECK(SDL_SetRenderDrawColor(renderer,0,0,0,255) && SDL_RenderClear(renderer));
    const uint64_t draws=effect.draws, submissions=effect.submissions;
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&b,&v,NULL,kArRenderBlendMode_Add,1));
    const bool empty=scenario==0||scenario==1||scenario==9;
    CHECK(effect.draws==draws+!empty && effect.submissions==submissions+!empty);
    if (scenario==8) {
      v.bg2_skybox.active_band=1;
      CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&b,&v,NULL,kArRenderBlendMode_Add,1));
      CHECK(effect.draws==draws+1 && effect.submissions==submissions+1);
    }
    if (empty || scenario==2 || scenario==8) {
      SDL_Surface *pixels=SDL_RenderReadPixels(renderer,NULL);
      SDL_Surface *rgba=pixels?SDL_ConvertSurface(pixels,SDL_PIXELFORMAT_RGBA32):NULL;
      CHECK(rgba!=NULL);
      unsigned lit=0;
      if (rgba) for (int y=0;y<rgba->h;++y) for (int x=0;x<rgba->w;++x)
        lit+=((const uint8_t*)rgba->pixels)[y*rgba->pitch+x*4]>0;
      CHECK(empty ? lit==0 : lit>0);
      SDL_DestroySurface(rgba);SDL_DestroySurface(pixels);
    }
  }
  TestResidentSkyWindow(device, gpu, renderer, &effect, motion, upload);
  CHECK(SDL_SetRenderTarget(renderer,previous_target));
  SDL_DestroyTexture(target); ArGpuEffectSource_Destroy(&effect);
  SDL_ReleaseGPUBuffer(gpu,motion); SDL_ReleaseGPUTransferBuffer(gpu,upload);
  SDL_ReleaseGPUTransferBuffer(gpu,download);
}

static void TestProductionPolicy(void) {
  unsigned rooms=0;
  for(unsigned group=1;group<=7;++group)for(unsigned room=1;room<=ActRaiser_ActionMapLast(group);++room) {
    const DioramaGpuPolicy p=DioramaGpuPolicy_Resolve(group,room,NULL,NULL,NULL,NULL,NULL);
    CHECK(p.capture==kDioramaGpuCapture_Owned && p.motion==kDioramaGpuMotion_Owned &&
        p.pack && p.unpack && p.resident);
    ++rooms;
  }
  CHECK(rooms==49);
  const DioramaGpuPolicy reference=DioramaGpuPolicy_Resolve(1,1,"reference","owned","owned","all","resident");
  CHECK(!reference.capture&&!reference.motion&&!reference.pack&&!reference.unpack&&!reference.resident);
  const DioramaGpuPolicy sim=DioramaGpuPolicy_Resolve(0,1,NULL,NULL,NULL,NULL,NULL);
  CHECK(!sim.capture&&!sim.motion&&!sim.pack&&!sim.resident);
  const DioramaGpuPolicy invalid=DioramaGpuPolicy_Resolve(1,99,NULL,NULL,NULL,NULL,NULL);
  CHECK(!invalid.capture&&!invalid.motion&&!invalid.resident);
  const DioramaGpuPolicy explicit_reference=DioramaGpuPolicy_Resolve(1,1,NULL,"0","0","0","reference");
  CHECK(!explicit_reference.capture&&!explicit_reference.motion&&!explicit_reference.pack&&!explicit_reference.resident);
  const DioramaGpuPolicy incomplete=DioramaGpuPolicy_Resolve(1,1,NULL,NULL,"compute","all","resident");
  CHECK(!incomplete.resident);
}

static uint32_t MapPixel(int x,int y,unsigned endpoint,bool periodic) {
  if(periodic)return (x+y)&1?0xff287810u:0xff020804u;
  if(x==12 && y==8)return endpoint?0xff2040f0u:0xfff04020u;
  return PatternPixel(x,y);
}

static void TestExactMapPlacement(ArRenderDevice *device,SDL_Renderer *renderer,
    SDL_Texture *current,bool resident) {
  const int bands[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
  const uint32_t mask=(1u<<bands[0])|(1u<<bands[1])|(1u<<bands[2]);
  const float phases[]={0,.49f,.5f,.75f,1};
  const int deltas[][2]={{2,-1},{-2,1},{0,2},{0,0},{2,0},{2,0},{2,0},{8,0},{2,0},{2,0}};
  ActionVoxelGeometry *voxel=ActionVoxel_Create();CHECK(voxel!=NULL);if(!voxel)return;
  voxel->ready=true;voxel->active_group=voxel->active_room=1;
  voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);
  static FrameSlot slot;
  static SrPpuBgPacket packet;
  const SDL_Rect region={kApron,0,kDisplayWidth,kHeight};
  SDL_Texture *saved=SDL_GetRenderTarget(renderer);
  for(unsigned test=0;test<SDL_arraysize(deltas);++test) {
    DioramaFrameGeneration_InvalidateHistory();
    memset(&slot,0,sizeof(slot));memset(&packet,0,sizeof(packet));
    packet.owned_sources=packet.words[2]=1;
    slot=(FrameSlot){.diorama_active=true,.interp_setting_enabled=true,
        .snes_width=kDisplayWidth,.snes_height=kHeight,.obj_apron=kApron,
        .capture_ticks=1,.bg_mode=1,.diorama_map_group=1,.diorama_map_number=1,
        .diorama_plane_request_mask=mask,.diorama_plane_content_mask=mask,
        .diorama_voxel=voxel,.background_packet=&packet};
    slot.action_bg_plan.valid=true;
    slot.action_bg_plan.layer[0]=(ActionBgLayerPlan){.valid=true,.role=kActionBgLayerRole_Playfield,
        .source=kActionBgSource_WorldMap,.default_edge=kActionBgEdge_LiveWorld,
        .world_width=1024,.world_height=768};
    if(test==4)slot.diorama_voxel=NULL;
    if(test==5)slot.action_bg_plan.layer[0].band_count=1;
    if(test==6)slot.diorama_plane_additive_mask=mask;
    if(test==8)slot.action_bg_plan.layer[0].role=kActionBgLayerRole_Backdrop;
    const bool expected=test<4 || test==9,periodic=test==9;
    ArRenderTexture raw[kDioramaPlane_Count]={0},resolved[kDioramaPlane_Count]={0};
    const uint8_t *pixels[kDioramaPlane_Count]={0};size_t pitches[kDioramaPlane_Count]={0};
    for(unsigned i=0;i<3;++i)raw[bands[i]]=ArSdlRenderBackend_BorrowTexture(current);
    uint32_t native[kDisplayWidth*kHeight];
    /* A third identical periodic capture exercises atlas swapping after the
     * first no-change pair, not merely initial endpoint seeding. */
    for(unsigned endpoint=0;endpoint<(periodic?3u:2u);++endpoint) {
      const int dx=endpoint*deltas[test][0],dy=endpoint*deltas[test][1];
      for(int y=0;y<kHeight;++y)for(int x=0;x<kDisplayWidth;++x)
        native[y*kDisplayWidth+x]=MapPixel(x+dx,y+dy,endpoint,periodic);
      DioramaFrameGeneration_FinishCapture();
      CHECK(SDL_UpdateTexture(current,&region,native,kDisplayWidth*sizeof(uint32_t)));
      slot.timestamp_ns=UINT64_C(1000000000)+test*UINT64_C(100000000)+endpoint*UINT64_C(16666667);
      slot.bg1_camera_x=dx;slot.bg1_camera_y=dy;
      DioramaFrameGeneration_Capture(device,&slot,raw,pixels,pitches,periodic&&endpoint?0:mask);
      if(!endpoint)continue;
      for(unsigned step=0;step<SDL_arraysize(phases);++step) {
        const float phase=phases[step];
        const uint32_t generated=DioramaFrameGeneration_Prepare(device,&slot,phase,raw,mask,resolved);
        CHECK(DioramaFrameGeneration_VoxelMapPlaneMask()==(expected?mask:0));
        if(!expected)continue;
        CHECK(generated==mask && DioramaFrameGeneration_GeneratedPlaneMask()==mask);
        CHECK(DioramaFrameGeneration_GpuPlaneMask()==0); /* Do not move effects twice. */
        CHECK(!resident || DioramaFrameGeneration_MetadataReadbackCount()==0);
        uint64_t hash=UINT64_C(14695981039346656037);
        for(unsigned i=0;i<3;++i) {
          const ArRenderPointF offset=DioramaFrameGeneration_PlaneOffset(bands[i]);
          hash=SourceFixtureHash(hash,&offset.x,sizeof(offset.x));
          hash=SourceFixtureHash(hash,&offset.y,sizeof(offset.y));
          CHECK(fabsf(offset.x-deltas[test][0]*(1-phase))<.00001f);
          CHECK(fabsf(offset.y-deltas[test][1]*(1-phase))<.00001f);
          CHECK(SDL_SetRenderTarget(renderer,ArSdlRenderBackend_UnwrapTexture(resolved[bands[i]])));
          SDL_Surface *image=SDL_RenderReadPixels(renderer,&region);
          SDL_Surface *argb=image?SDL_ConvertSurface(image,SDL_PIXELFORMAT_ARGB8888):NULL;
          CHECK(argb!=NULL);
          if(argb)for(int y=0;y<kHeight;++y)for(int x=0;x<kDisplayWidth;++x) {
            const int px=x+deltas[test][0],py=y+deltas[test][1];
            const bool previous=phase<.5f && px>=0 && px<kDisplayWidth && py>=0 && py<kHeight;
            const uint32_t want=MapPixel(x+dx,y+dy,previous?0:1,periodic);
            const uint32_t *row=(const uint32_t *)((const uint8_t *)argb->pixels+y*argb->pitch);
            CHECK(row[x]==want);
          }
          if(argb)for(int y=0;y<kHeight;++y)
            hash=SourceFixtureHash(hash,(const uint8_t *)argb->pixels+y*argb->pitch,kDisplayWidth*4);
          SDL_DestroySurface(argb);SDL_DestroySurface(image);
          CHECK(SDL_SetRenderTarget(renderer,saved));
        }
        printf("exact-map-case=%u endpoint=%u phase=%.2f rgba-offset-fnv=%016" PRIx64 "\n",
            test,endpoint,phase,hash);
      }
    }
  }
  DioramaFrameGeneration_InvalidateHistory();
  CHECK(DioramaFrameGeneration_VoxelMapPlaneMask()==0);
  for(unsigned i=0;i<3;++i)CHECK(DioramaFrameGeneration_PlaneOffset(bands[i]).x==0 &&
      DioramaFrameGeneration_PlaneOffset(bands[i]).y==0);
  ActionVoxel_Release(voxel);
  printf("exact Voxel map: signed/vertical continuous placement, all3 bands, endpoint bytes, border fallback, unchanged pixels and unsupported-source guards\n");
}

/* Hash both raw and generated outputs through the same opaque copy, together
 * with the masks and offsets consumed by effects. The unchanged encoder and
 * candidate run this identical fixture; no new search-vector API is needed. */
static void HashMapTransition(SDL_Renderer *renderer,SDL_Texture *readback,
    const ArRenderTexture textures[kDioramaPlane_Count],uint32_t generated,unsigned test) {
  const int planes[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far,
      SR_PPU_OVERLAY_BG2,SR_PPU_OVERLAY_OBJ};
  uint64_t hash=UINT64_C(14695981039346656037);
  const uint32_t masks[]={generated,DioramaFrameGeneration_GpuPlaneMask(),
      DioramaFrameGeneration_VoxelMapPlaneMask(),DioramaFrameGeneration_GeneratedPlaneMask()};
  hash=SourceFixtureHash(hash,masks,sizeof(masks));
  SDL_Texture *saved=SDL_GetRenderTarget(renderer);
  for(unsigned i=0;i<SDL_arraysize(planes);++i) {
    const int plane=planes[i];
    const ArRenderPointF offset=DioramaFrameGeneration_PlaneOffset(plane);
    hash=SourceFixtureHash(hash,&offset.x,sizeof(offset.x));
    hash=SourceFixtureHash(hash,&offset.y,sizeof(offset.y));
    SDL_Texture *texture=ArSdlRenderBackend_UnwrapTexture(textures[plane]);
    if(!texture)continue;
    DioramaPlaneCaptureRegion region;
    CHECK(DioramaPlaneCaptureRegion_Resolve(plane,kSurfaceWidth,kHeight,kApron,0,&region));
    const SDL_FRect source={(float)region.x,0,(float)region.width,kHeight};
    const SDL_FRect destination={0,0,(float)region.width,kHeight};
    SDL_BlendMode blend;
    CHECK(SDL_GetTextureBlendMode(texture,&blend));
    CHECK(SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE));
    CHECK(SDL_SetRenderTarget(renderer,readback));
    CHECK(SDL_RenderTexture(renderer,texture,&source,&destination));
    CHECK(SDL_SetTextureBlendMode(texture,blend));
    const SDL_Rect rect={0,0,region.width,kHeight};
    SDL_Surface *image=SDL_RenderReadPixels(renderer,&rect);
    SDL_Surface *rgba=image?SDL_ConvertSurface(image,SDL_PIXELFORMAT_RGBA32):NULL;
    CHECK(rgba!=NULL);
    if(rgba)for(int y=0;y<kHeight;++y)
      hash=SourceFixtureHash(hash,(const uint8_t *)rgba->pixels+y*rgba->pitch,(size_t)region.width*4);
    SDL_DestroySurface(rgba);SDL_DestroySurface(image);
  }
  CHECK(SDL_SetRenderTarget(renderer,saved));
  printf("exact-map-transition=%u rgba-mask-offset-fnv=%016" PRIx64 "\n",test,hash);
}

static void TestExactMapTransitions(ArRenderDevice *device,SDL_Renderer *renderer,
    SDL_Texture *current) {
  const int bands[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
  const uint32_t map_mask=(1u<<bands[0])|(1u<<bands[1])|(1u<<bands[2]);
  const uint32_t controls=(1u<<SR_PPU_OVERLAY_BG2)|(1u<<SR_PPU_OVERLAY_OBJ);
  const uint32_t mask=map_mask|controls;
  ActionVoxelGeometry *voxel=ActionVoxel_Create();CHECK(voxel);if(!voxel)return;
  voxel->ready=true;voxel->active_group=voxel->active_room=1;
  voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);
  static FrameSlot slot;static SrPpuBgPacket packet;
  memset(&slot,0,sizeof(slot));memset(&packet,0,sizeof(packet));
  packet.owned_sources=packet.words[2]=3;
  slot=(FrameSlot){.diorama_active=true,.interp_setting_enabled=true,
      .snes_width=kDisplayWidth,.snes_height=kHeight,.obj_apron=kApron,
      .capture_ticks=1,.bg_mode=1,.diorama_map_group=1,.diorama_map_number=1,
      .diorama_plane_request_mask=mask,.diorama_plane_content_mask=mask,
      .diorama_voxel=voxel,.background_packet=&packet};
  slot.action_bg_plan.valid=true;
  slot.action_bg_plan.layer[0]=(ActionBgLayerPlan){.valid=true,.role=kActionBgLayerRole_Playfield,
      .source=kActionBgSource_WorldMap,.default_edge=kActionBgEdge_LiveWorld,
      .world_width=1024,.world_height=768};
  ArRenderTexture raw[kDioramaPlane_Count]={0},resolved[kDioramaPlane_Count]={0};
  const uint8_t *pixels[kDioramaPlane_Count]={0};size_t pitches[kDioramaPlane_Count]={0};
  for(unsigned i=0;i<3;++i)raw[bands[i]]=ArSdlRenderBackend_BorrowTexture(current);
  raw[SR_PPU_OVERLAY_BG2]=raw[SR_PPU_OVERLAY_OBJ]=ArSdlRenderBackend_BorrowTexture(current);
  uint32_t native[kSurfaceWidth*kHeight];
  pixels[SR_PPU_OVERLAY_OBJ]=(const uint8_t *)native;
  pitches[SR_PPU_OVERLAY_OBJ]=kSurfaceWidth*4;
  const SDL_Rect region={0,0,kSurfaceWidth,kHeight};
  SDL_Texture *readback=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET,kSurfaceWidth,kHeight);
  CHECK(readback);if(!readback){ActionVoxel_Release(voxel);return;}
  DioramaFrameGeneration_InvalidateHistory();
  unsigned case_number=0;
  for(unsigned endpoint=0;endpoint<13;++endpoint) {
    /* Lifecycle mode/editor changes happen at capture boundaries. The first
     * capture seeds a changed key; its second compatible capture can pair. */
    slot.diorama_voxel=(endpoint==2||endpoint==3)?NULL:voxel;
    if(endpoint==6)++slot.diorama_layer_section;
    if(endpoint==8)DioramaFrameGeneration_InvalidateHistory();
    raw[bands[1]]=endpoint==10?ArRenderTexture_Invalid():ArSdlRenderBackend_BorrowTexture(current);
    for(int y=0;y<kHeight;++y)for(int x=0;x<kSurfaceWidth;++x)
      native[y*kSurfaceWidth+x]=MapPixel(x-kApron+(int)endpoint*2,y,endpoint&1,false);
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current,&region,native,kSurfaceWidth*4));
    slot.timestamp_ns=UINT64_C(3000000000)+endpoint*UINT64_C(16666667);
    slot.bg1_camera_x=slot.bg2_camera_x=(int)endpoint*2;
    DioramaFrameGeneration_Capture(device,&slot,raw,pixels,pitches,mask);
    if(endpoint==1) {
      /* The first presentation lacks a texture, before any metadata readback.
       * Repeat it, then restore all sources, hide part/all of the map, restore
       * again. Other global planes and actor fields remain live throughout. */
      for(unsigned presentation=0;presentation<6;++presentation) {
        ArRenderTexture available[kDioramaPlane_Count];memcpy(available,raw,sizeof(available));
        if(presentation<2)available[bands[1]]=ArRenderTexture_Invalid();
        const uint32_t current_mask=presentation==3?controls|(1u<<bands[0]):
            presentation==4?controls:mask;
        const float phase=presentation&1?.75f:.25f;
        const uint32_t generated=DioramaFrameGeneration_Prepare(device,&slot,phase,
            available,current_mask,resolved);
        const uint32_t expected_map=presentation<2||presentation==4?0:
            presentation==3?1u<<bands[0]:map_mask;
        CHECK(DioramaFrameGeneration_VoxelMapPlaneMask()==expected_map);
        if(presentation<2)CHECK(!(generated&(1u<<bands[1])));
        HashMapTransition(renderer,readback,resolved,generated,case_number++);
      }
    } else {
      const uint32_t generated=DioramaFrameGeneration_Prepare(device,&slot,.25f,raw,mask,resolved);
      const uint32_t expected_map=endpoint==5||endpoint==7||endpoint==9||endpoint==12?map_mask:0;
      CHECK(DioramaFrameGeneration_VoxelMapPlaneMask()==expected_map);
      HashMapTransition(renderer,readback,resolved,generated,case_number++);
    }
  }
  DioramaFrameGeneration_InvalidateHistory();
  CHECK(DioramaFrameGeneration_VoxelMapPlaneMask()==0 && DioramaFrameGeneration_GpuPlaneMask()==0);
  ActionVoxel_Release(voxel);SDL_DestroyTexture(readback);
  puts("exact Voxel map transitions: missing/restored textures, partial/hidden masks, ordinary mode, editor section, reset and reseeding with BG2/actor controls");
}

#include "diorama_trunk_guard_framegen.inc"

int main(void) {
  TestProductionPolicy();
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Texture *current = NULL;
  SDL_Texture *scene = NULL;
  ArRenderDevice render_device = {0};
  ArSdlRenderBackend render_backend = {0};
  const bool resident = getenv("AR_GPU_EFFECT_PROJECTION") != NULL;
  DioramaFrameGeneration_AllowSourceProjection(resident);
  const bool gpu_motion = getenv("AR_GPU_BG_MOTION") != NULL;
  const bool gpu_owned = gpu_motion && strcmp(getenv("AR_GPU_BG_MOTION"), "owned") == 0;
  const int test_plane = gpu_motion ? SR_PPU_OVERLAY_BG1 : kDioramaPlane_Backdrop;
  DioramaPlaneCaptureRegion region;
  CHECK(DioramaPlaneCaptureRegion_Resolve(
      kDioramaPlane_Backdrop, kSurfaceWidth, kHeight, kApron, 3, &region));
  CHECK(region.x == kApron && region.width == kDisplayWidth &&
        region.height == kHeight);
  CHECK(DioramaPlaneCaptureRegion_Resolve(
      SR_PPU_OVERLAY_OBJ, kSurfaceWidth, kHeight, kApron, 0, &region));
  CHECK(region.x == 0 && region.width == kSurfaceWidth &&
        region.height == kHeight);
  const int backgrounds[] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi,
      kDioramaPlane_Bg1Far, SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far};
  for (unsigned i = 0; i < sizeof(backgrounds) / sizeof(backgrounds[0]); i++) {
    CHECK(DioramaPlaneCaptureRegion_Resolve(
        backgrounds[i], kSurfaceWidth, kHeight, kApron, 0, &region));
    CHECK(region.x == kApron && region.width == kDisplayWidth);
    CHECK(DioramaPlaneCaptureRegion_Resolve(
        backgrounds[i], kSurfaceWidth, kHeight, kApron, i < 3 ? 1 : 2, &region));
    CHECK(region.x == 0 && region.width == kSurfaceWidth);
    CHECK(DioramaPlaneCaptureRegion_Resolve(
        backgrounds[i], kSurfaceWidth, kHeight, kApron, i < 3 ? 2 : 1, &region));
    CHECK(region.x == kApron && region.width == kDisplayWidth);
  }
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
  if (window && gpu_motion) {
    if (ArSdlRenderBackend_CreateForWindow(&render_device, window, NULL))
      renderer = ArSdlRenderBackend_Renderer(&render_device);
  } else renderer = window ? CreateTestRenderer(window) : NULL;
  if (!renderer && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  CHECK(renderer != NULL);
  if (!renderer) goto done;
  SDL_GPUDevice *gpu_device = SDL_GetGPURendererDevice(renderer);
  printf("diorama frame-generation renderer=%s gpu=%s\n",
         SDL_GetRendererName(renderer),
         gpu_device ? SDL_GetGPUDeviceDriver(gpu_device) : "fallback");
  if (!gpu_motion) CHECK(ArSdlRenderBackend_Bind(
      &render_device, &render_backend, renderer));

  if (resident && gpu_device) TestSourcePrimitives(&render_device, gpu_device, renderer);

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
  /* Endpoints are now copied from the compositor texture on the GPU, so the
   * source texture has to hold this frame's pixels BEFORE Capture runs --
   * exactly the order Diorama_Upload establishes in the real path. */
  ArRenderTexture sources[kDioramaPlane_Count] = {0};
  sources[test_plane] =
      ArSdlRenderBackend_BorrowTexture(current);
  const SDL_Rect source_rect = {kApron, 0, kDisplayWidth, kHeight};
  planes[test_plane] = (uint8_t *)previous_pixels;
  plane_pitches[test_plane] =
      kSurfaceWidth * sizeof(uint32_t);

  static FrameSlot slot;
  static SrPpuBgPacket packet;
  if (gpu_owned) {
    packet.owned_sources = packet.words[2] = 7;
    slot.background_packet = &packet;
    planes[test_plane] = NULL;
  }
  slot.diorama_active = true;
  slot.interp_setting_enabled = true;
  slot.snes_width = kDisplayWidth;
  slot.snes_height = kHeight;
  slot.obj_apron = kApron;
  slot.capture_ticks = 1;
  slot.bg_mode = 1;
  slot.diorama_plane_request_mask = 1u << test_plane;
  slot.diorama_plane_content_mask = slot.diorama_plane_request_mask;
  slot.timestamp_ns = 1000000;
  /* Startup prepares shaders before any capture initializes the live room
   * policy. The direct-output CTest also verifies this uses no output atlas. */
  if (gpu_motion)
    CHECK(DioramaFrameGeneration_PreparePipelines(&render_device, true));
  CHECK(SDL_UpdateTexture(
      current, &source_rect, &previous_pixels[kApron],
      kSurfaceWidth * sizeof(uint32_t)));
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches,
      1u << test_plane);

  planes[test_plane] = gpu_owned ? NULL : (uint8_t *)current_pixels;
  slot.timestamp_ns += 16666667;
  slot.bg1_camera_x = -2; /* Image moved right by two pixels. */
  DioramaFrameGeneration_FinishCapture();
  CHECK(SDL_UpdateTexture(
      current, &source_rect, &current_pixels[kApron],
      kSurfaceWidth * sizeof(uint32_t)));
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches,
      1u << test_plane);

  CHECK(SDL_SetRenderTarget(renderer, scene));
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, 32, 32, SDL_LOGICAL_PRESENTATION_STRETCH));
  const SDL_Rect viewport = {1, 2, 30, 28};
  const SDL_Rect clip = {3, 4, 20, 18};
  CHECK(SDL_SetRenderViewport(renderer, &viewport));
  CHECK(SDL_SetRenderClipRect(renderer, &clip));
  CHECK(SDL_SetRenderDrawColor(renderer, 11, 22, 33, 44));

  ArRenderTexture raw[kDioramaPlane_Count] = {0};
  ArRenderTexture resolved[kDioramaPlane_Count] = {0};
  raw[test_plane] =
      ArSdlRenderBackend_BorrowTexture(current);
  const uint32_t generated = DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved);
  CHECK(generated == (1u << test_plane));
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == generated);
  CHECK(DioramaFrameGeneration_GpuPlaneMask() == (gpu_motion ? (1u << test_plane) : 0));
  ArRenderPointF offset = DioramaFrameGeneration_PlaneOffset(test_plane);
  CHECK(DioramaFrameGeneration_SourceProjectionActive() == resident);
  CHECK(!resident || DioramaFrameGeneration_MetadataReadbackCount() == 0);
  CHECK(fabsf(offset.x+(resident ? 0 : 1)) < .001f && fabsf(offset.y) < .001f);
  CHECK(DioramaFrameGeneration_PlaneOffset(SR_PPU_OVERLAY_BG2).x == 0);
  CHECK(DioramaFrameGeneration_PlaneOffset(-1).x == 0);
  CHECK(!ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

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
  CHECK(SDL_SetRenderTarget(
      renderer, ArSdlRenderBackend_UnwrapTexture(
                    resolved[test_plane])));
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
  CHECK(SDL_SetRenderViewport(renderer, NULL));
  CHECK(SDL_SetRenderClipRect(renderer, NULL));
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, &source_rect);
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

  /* Both endpoint owners publish the same current-capture-to-image motion.
   * A multi-tick pair uses the same phase mapping as texture generation. */
  const float phases[] = {.25f,.75f};
  for (unsigned i = 0; i < 2; i++) {
    CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,phases[i],raw,
        1u << test_plane,resolved) == (1u << test_plane));
    offset = DioramaFrameGeneration_PlaneOffset(test_plane);
    CHECK(fabsf(offset.x+(resident ? 0 : 2*(1-phases[i]))) < .001f && fabsf(offset.y) < .001f);
  }
  slot.capture_ticks = 2;
  CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,.5f,raw,
      1u << test_plane,resolved) == (1u << test_plane));
  CHECK(fabsf(DioramaFrameGeneration_PlaneOffset(test_plane).x+(resident ? 0 : .5f)) < .001f);
  slot.capture_ticks = 1;

  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, kPresentationFrameGenerationPhaseNone, raw,
      1u << test_plane, resolved) == 0);
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == 0);
  CHECK(DioramaFrameGeneration_PlaneOffset(test_plane).x == 0);
  CHECK(ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  /* A synchronized but byte-identical raw endpoint is already authoritative.
   * It must not create a redundant private pair or generated plane. */
  slot.timestamp_ns += 16666667;
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches, 0);
  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  /* A room-key discontinuity seeds a new endpoint but never blends across the
   * transition. */
  slot.timestamp_ns += 16666667;
  slot.diorama_map_number++;
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches, 0);
  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  /* Opening a gate and leaving Voxel mode replace the source geometry even
   * inside the same room. Never blend across those topology changes. */
  ActionVoxelGeometry *castle_a=ActionVoxel_Create(), *castle_b=ActionVoxel_Create();
  CHECK(castle_a && castle_b);
  const ActionVoxelGeometry *castle_frames[]={castle_a,castle_a,castle_b,NULL,NULL};
  for (unsigned pass=0;pass<sizeof(castle_frames)/sizeof(*castle_frames);++pass) {
    const uint8_t *endpoint=(const uint8_t *)(pass&1 ? current_pixels : previous_pixels);
    planes[test_plane]=gpu_owned ? NULL : endpoint;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current,&source_rect,endpoint+kApron*sizeof(uint32_t),
        kSurfaceWidth*sizeof(uint32_t)));
    slot.timestamp_ns+=16666667;
    slot.diorama_voxel=castle_frames[pass];
    DioramaFrameGeneration_Capture(&render_device,&slot,sources,planes,
        plane_pitches,1u<<test_plane);
    const uint32_t expected=(pass==1 || pass==4) ? 1u<<test_plane : 0;
    CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,.5f,raw,
        1u<<test_plane,resolved)==expected);
  }
  ActionVoxel_Release(castle_a);
  ActionVoxel_Release(castle_b);

  /* Complete Voxel inputs and native winner-cropped inputs can have the same
   * additive membership. Never morph old actor-shaped holes into the complete
   * source when a readiness/policy transition changes that ownership mode. */
  slot.diorama_plane_additive_mask=(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Obj2);
  for(unsigned pass=0;pass<4;++pass) {
    const uint8_t *endpoint=(const uint8_t *)(pass&1?current_pixels:previous_pixels);
    planes[test_plane]=gpu_owned?NULL:endpoint;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current,&source_rect,endpoint+kApron*sizeof(uint32_t),
        kSurfaceWidth*sizeof(uint32_t)));
    slot.timestamp_ns+=16666667;
    slot.diorama_independent_subscreen=pass<2;
    DioramaFrameGeneration_Capture(&render_device,&slot,sources,planes,
        plane_pitches,1u<<test_plane);
    CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,.5f,raw,
        1u<<test_plane,resolved)==(pass&1?1u<<test_plane:0));
  }
  slot.diorama_plane_additive_mask=0;

  /* Shrinking and restoring the capture reallocates the compute atlas. Neither
   * its old field nor its old endpoint can survive the geometry change. */
  for (int pass = 0; pass < 3; ++pass) {
    slot.snes_height = pass == 0 ? kHeight - 4 : kHeight;
    const uint8_t *endpoint = (const uint8_t *)(pass == 2 ? current_pixels : previous_pixels);
    planes[test_plane] = gpu_owned ? NULL : endpoint;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &source_rect,
        endpoint + kApron * sizeof(uint32_t), kSurfaceWidth * sizeof(uint32_t)));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_Capture(&render_device, &slot, sources, planes,
        plane_pitches, 1u << test_plane);
    const uint32_t expected = pass == 2 ? 1u << test_plane : 0;
    CHECK(DioramaFrameGeneration_Prepare(&render_device, &slot, 0.5f, raw,
        1u << test_plane, resolved) == expected);
    CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == expected);
    if (gpu_motion) CHECK(DioramaFrameGeneration_GpuPlaneMask() == expected);
  }

  /* The presenter can replace backgrounds while the PPU still captures them.
   * Old GPU textures remain valid, but hidden layers must not get analyzed or
   * resurrect a motion pair. Turning them back on requires two fresh captures. */
  for (unsigned pass = 0; pass < 4; ++pass) {
    const uint8_t *endpoint=(const uint8_t *)(pass==3?current_pixels:previous_pixels);
    planes[test_plane]=gpu_owned?NULL:endpoint;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current,&source_rect,endpoint+kApron*sizeof(uint32_t),
        kSurfaceWidth*sizeof(uint32_t)));
    slot.timestamp_ns+=16666667;
    DioramaFrameGeneration_CaptureWithSkybox(&render_device,&slot,sources,planes,
        plane_pitches,pass<2?0:1u<<test_plane,1u<<test_plane,
        ArRenderTexture_Invalid(),false);
    CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,.5f,raw,
        1u<<test_plane,resolved)==(pass==3?1u<<test_plane:0));
  }

  /* A room transition must discard the old pair even when the new source
   * reports no changed planes. Reusing GPU allocations must not reuse motion. */
  DioramaFrameGeneration_InvalidateHistory();
  CHECK(DioramaFrameGeneration_GpuPlaneMask() == 0);
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == 0);
  CHECK(!DioramaFrameGeneration_SourceProjectionActive());
  CHECK(DioramaFrameGeneration_PlaneOffset(test_plane).x == 0);
  CHECK(DioramaFrameGeneration_PlaneOffset(test_plane).y == 0);
  slot.timestamp_ns += 16666667;
  DioramaFrameGeneration_Capture(&render_device, &slot, sources, planes,
      plane_pitches, 0);
  CHECK(DioramaFrameGeneration_Prepare(&render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(resolved[test_plane], raw[test_plane]));

  /* A changed but uniform pair has no reliable motion. GPU preparation may
   * already have copied its output before confidence reaches the CPU; that
   * private copy must not replace the current image or publish an offset. */
  DioramaFrameGeneration_Reset();
  for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
    for (unsigned i = 0; i < kSurfaceWidth * kHeight; ++i)
      current_pixels[i] = endpoint ? 0xff557799u : 0xff113355u;
    planes[test_plane] = gpu_owned ? NULL : (uint8_t *)current_pixels;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &source_rect, &current_pixels[kApron],
        kSurfaceWidth * sizeof(uint32_t)));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_Capture(&render_device, &slot, sources, planes,
        plane_pitches, 1u << test_plane);
  }
  CHECK(DioramaFrameGeneration_Prepare(&render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(resolved[test_plane], raw[test_plane]));
  CHECK(DioramaFrameGeneration_GpuPlaneMask() == 0);
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == 0);
  offset = DioramaFrameGeneration_PlaneOffset(test_plane);
  CHECK(offset.x == 0 && offset.y == 0);

  /* The clamped skybox is independent of the gameplay planes and has no
   * apron. A compact source pitch must remain valid through interpolation. */
  CHECK(SDL_SetRenderTarget(renderer, scene));
  DioramaFrameGeneration_Reset();
  uint32_t sky_pixels[kDisplayWidth * kHeight];
  const SDL_Rect sky_rect = {0, 0, kDisplayWidth, kHeight};
  slot.diorama_plane_request_mask = slot.diorama_plane_content_mask = 0;
  slot.diorama_skybox_surface = (SrPpuSurfaceView){
    .data = gpu_owned ? NULL : (const uint8_t *)sky_pixels,
    .width_pixels = kDisplayWidth, .height_pixels = kHeight,
    .pitch_bytes = kDisplayWidth * sizeof(uint32_t),
  };
  const ArRenderTexture sky_raw = ArSdlRenderBackend_BorrowTexture(current);
  // Drain multiple source frames without presenting. The queued GPU result
  // must belong to the newest pair (+4), never an earlier discarded fence.
  const int sky_shifts[] = {0, 2, 1, 5};
  for (int endpoint = 0; endpoint < 4; ++endpoint) {
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kDisplayWidth; ++x)
        sky_pixels[y * kDisplayWidth + x] = PatternPixel(x - sky_shifts[endpoint], y);
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &sky_rect, sky_pixels, kDisplayWidth * 4));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_CaptureWithSkybox(
        &render_device, &slot, sources, planes, plane_pitches, 0, 0, sky_raw, true);
  }
  ArRenderTexture sky_resolved;
  CHECK(DioramaFrameGeneration_PrepareWithSkybox(
      &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved) ==
      (1u << kDioramaFrameGenerationSkybox));
  CHECK(!ArRenderTexture_Equals(sky_resolved, sky_raw));
  const ArRenderPointF sky_offset = DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox);
  CHECK(fabsf(sky_offset.x+(resident ? 0 : 2)) < .001f && fabsf(sky_offset.y) < .001f);
  CHECK(SDL_GetRenderTarget(renderer) == scene);
  /* An old caller must not accidentally address the private extra slot. */
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw, UINT32_MAX, resolved) == 0);
  /* A periodic source animates in world space; falling water is not camera
   * motion. Switching from a finite source must discard its private pair. */
  slot.diorama_skybox_periodic = true;
  for (int endpoint = 0; endpoint < 2; ++endpoint) {
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kDisplayWidth; ++x)
        sky_pixels[y * kDisplayWidth + x] = PatternPixel(x, y - endpoint * 2);
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &sky_rect, sky_pixels, kDisplayWidth * 4));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_CaptureWithSkybox(
        &render_device, &slot, sources, planes, plane_pitches, 0, 0, sky_raw, true);
    CHECK(DioramaFrameGeneration_PrepareWithSkybox(
        &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved) == 0);
    CHECK(ArRenderTexture_Equals(sky_resolved, sky_raw));
    CHECK(DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox).x == 0);
    CHECK(DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox).y == 0);
  }
  DioramaFrameGeneration_Reset();
  CHECK(DioramaFrameGeneration_PrepareWithSkybox(
      &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved) == 0);
  CHECK(ArRenderTexture_Equals(sky_resolved, sky_raw));

  if (resident) {
    /* Room qualification can change without destroying the renderer. A
     * reference frame must download its metadata; returning to resident mode
     * must stop downloading without inheriting a stale pending result. */
    slot.diorama_skybox_periodic = false;
    unsigned downloads = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
      DioramaFrameGeneration_AllowSourceProjection(mode != 0);
      for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
        slot.timestamp_ns += 16666667;
        DioramaFrameGeneration_CaptureWithSkybox(
            &render_device, &slot, sources, planes, plane_pitches, 0, 0, sky_raw, true);
        DioramaFrameGeneration_FinishCapture();
        CHECK(DioramaFrameGeneration_SourceProjectionActive() == (mode != 0));
        DioramaFrameGeneration_PrepareWithSkybox(
            &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved);
      }
      if (!mode) {
        downloads = DioramaFrameGeneration_MetadataReadbackCount();
        CHECK(downloads > 0);
      } else {
        CHECK(DioramaFrameGeneration_MetadataReadbackCount() == downloads);
      }
    }
    /* Projection also runs with interpolation disabled, without creating a
     * motion pair. A failed batch must recover and latch until reset. */
    DioramaFrameGeneration_Reset();
    DioramaFrameGeneration_AllowSourceProjection(true);
    slot.interp_setting_enabled=false;
    for(unsigned attempt=0;attempt<3;++attempt) {
      slot.timestamp_ns+=16666667;
      DioramaFrameGeneration_CaptureWithSkybox(
          &render_device,&slot,sources,planes,plane_pitches,0,0,sky_raw,true);
      CHECK(DioramaFrameGeneration_SourceProjectionActive()==(attempt!=1));
      CHECK(DioramaFrameGeneration_SourceProjectionFallback()==(attempt==1));
      CHECK(DioramaFrameGeneration_PrepareWithSkybox(
          &render_device,&slot,.5f,raw,0,resolved,sky_raw,&sky_resolved)==0);
      CHECK(DioramaFrameGeneration_MetadataReadbackCount()==0);
      if(attempt==0) {
        const ActionEffectSourceBatch failed={.failed=true};
        CHECK(!DioramaFrameGeneration_DrawSource(&render_device,&failed,NULL,NULL,kArRenderBlendMode_Add,1));
        CHECK(DioramaFrameGeneration_SourceProjectionFailed());
        DioramaFrameGeneration_RecoverSourceProjection(&render_device);
        CHECK(!DioramaFrameGeneration_SourceProjectionActive());
        DioramaFrameGeneration_InvalidateHistory();
        CHECK(DioramaFrameGeneration_SourceProjectionFallback());
        DioramaFrameGeneration_AllowSourceProjection(false);
        CHECK(!DioramaFrameGeneration_SourceProjectionFallback());
        DioramaFrameGeneration_AllowSourceProjection(true);
      } else if(attempt==1) DioramaFrameGeneration_Reset();
    }
    slot.interp_setting_enabled=true;
  }

  if(gpu_motion && gpu_owned) {
    TestExactMapPlacement(&render_device,renderer,current,resident);
    TestExactMapTransitions(&render_device,renderer,current);
  }
  TestTrunkGuardPlacement(&render_device,renderer,current);
  TestVoxelGuardPlacement(&render_device,renderer,current);

  /* Teardown may happen immediately after dispatch, without Prepare ever
   * joining it. Repeated lifetimes must retire the borrow before destruction. */
  slot.diorama_skybox_periodic = false;
  for (unsigned reset = 0; reset < 3; ++reset) {
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_CaptureWithSkybox(
        &render_device, &slot, sources, planes, plane_pitches, 0, 0, sky_raw, true);
    DioramaFrameGeneration_InvalidateHistory();
    CHECK(DioramaFrameGeneration_PrepareWithSkybox(
        &render_device, &slot, .5f, raw, 0, resolved, sky_raw, &sky_resolved) == 0);
    DioramaFrameGeneration_Reset();
  }

done:
  DioramaFrameGeneration_Shutdown();
  SDL_DestroyTexture(scene);
  SDL_DestroyTexture(current);
  ArSdlRenderBackend_Destroy(&render_device);
  if (!gpu_motion) SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("diorama frame-generation test: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
