/* Compare the terrain region transfer and its fallback using native room captures. */
#include <SDL3/SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../action_editor/room_scene.h"
#include "action/action_scene_snapshot.h"
#include "diorama/diorama_terrain_transfer.h"
#include "host/frame_producer.h"
#include "platform/sdl/render_sdl_interop.h"
#include "present/presentation_upload_mirror.h"

enum { kBanks = 3, kMaxWindows = 128 };

typedef struct TerrainWindow {
  ArRenderRectI bounds;
  EditorRoomTerrainSource source;
  uint32_t *pixels;
  unsigned tick;
} TerrainWindow;

typedef struct Contiguous {
  ArRenderTexture textures[kBanks];
  PresentationUploadMirror mirrors[kBanks];
  int width, height;
} Contiguous;

typedef struct Transfers {
  uint64_t bytes;
  unsigned calls;
} Transfers;

enum { kContiguous, kCaptureTransfer, kMethodCount };

static const uint32_t *TerrainBank(const TerrainWindow *window, unsigned bank) {
  return window->pixels + (size_t)bank * window->bounds.w * window->bounds.h;
}
static void FreeContiguous(ArRenderDevice *device, Contiguous *store) {
  for (unsigned bank = 0; bank < kBanks; ++bank) {
    ArRenderDevice_DestroyTexture(device, store->textures[bank]);
    PresentationUploadMirror_Reset(&store->mirrors[bank]);
  }
  *store = (Contiguous){0};
}

static bool UpdateContiguous(ArRenderDevice *device, Contiguous *store,
    const TerrainWindow *window, Transfers *transfers) {
  const int w = window->bounds.w, h = window->bounds.h;
  if (w != store->width || h != store->height) {
    FreeContiguous(device, store);
    if (!DioramaTerrainTextures(device, store->textures, w, h)) return false;
    store->width = w;
    store->height = h;
  }
  if (!ArRenderDevice_BeginTextureUploads(device)) return false;
  bool ok = true;
  for (unsigned bank = 0; ok && bank < kBanks; ++bank) {
    PresentationUploadResult update;
    ok = PresentationUploadMirror_UploadArgb8888(&store->mirrors[bank], device,
        store->textures[bank], (const uint8_t *)TerrainBank(window, bank), w, h, w * 4, 0, 0, &update);
    if (ok && update.changed) {
      ++transfers->calls;
      transfers->bytes += update.uploaded_bytes;
    }
  }
  return ArRenderDevice_EndTextureUploads(device, ok) && ok;
}

static bool EqualPixels(const uint32_t *a, int a_pitch, const uint32_t *b, int b_pitch,
    int w, int h) {
  for (int y = 0; y < h; ++y)
    if (memcmp(a + (size_t)y * a_pitch, b + (size_t)y * b_pitch, (size_t)w * sizeof(*a)))
      return false;
  return true;
}

static bool Validate(ArRenderDevice *device, const ArRenderTexture textures[kBanks],
    const TerrainWindow *window) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *previous_target = SDL_GetRenderTarget(renderer);
  const ArRenderRectI bounds = window->bounds;
  SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_TARGET, bounds.w, bounds.h);
  bool ok = target && SDL_SetRenderTarget(renderer, target);
  for (unsigned bank = 0; ok && bank < kBanks; ++bank) {
    SDL_Texture *art = ArSdlRenderBackend_UnwrapTexture(textures[bank]);
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    ok = SDL_GetTextureBlendMode(art, &blend) && SDL_SetTextureBlendMode(art, SDL_BLENDMODE_NONE) &&
        SDL_RenderTexture(renderer, art, NULL, NULL);
    SDL_Surface *read = ok ? SDL_RenderReadPixels(renderer, NULL) : NULL;
    SDL_Surface *argb = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_ARGB8888) : NULL;
    ok = argb && argb->w == bounds.w && argb->h == bounds.h &&
        EqualPixels(argb->pixels, argb->pitch / 4, TerrainBank(window, bank), bounds.w, bounds.w, bounds.h);
    SDL_DestroySurface(argb);
    SDL_DestroySurface(read);
    ok = SDL_SetTextureBlendMode(art, blend) && ok;
    if (!ok) SDL_SetError("texture parity failed at tick %u, bank %u", window->tick, bank);
  }
  if (target && !SDL_SetRenderTarget(renderer, previous_target)) ok = false;
  SDL_DestroyTexture(target);
  return ok;
}

static bool RunSequence(ArRenderDevice *device, SDL_GPUDevice *gpu, const TerrainWindow *windows,
    unsigned count, unsigned method, int repeat, FILE *csv) {
  const char *names[kMethodCount] = {"contiguous", "capture-transfer"};
  Contiguous contiguous = {0};
  DioramaTerrainSpare spare = {0};
  bool ok = true;
  for (unsigned frame = 0; ok && frame < count; ++frame) {
    const TerrainWindow *window = &windows[frame];
    Transfers transfers = {0};
    const uint64_t wall = SDL_GetTicksNS(), cpu = HostFrameProducer_ThreadCpuTimeNs();
    if (method == kCaptureTransfer) {
      const TerrainWindow *previous = frame ? &windows[frame - 1] : NULL;
      const uint32_t *pixels[3], *old[3];
      for (unsigned bank = 0; bank < kBanks; ++bank) {
        pixels[bank] = TerrainBank(window, bank);
        old[bank] = previous ? TerrainBank(previous, bank) : NULL;
      }
      ok = DioramaTransferTerrainWindow(device, &spare, contiguous.textures, window->bounds,
          previous ? previous->bounds : (ArRenderRectI){0},
          previous && EditorRoomTerrainSource_Equals(previous->source, window->source),
          pixels, old, &transfers.bytes, &transfers.calls);
    } else {
      ok = UpdateContiguous(device, &contiguous, window, &transfers);
    }
    const uint64_t cpu_ns = HostFrameProducer_ThreadCpuTimeNs() - cpu;
    const uint64_t wall_ns = SDL_GetTicksNS() - wall;
    if (ok && repeat < 0) ok = Validate(device, contiguous.textures, window);
    if (ok) ok = SDL_WaitForGPUIdle(gpu);
    const uint64_t serial_ns = SDL_GetTicksNS() - wall;
    if (ok && repeat >= 0)
      fprintf(csv, "%s,%d,%u,%u,%d,%d,%d,%d,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
          names[method], repeat, frame, window->tick, window->bounds.x, window->bounds.y,
          window->bounds.w, window->bounds.h, transfers.calls, transfers.bytes, cpu_ns, wall_ns, serial_ns);
  }
  FreeContiguous(device, &contiguous);
  DioramaTerrainSpare_Reset(device, &spare);
  return ok;
}

static unsigned CaptureWindows(EditorRoomScene *scene, FILE *route, TerrainWindow *windows) {
  char line[160];
  unsigned count = 0;
  while (fgets(line, sizeof(line), route)) {
    if (count == kMaxWindows) return 0;
    TerrainWindow *window = &windows[count];
    ArRenderRectI *r = &window->bounds;
    if (sscanf(line, "%u,%d,%d,%d,%d", &window->tick, &r->x, &r->y, &r->w, &r->h) != 5 ||
        r->x < -4096 || r->x > 8192 || r->y < -4096 || r->y > 8192 ||
        r->w <= 0 || r->w > kDioramaVoxelTerrainMaximum ||
        r->h <= 0 || r->h > kDioramaVoxelTerrainMaximum) {
      SDL_SetError("invalid terrain window at route line %u", count + 1);
      return 0;
    }
    const int cx = SDL_clamp(r->x + r->w / 2 - 128, 0, (int)EditorRoomScene_Width(scene) - 256);
    const int cy = SDL_clamp(r->y + r->h / 2 - 112, 0, (int)EditorRoomScene_Height(scene) - 224);
    if (!EditorRoomScene_Render(scene, cx, cy, window->tick, 120, 64, NULL) ||
        !EditorRoomScene_VoxelTerrainSource(scene, &window->source)) {
      SDL_SetError("room terrain unavailable for tick %u at %d,%d", window->tick, cx, cy);
      return 0;
    }
    const size_t pixels = (size_t)r->w * r->h;
    window->pixels = malloc(kBanks * pixels * sizeof(*window->pixels));
    uint32_t *native = malloc(pixels * sizeof(*native));
    uint32_t *banks[kBanks];
    for (unsigned bank = 0; window->pixels && bank < kBanks; ++bank)
      banks[bank] = window->pixels + bank * pixels;
    const bool ok = window->pixels && native && EditorRoomScene_CaptureVoxelTerrain(scene, *r, banks, native);
    free(native);
    if (!ok) {
      SDL_SetError("terrain capture failed at tick %u (%d,%d %dx%d)", window->tick, r->x,r->y,r->w,r->h);
      return 0;
    }
    ++count;
  }
  return ferror(route) ? 0 : count;
}

int TerrainUploadBenchmark_Main(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr, "usage: %s --terrain-uploads output.csv layers.ini room.arscene windows.csv repeats\n", argv[0]);
    return 2;
  }
  char *end;
  const long repeats = strtol(argv[6], &end, 10);
  if (end == argv[6] || *end || repeats <= 0 || repeats > 100) return 2;
  int result = 1;
  TerrainWindow windows[kMaxWindows] = {0};
  EditorRoomScene *scene = NULL;
  SDL_Window *window = NULL;
  ArRenderDevice device = {0};
  FILE *out = NULL, *route = NULL;
  size_t size;
  char *ini = SDL_LoadFile(argv[3], NULL);
  uint8_t *snapshot = SDL_LoadFile(argv[4], &size);
  ActionSceneSnapshot *assets = malloc(sizeof(*assets));
  if (!ini || !snapshot || !assets ||
      !ActionSceneSnapshot_Decode(snapshot, size, assets)) goto done;
  scene = EditorRoomScene_Create(assets);
  if (!scene || !EditorRoomScene_Configure(scene, ini)) goto done;
  EditorRoomScene_Set3DMode(scene, kAction3D_Voxel);
  EditorRoomScene_EnableEffects(scene, false);
  route = fopen(argv[5], "r");
  const unsigned count = route ? CaptureWindows(scene, route, windows) : 0;
  if (!count || !SDL_Init(SDL_INIT_VIDEO)) goto done;
  window = SDL_CreateWindow("Voxel terrain upload benchmark", 1280, 800, SDL_WINDOW_HIDDEN);
  if (!window || !ArSdlRenderBackend_CreateForWindow(&device, window, getenv("SDL_GPU_DRIVER"))) goto done;
  SDL_GPUDevice *gpu = ArSdlRenderBackend_GpuDevice(&device);
  if (!gpu) goto done;
  fprintf(stderr, "GPU backend: %s, %u native room windows\n", SDL_GetGPUDeviceDriver(gpu), count);
  for (unsigned method = 0; method < kMethodCount; ++method)
    if (!RunSequence(&device, gpu, windows, count, method, -1, NULL)) goto done;
  fprintf(stderr, "GPU readback matched all three banks for every window on all paths\n");
  out = fopen(argv[2], "w");
  if (!out) goto done;
  fprintf(out, "method,repeat,frame,tick,x,y,width,height,uploads,bytes,cpu_ns,wall_ns,serial_ns\n");
  for (int repeat = 0; repeat < repeats; ++repeat)
    for (unsigned method = 0; method < kMethodCount; ++method)
      if (!RunSequence(&device, gpu, windows, count, (method + repeat) % kMethodCount, repeat, out)) goto done;
  result = 0;
done:
  if (out && fclose(out)) result = 1;
  if (result) fprintf(stderr, "terrain upload benchmark failed: %s\n", SDL_GetError());
  if (route) fclose(route);
  ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  if (scene) EditorRoomScene_Destroy(scene);
  for (unsigned i = 0; i < kMaxWindows; ++i) free(windows[i].pixels);
  free(assets);
  SDL_free(snapshot);
  SDL_free(ini);
  return result;
}
