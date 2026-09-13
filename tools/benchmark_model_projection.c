/* Isolated architectural experiment, NOT an in-game FPS benchmark.
 * Both paths draw identical production Low models, precompiled onto a sphere.
 * CPU: project with the existing math + ordinary depth batch (held views cache
 * projection). GPU: retain those source vertices and submit a camera matrix.
 * Culling/LOD selection, scene compilation, game execution and weather are not
 * timed here. Verify performs readback separately; timing never reads pixels.
 */
#include <SDL3/SDL.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/parallel_work.h"
#include "platform/sdl/render_sdl_internal.h"
#include "scene3d_math.h"
#include "present_world_nav_geometry.h"
#include "sim/sim3d_depth_pass.h"
#include "sim3d_depth_reference.h"
#include "sim/sim_background_voxel_biome.h"
#include "sim/sim_background_voxel_model_cache.h"
#include "sim/sim_background_voxel_palette.h"
#include "sim/sim_background_voxel_proportions.h"
#include "sim/sim_world_navigation_globe.h"

enum { kObjects = 512, kMaxVertices = 256 * 1024, kWarmup = 120 };
/* Immutable during a worker job; verification changes viewport between jobs. */
static int kWidth = 1280, kHeight = 800;
typedef struct Work {
  const Sim3DDepthModelVertex *source;
  Sim3DDepthVertex *output;
  Scene3DClipPoint *clip;
  float matrix[16];
  bool *valid;
} Work;
typedef struct Scene {
  Sim3DDepthModelVertex *vertices;
  Sim3DDepthVertex *projected;
  Scene3DClipPoint *clip;
  bool *valid;
  size_t count;
} Scene;
static uint64_t upload_bytes, draw_calls;
void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  if (vertices && indices) ++draw_calls;
}
void Sim3DPerformance_AddGeometryUpload(uint64_t bytes) { upload_bytes += bytes; }
void Sim3DPerformance_AddGeometryCopy(uint64_t bytes, uint64_t calls) { (void)bytes; (void)calls; }
void Sim3DPerformance_AddAtlasCopy(uint64_t bytes) { (void)bytes; }

static bool BuildScene(Scene *scene) {
  scene->vertices = malloc(kMaxVertices * sizeof(*scene->vertices));
  scene->projected = malloc(kMaxVertices * sizeof(*scene->projected));
  scene->clip = malloc(kMaxVertices * sizeof(*scene->clip));
  scene->valid = malloc(kMaxVertices * sizeof(*scene->valid));
  if (!scene->vertices || !scene->projected || !scene->clip || !scene->valid) return false;
  const SimBackgroundVoxelKind kinds[] = {
    kSimBackgroundVoxel_House, kSimBackgroundVoxel_Tree,
    kSimBackgroundVoxel_House, kSimBackgroundVoxel_BroadTree,
    kSimBackgroundVoxel_Palm, kSimBackgroundVoxel_Cathedral,
    kSimBackgroundVoxel_Factory, kSimBackgroundVoxel_Shrub,
  };
  for (int i = 0; i < kObjects; ++i) {
    const SimBackgroundVoxelKind kind = kinds[i % 8];
    const uint8_t footprint = kind == kSimBackgroundVoxel_Cathedral ||
        kind == kSimBackgroundVoxel_Factory ? 2 : 1;
    const SimBackgroundVoxelObject object = {
      .kind = kind, .town = 1 + i % 6, .development_level = 2,
      .cell_x = i % 32, .cell_y = i / 32,
      .source_cells_w = footprint, .source_cells_h = footprint,
      .footprint_cells_w = footprint, .footprint_cells_d = footprint,
      .flags = kSimBackgroundVoxel_IsolatedTree,
    };
    const SimBackgroundVoxelBiome biome = SimBackgroundVoxelBiome_ForTown(object.town);
    const SimBackgroundVoxelModelShadingKey light = {
      .light_azimuth_deg = 315, .light_elevation_deg = 45,
      .shading = kSimBackgroundVoxelShading_AmbientOcclusion, .biome = biome,
    };
    const SimBackgroundVoxelModelShading *shading = NULL;
    const SimBackgroundVoxelModelView *model = SimBackgroundVoxelModelCache_Get(
        &object, kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Architectural,
        &light, &shading);
    if (!model || !shading || !model->face_count || model->overflow ||
        model->face_count * 4u > kMaxVertices - scene->count) return false;
    SimBackgroundVoxelPalette palette;
    SimBackgroundVoxelPalette_Build(&object, biome, &palette);
    const SimBackgroundVoxelProportions *proportions = SimBackgroundVoxelProportions_Get(kind);
    const float centre = footprint * kSimTownCellPixels * .5f;
    for (int f = 0; f < model->face_count; ++f) {
      const uint32_t color = SimBackgroundVoxelPalette_Base(&palette,
          (SimBackgroundVoxelMaterial)shading->material[f]);
      for (int p = 0; p < 4; ++p) {
        const SimBackgroundVoxelModelPoint point = model->faces[f].points[p];
        const float x = centre + (point.x - centre) * proportions->footprint_scale;
        const float y = centre + (point.y - centre) * proportions->footprint_scale;
        float normal[3], metric;
        if (!SimWorldNavigationGlobe_SampleAtRadius(96,
            kSimWorldMapTiles * .5f + (i % 32 - 16) * 2 + x / kSimTownCellPixels,
            kSimWorldMapTiles * .5f + (i / 32 - 8) * 2 + y / kSimTownCellPixels,
            normal, &metric)) return false;
        const float radius = 1.92f + point.z * proportions->height_scale * metric * .02f / kSimTownCellPixels;
        const float shade = .74f + .18f * shading->brightness[f][p] / 255.0f;
        scene->vertices[scene->count++] = (Sim3DDepthModelVertex){
          .position = {radius * normal[0], radius * normal[1], radius * normal[2] - 1.92f},
          .color = {((color >> 16) & 255) / 255.0f * shade,
                    ((color >> 8) & 255) / 255.0f * shade,
                    (color & 255) / 255.0f * shade, 1},
        };
      }
    }
  }
  return true;
}

static void ProjectRange(void *context, size_t first, size_t end) {
  Work *work = context;
  WorldNavigationProjection projection = {.clip_frustum = true};
  if (work->clip) memcpy(projection.matrix, work->matrix, sizeof(projection.matrix));
  for (size_t i = first; i < end; ++i) {
    const Sim3DDepthModelVertex *v = &work->source[i];
    Scene3DPoint screen = {0};
    float depth = 0;
    work->valid[i] = work->clip
        ? WorldNavigationProjectClippedPoint(&projection, (ArRenderRectI){0,0,kWidth,kHeight},
            v->position, &screen, &depth, &work->clip[i])
        : Scene3D_ProjectWorldPointWithDepth(work->matrix,
            v->position[0], v->position[1], v->position[2], kWidth, kHeight, &screen, &depth);
    work->output[i] = (Sim3DDepthVertex){screen.x, screen.y, depth, v->color, {-1,-1}};
  }
}

static void Camera(unsigned frame, bool moving, bool clipping, float matrix[16]) {
  const float time = moving ? frame * .007f : 0;
  const Scene3DCamera camera = {
    .tilt_x = -.35f + .2f * sinf(time), .tilt_y = .35f * sinf(time * .7f),
    .distance = 4.5f, .fov_y = .48f,
  };
  Scene3D_BuildViewProjection(&camera, kWidth, kHeight, matrix);
  if (clipping) {
    matrix[12] += 3.5f * sinf(time * .7f);
    matrix[13] += 1.5f * cosf(time * .3f);
  }
}

static SDL_Texture *Draw(ArRenderDevice *device, Scene *scene, Sim3DDepthMesh **mesh,
    HostParallelWork *workers, bool gpu, bool project, bool clipping,
    const float matrix[16], double *prepare_ms) {
  if (!Sim3DDepthPass_Begin(device, kWidth, kHeight, kArRenderFilter_Nearest)) return NULL;
  const uint64_t start = SDL_GetTicksNS();
  if (gpu) {
    if (!*mesh) *mesh = clipping ? Sim3DDepthPass_CreateHardwareClippedModelMesh()
                               : Sim3DDepthPass_CreateModelMesh();
    if (!*mesh || (!Sim3DDepthPass_MeshReady(*mesh) &&
        !Sim3DDepthPass_UpdateModelMesh(*mesh, scene->vertices, scene->count / 4)) ||
        !Sim3DDepthPass_AppendModelMesh(*mesh, matrix)) return NULL;
  } else {
    if (project) {
      Work work = {.source = scene->vertices, .output = scene->projected, .valid = scene->valid,
        .clip = clipping ? scene->clip : NULL};
      memcpy(work.matrix, matrix, sizeof(work.matrix));
      HostParallelWork_Run(workers, scene->count, 512, ProjectRange, &work);
      /* Input bounds are verified once outside timing. The projection call
       * still performs its normal finite/behind-eye checks in each worker. */
    }
    if (!WorldNavigationAppendProjectedQuads(kSim3DDepthPass_Solid, scene->projected,
        clipping ? scene->clip : NULL, scene->count / 4, (ArRenderRectI){0,0,kWidth,kHeight})) return NULL;
  }
  *prepare_ms = (SDL_GetTicksNS() - start) / 1e6;
  return ArSdlRenderBackend_UnwrapTexture(Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid()));
}

static SDL_Surface *Read(SDL_Renderer *renderer, SDL_Texture *texture) {
  if (!texture || !SDL_SetRenderTarget(renderer, texture)) return NULL;
  SDL_Surface *read = SDL_RenderReadPixels(renderer, NULL);
  SDL_Surface *rgba = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32) : NULL;
  SDL_DestroySurface(read);
  if (!SDL_SetRenderTarget(renderer, NULL)) { SDL_DestroySurface(rgba); return NULL; }
  return rgba;
}

static int Verify(ArRenderDevice *device, SDL_Renderer *renderer, Scene *scene,
    Sim3DDepthMesh **mesh, HostParallelWork *workers, bool clipping, const char *output) {
  unsigned max_delta = 0;
  uint64_t different = 0, coverage = 0, pixels = 0, painted = 0;
  uint64_t partial_quads = 0, outside_quads = 0, visible_different = 0;
  uint64_t delta_pixels[4] = {0}, worst_different = 0;
  unsigned interior_max_delta = 0;
  const int sizes[][2] = {{1280,800}, {800,600}, {1792,1344}, {1279,799}, {853,641}, {1920,1080}};
  enum { kViews = 32 };
  const unsigned cases = kViews * sizeof(sizes) / sizeof(sizes[0]);
  for (unsigned frame = 0; frame < cases; ++frame) {
    const uint64_t before_different = different, before_coverage = coverage;
    kWidth = sizes[frame / kViews][0]; kHeight = sizes[frame / kViews][1];
    float matrix[16]; Camera((frame % kViews) * 47, true, clipping, matrix);
    double unused;
    SDL_Surface *a = Read(renderer, Draw(device, scene, mesh, workers, false, true, clipping, matrix, &unused));
    SDL_Surface *b = Read(renderer, Draw(device, scene, mesh, workers, true, false, clipping, matrix, &unused));
    if (!a || !b) { SDL_DestroySurface(a); SDL_DestroySurface(b); return 1; }
    for (size_t i = 0; i < scene->count; ++i) if (!scene->valid[i]) {
      SDL_DestroySurface(a); SDL_DestroySurface(b); return 1;
    }
    if (clipping) for (size_t i = 0; i < scene->count; i += 4) {
      uint8_t all = 63, any = 0;
      for (int p = 0; p < 4; ++p) {
        const uint8_t outside = WorldNavigationClipOutside(scene->clip[i + p]);
        all &= outside; any |= outside;
      }
      partial_quads += any && !all; outside_quads += all != 0;
    }
    for (int y = 0; y < kHeight; ++y) {
      const uint8_t *pa = (const uint8_t *)a->pixels + y * a->pitch;
      const uint8_t *pb = (const uint8_t *)b->pixels + y * b->pitch;
      for (int x = 0; x < kWidth; ++x) {
        bool changed = false;
        unsigned pixel_delta = 0;
        for (int c = 0; c < 4; ++c) {
          unsigned delta = (unsigned)abs(pa[x * 4 + c] - pb[x * 4 + c]);
          if (delta > max_delta) max_delta = delta;
          if (delta > pixel_delta) pixel_delta = delta;
          if (pa[x * 4 + 3] && pb[x * 4 + 3] && delta > interior_max_delta)
            interior_max_delta = delta;
          changed |= delta != 0;
        }
        different += changed; ++pixels;
        if (pixel_delta) ++delta_pixels[pixel_delta <= 2 ? pixel_delta - 1 : pixel_delta <= 8 ? 2 : 3];
        visible_different += changed && pa[x * 4 + 3] && pb[x * 4 + 3];
        painted += pa[x * 4 + 3] != 0;
        coverage += (pa[x * 4 + 3] != 0) != (pb[x * 4 + 3] != 0);
      }
    }
    const uint64_t frame_different = different - before_different;
    if (frame_different) fprintf(stderr, "comparison case=%u size=%dx%d different=%" PRIu64
        " coverage=%" PRIu64 "\n", frame, kWidth, kHeight, frame_different, coverage - before_coverage);
    if (output && frame_different > worst_different) {
      char path[2048];
      int n = snprintf(path, sizeof(path), "%s/worst-cpu.bmp", output);
      if (n < 0 || (size_t)n >= sizeof(path) || !SDL_SaveBMP(a, path)) {
        SDL_DestroySurface(a); SDL_DestroySurface(b); return 1;
      }
      n = snprintf(path, sizeof(path), "%s/worst-gpu.bmp", output);
      if (n < 0 || (size_t)n >= sizeof(path) || !SDL_SaveBMP(b, path)) {
        SDL_DestroySurface(a); SDL_DestroySurface(b); return 1;
      }
      worst_different = frame_different;
    }
    if (output && frame == 2) {
      char path[2048];
      int n = snprintf(path, sizeof(path), "%s/model-cpu.bmp", output);
      if (n < 0 || (size_t)n >= sizeof(path) || !SDL_SaveBMP(a, path)) {
        SDL_DestroySurface(a); SDL_DestroySurface(b); return 1;
      }
      n = snprintf(path, sizeof(path), "%s/model-gpu.bmp", output);
      if (n < 0 || (size_t)n >= sizeof(path) || !SDL_SaveBMP(b, path)) {
        SDL_DestroySurface(a); SDL_DestroySurface(b); return 1;
      }
    }
    SDL_DestroySurface(a); SDL_DestroySurface(b);
  }
  printf("{\"mode\":\"%s\",\"cases\":%u,\"vertices\":%zu,\"pixels\":%" PRIu64
      ",\"painted\":%" PRIu64 ",\"different\":%" PRIu64 ",\"coverage_differences\":%" PRIu64
      ",\"max_channel_delta\":%u,\"visible_differences\":%" PRIu64 ",\"visible_max_delta\":%u"
      ",\"delta_1_pixels\":%" PRIu64 ",\"delta_2_pixels\":%" PRIu64
      ",\"delta_3_to_8_pixels\":%" PRIu64 ",\"delta_over_8_pixels\":%" PRIu64
      ",\"partial_quads\":%" PRIu64 ",\"outside_quads\":%" PRIu64 "}\n",
      clipping ? "verify-clipped" : "verify", cases, scene->count, pixels, painted, different,
      coverage, max_delta, visible_different, interior_max_delta, delta_pixels[0], delta_pixels[1],
      delta_pixels[2], delta_pixels[3], partial_quads, outside_quads);
  /* Deliberately strict: a prototype is not promoted by silently accepting
   * a new tolerance. Any difference remains an investigation/result. */
  return !painted || different ? 1 : 0;
}

static int CompareDouble(const void *a, const void *b) {
  const double av = *(const double *)a, bv = *(const double *)b;
  return (av > bv) - (av < bv);
}

static bool ParseInt(const char *text, int low, int high, int *out) {
  char *end; errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno || end == text || *end || value < low || value > high) return false;
  *out = (int)value; return true;
}

int main(int argc, char **argv) {
  int helpers, frames;
  const bool clipping = argc > 1 && (!strcmp(argv[1], "cpu-clipped") ||
      !strcmp(argv[1], "gpu-clipped") || !strcmp(argv[1], "verify-clipped"));
  const bool verify = argc > 1 && (!strcmp(argv[1], "verify") || !strcmp(argv[1], "verify-clipped"));
  if (argc < 5 || argc > 6 ||
      (strcmp(argv[1], "cpu") && strcmp(argv[1], "gpu") && !verify && !clipping) ||
      (strcmp(argv[2], "held") && strcmp(argv[2], "moving")) ||
      !ParseInt(argv[3], 0, 3, &helpers) || !ParseInt(argv[4], 16, 10000, &frames) ||
      (clipping && strcmp(argv[2], "moving")) ||
      (argc == 6 && !verify)) {
    fprintf(stderr, "Usage: %s cpu|gpu|verify|cpu-clipped|gpu-clipped|verify-clipped held|moving helpers(0..3) frames(16..10000) [verify-output-directory]\n", argv[0]);
    return 2;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_Window *window = SDL_CreateWindow("Retained model prototype", kWidth, kHeight, SDL_WINDOW_HIDDEN);
  SDL_Renderer *renderer = NULL;
  ArSdlRenderBackend backend = {0}; ArRenderDevice device = {0};
  SDL_PropertiesID props = SDL_CreateProperties();
  if (props && window) {
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
    renderer = SDL_CreateRendererWithProperties(props);
  }
  SDL_DestroyProperties(props);
  Scene scene = {0}; Sim3DDepthMesh *mesh = NULL;
  HostParallelWork *workers = NULL;
  double *times = NULL;
  int result = 1;
  if (!renderer || !ArSdlRenderBackend_Bind(&device, &backend, renderer)) { result = 77; goto done; }
  SDL_GPUDevice *gpu_device = SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
      SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  fprintf(stderr, "model prototype: driver=%s helpers-requested=%d\n", SDL_GetGPUDeviceDriver(gpu_device), helpers);
  if (!SDL_SetRenderVSync(renderer, 0)) fprintf(stderr, "VSync override unavailable: %s\n", SDL_GetError());
  if (!BuildScene(&scene)) goto done;
  workers = HostParallelWork_Create((unsigned)helpers);
  if (helpers && !workers) { fprintf(stderr, "requested workers unavailable\n"); goto done; }
  if (verify) {
    result = Verify(&device, renderer, &scene, &mesh, workers, clipping, argc == 6 ? argv[5] : NULL);
    goto done;
  }
  const bool gpu = !strcmp(argv[1], "gpu") || !strcmp(argv[1], "gpu-clipped");
  const bool moving = !strcmp(argv[2], "moving");
  times = malloc((size_t)frames * sizeof(*times));
  if (!times) goto done;
  double prepare_total = 0, scene_total = 0, frame_total = 0;
  uint64_t bytes = 0, draws = 0;
  /* Check every measured camera against the same conservative GPU bounds,
   * outside timing, including when benchmarking CPU-only. */
  if (!Sim3DDepthPass_Begin(&device, kWidth, kHeight, kArRenderFilter_Nearest) ||
      !(mesh = clipping ? Sim3DDepthPass_CreateHardwareClippedModelMesh() : Sim3DDepthPass_CreateModelMesh()) ||
      !Sim3DDepthPass_UpdateModelMesh(mesh, scene.vertices, scene.count / 4)) goto done;
  for (int f = 0; f < frames + kWarmup; ++f) {
    float matrix[16]; Camera((unsigned)f, moving, clipping, matrix);
    if (!Sim3DDepthPass_Begin(&device, kWidth, kHeight, kArRenderFilter_Nearest) ||
        !Sim3DDepthPass_AppendModelMesh(mesh, matrix)) goto done;
  }
  for (int f = 0; f < frames + kWarmup; ++f) {
    SDL_PumpEvents();
    float matrix[16]; Camera((unsigned)f, moving, clipping, matrix);
    double prepare_ms;
    const uint64_t start = SDL_GetTicksNS(), before_bytes = upload_bytes, before_draws = draw_calls;
    SDL_Texture *texture = Draw(&device, &scene, &mesh, workers, gpu,
        moving || f == 0, clipping, matrix, &prepare_ms);
    const double scene_ms = (SDL_GetTicksNS() - start) / 1e6;
    if (!texture || !SDL_RenderTexture(renderer, texture, NULL, NULL) || !SDL_RenderPresent(renderer)) goto done;
    const double elapsed = (SDL_GetTicksNS() - start) / 1e6;
    if (f >= kWarmup) {
      times[f - kWarmup] = elapsed;
      frame_total += elapsed; prepare_total += prepare_ms; scene_total += scene_ms;
      bytes += upload_bytes - before_bytes; draws += draw_calls - before_draws;
    }
  }
  qsort(times, (size_t)frames, sizeof(*times), CompareDouble);
  printf("{\"mode\":\"%s\",\"camera\":\"%s\",\"helpers_requested\":%d,\"objects\":%d,"
      "\"vertices\":%zu,\"frames\":%d,\"prepare_ms\":%.6f,\"scene_cpu_ms\":%.6f,\"frame_ms\":%.6f,"
      "\"frame_p50_ms\":%.6f,\"frame_p95_ms\":%.6f,\"geometry_bytes_per_frame\":%.1f,\"draws_per_frame\":%.1f}\n",
      argv[1], argv[2], helpers, kObjects, scene.count, frames, prepare_total / frames, scene_total / frames,
      frame_total / frames, times[frames / 2], times[(frames - 1) * 95 / 100],
      (double)bytes / frames, (double)draws / frames);
  result = 0;
done:
  if (result) fprintf(stderr, "model prototype failed/skipped (%d): %s\n", result, SDL_GetError());
  free(times);
  HostParallelWork_Destroy(workers);
  Sim3DDepthPass_DestroyMesh(mesh);
  Sim3DDepthPass_Reset(&device);
  ArRenderDevice_Reset(&device);
  SimBackgroundVoxelModelCache_Reset();
  free(scene.vertices); free(scene.projected); free(scene.clip); free(scene.valid);
  SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
  return result;
}
