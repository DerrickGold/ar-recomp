/* ROM-free visual review of the in-game prototype renderer. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/sdl/render_sdl_internal.h"
#include "sim/church/church_scene.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/sim3d/sim3d_performance.h"
#include "sim/voxels/sim_background_voxel_model_cache.h"

/* The standalone preview never invokes live-SIM mountain effects. */
bool SimBackgroundVoxelRenderer_Ready(uint32_t serial) {
  (void)serial;
  return false;
}

void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  (void)vertices;
  (void)indices;
}
void Sim3DPerformance_AddGeometryUpload(uint64_t bytes) { (void)bytes; }
void Sim3DPerformance_AddGeometryCopy(uint64_t bytes, uint64_t calls) {
  (void)bytes;
  (void)calls;
}
void Sim3DPerformance_AddAtlasCopy(uint64_t bytes) { (void)bytes; }

static void ExampleTown(SimBackgroundVoxelScene *scene) {
  *scene = (SimBackgroundVoxelScene){.town = 1};
  for (int y = 7; y < 22; y += 2) {
    for (int x = 9; x < 20; x += 2) {
      if (x == 13) continue;
      scene->objects[scene->object_count++] = (SimBackgroundVoxelObject){
          .kind = (x + y) % 3 ? kSimBackgroundVoxel_House : kSimBackgroundVoxel_Tree,
          .town = 1,
          .development_level = 2,
          .cell_x = x,
          .cell_y = y,
          .source_cells_w = 1,
          .source_cells_h = 1,
          .footprint_cells_w = 1,
          .footprint_cells_d = 1,
          .visual_state = kSimStructureVisualState_Finished,
      };
    }
  }
  scene->objects[scene->object_count++] = (SimBackgroundVoxelObject){
      .kind = kSimBackgroundVoxel_Cathedral,
      .town = 1,
      .cell_x = 13,
      .cell_y = 3,
      .source_cells_w = 2,
      .source_cells_h = 2,
      .footprint_cells_w = 2,
      .footprint_cells_d = 2,
  };
}

static bool EqualImage(const SDL_Surface *a, const SDL_Surface *b) {
  if (!a || !b || a->w != b->w || a->h != b->h || a->format != b->format) return false;
  const int row = a->w * SDL_BYTESPERPIXEL(a->format);
  for (int y = 0; y < a->h; y++)
    if (memcmp((const char *)a->pixels + y * a->pitch, (const char *)b->pixels + y * b->pitch, row))
      return false;
  return true;
}
static bool VerifyActors(ArRenderDevice *device, SimBackgroundVoxelScene *town,
                         const uint32_t *ground, ChurchSceneOptions options) {
  uint32_t *pixels = calloc(256 * 512, sizeof(*pixels));
  if (!pixels) return false;
  ArRenderTexture atlas = ArRenderTexture_Invalid();
  const ArRenderTextureDesc desc = {.width = 256,
                                    .height = 512,
                                    .format = kArRenderPixelFormat_Argb8888,
                                    .usage = kArRenderTextureUsage_Streaming,
                                    .filter = kArRenderFilter_Nearest,
                                    .blend = kArRenderBlendMode_Alpha};
  const ArRenderRectI viewport = {0, 0, 1280, 800};
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  bool ok = ArRenderDevice_CreateTexture(device, &desc, &atlas) &&
            ArRenderDevice_UpdateTexture(device, atlas, NULL, pixels, 256 * 4);
  options.people = atlas;
  ok = ok && ChurchScene_Draw(device, viewport, town, ground, &options);
  const ChurchSceneStats cached = ChurchScene_Stats();
  SDL_Surface *empty = SDL_RenderReadPixels(renderer, NULL);
  options.actor_bounds[0] = (ArRenderRectI){8, 5, 16, 23};
  for (int y = 5; y < 28; y++)
    for (int x = 8; x < 24; x++)
      if (y < 24 || (x >= 12 && x < 20)) pixels[(224 + y) * 256 + x] = 0xffffffff;
  ok = ok && ArRenderDevice_UpdateTexture(device, atlas, NULL, pixels, 256 * 4) &&
       ChurchScene_Draw(device, viewport, town, ground, &options);
  SDL_Surface *lit = SDL_RenderReadPixels(renderer, NULL);
  Uint8 r = 0, g, b, a;
  ok = ok && lit && SDL_ReadSurfacePixel(lit, 574, 350, &r, &g, &b, &a) && r > 100 && r < 245;
  /* An opaque white figure can only brighten this floor; darker surrounding
   * pixels therefore demonstrate the separate soft contact shadow. */
  unsigned darkened = 0;
  if (empty && lit) {
    for (int y = 360; y < 400; y++)
      for (int x = 525; x < 630; x++) {
        Uint8 er, eg, eb, ea, lr, lg, lb, la;
        if (SDL_ReadSurfacePixel(empty, x, y, &er, &eg, &eb, &ea) &&
            SDL_ReadSurfacePixel(lit, x, y, &lr, &lg, &lb, &la) && lr + lg + lb + 10 < er + eg + eb)
          darkened++;
      }
    /* No second person means no second shadow or change on that side. */
    const int bpp = SDL_BYTESPERPIXEL(empty->format);
    for (int y = 0; y < empty->h && ok; y++)
      ok = !memcmp((const char *)empty->pixels + y * empty->pitch + 640 * bpp,
                   (const char *)lit->pixels + y * lit->pitch + 640 * bpp, 640 * bpp);
  }
  ok = ok && darkened > 0;
  for (int y = 5; y < 28; y++)
    for (int x = 8; x < 24; x++)
      if (y < 24 || (x >= 12 && x < 20)) pixels[(224 + y) * 256 + x] = 0xff808080;
  options.brightness = .5f;
  ok = ok && ArRenderDevice_UpdateTexture(device, atlas, NULL, pixels, 256 * 4) &&
       ChurchScene_Draw(device, viewport, town, ground, &options);
  SDL_Surface *faded = SDL_RenderReadPixels(renderer, NULL);
  Uint8 fr = 0;
  ok = ok && faded && SDL_ReadSurfacePixel(faded, 574, 350, &fr, &g, &b, &a) &&
       abs((int)fr * 2 - r) <= 4; /* Captured native fade is applied exactly once. */
  memset(pixels, 0, 256 * 512 * sizeof(*pixels));
  options.actor_bounds[0] = (ArRenderRectI){0};
  options.brightness = 1;
  ok = ok && ArRenderDevice_UpdateTexture(device, atlas, NULL, pixels, 256 * 4) &&
       ChurchScene_Draw(device, viewport, town, ground, &options);
  SDL_Surface *gone = SDL_RenderReadPixels(renderer, NULL);
  ok = ok && EqualImage(empty, gone) && ChurchScene_Stats().room_bakes == cached.room_bakes &&
       ChurchScene_Stats().geometry_builds == cached.geometry_builds;
  SDL_DestroySurface(empty);
  SDL_DestroySurface(lit);
  SDL_DestroySurface(faded);
  SDL_DestroySurface(gone);
  ArRenderDevice_DestroyTexture(device, atlas);
  free(pixels);
  printf("church native sprites: light, contact shadow, absent actor, single fade and cache reuse "
         "%s\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static bool Verify(ArRenderDevice *device, SimBackgroundVoxelScene *town) {
  const ChurchSceneOptions normal = {.brightness = 1,
                                     .seconds = 22,
                                     .town_serial = 1,
                                     .shading = kSimBackgroundVoxelShading_MaterialAware,
                                     .style = kSimBackgroundVoxelStyle_Varied,
                                     .light_azimuth_deg = 225,
                                     .light_elevation_deg = 38};
  ChurchSceneOptions options = normal;
  const ArRenderRectI viewport = {0, 0, 1280, 800};
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  bool ok = ChurchScene_Draw(device, viewport, town, NULL, &options);
  SDL_Surface *cold = SDL_RenderReadPixels(renderer, NULL);
  for (int i = 0; i < 3 && ok; i++)
    ok = ChurchScene_Draw(device, viewport, town, NULL, &options);
  SDL_Surface *warm = SDL_RenderReadPixels(renderer, NULL);
  ok = ok && EqualImage(cold, warm) && ChurchScene_Stats().room_bakes == 1 &&
       ChurchScene_Stats().geometry_builds == 1;
  SDL_DestroySurface(warm);
  /* This crop is inside the nave, below the exterior aperture. The only
   * time-dependent pixels here are the motes, so caching must not freeze it. */
  options.seconds = 22.5;
  ok = ok && ChurchScene_Draw(device, viewport, town, NULL, &options);
  warm = SDL_RenderReadPixels(renderer, NULL);
  bool dust_moved = false;
  if (cold && warm && cold->format == warm->format) {
    const int bpp = SDL_BYTESPERPIXEL(cold->format);
    for (int y = 320; y < 480 && !dust_moved; y++)
      dust_moved = memcmp((const char *)cold->pixels + y * cold->pitch + 430 * bpp,
                          (const char *)warm->pixels + y * warm->pitch + 430 * bpp, 420 * bpp) != 0;
  }
  ok = ok && dust_moved && ChurchScene_Stats().room_bakes == 1 &&
       ChurchScene_Stats().geometry_builds == 1;
  SDL_DestroySurface(warm);
  options.brightness = .5f;
  ok = ok && ChurchScene_Draw(device, viewport, town, NULL, &options) &&
       ChurchScene_Stats().room_bakes == 1;
  options = normal;
  ok = ok && ChurchScene_Draw(device, viewport, town, NULL, &options);
  warm = SDL_RenderReadPixels(renderer, NULL);
  ok = ok && EqualImage(cold, warm);
  SDL_DestroySurface(warm);
  /* Resizing rebakes images without recompiling the world/lighting. */
  ok = ok && ChurchScene_Draw(device, (ArRenderRectI){0, 0, 800, 600}, town, NULL, &options) &&
       ChurchScene_Stats().room_bakes == 2 && ChurchScene_Stats().geometry_builds == 1;
  ok = ok && ChurchScene_Draw(device, viewport, town, NULL, &options) &&
       ChurchScene_Stats().room_bakes == 3;
  warm = SDL_RenderReadPixels(renderer, NULL);
  ok = ok && EqualImage(cold, warm);
  SDL_DestroySurface(warm);
  ChurchScene_Reset(device);
  ok = ok && ChurchScene_Draw(device, viewport, town, NULL, &options);
  warm = SDL_RenderReadPixels(renderer, NULL);
  ok = ok && EqualImage(cold, warm);
  SDL_DestroySurface(warm);
  SDL_DestroySurface(cold);
  /* A newly founded town may have no models visible beyond its cathedral.
   * Empty exterior geometry must not force a new room bake every frame. */
  ChurchScene_Reset(device);
  town->objects[0] = town->objects[town->object_count - 1];
  town->object_count = 1;
  options.town_serial++;
  uint32_t *ground = malloc(512 * 512 * sizeof(*ground));
  ok = ok && ground;
  if (ground)
    for (int i = 0; i < 512 * 512; i++)
      ground[i] = 0xff507834;
  for (int i = 0; i < 3 && ok; i++)
    ok = ChurchScene_Draw(device, viewport, town, ground, &options);
  ok = ok && ChurchScene_Stats().room_bakes == 1 && ChurchScene_Stats().geometry_builds == 1;
  ok = ok && VerifyActors(device, town, ground, options);
  free(ground);
  printf("church cached images: cold/warm/reset parity, live dust, fade/time reuse, viewport "
         "invalidation %s\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

int main(int argc, char **argv) {
  bool verify = argc > 1 && !strcmp(argv[1], "--verify");
  const char *output = argc > 1 && !verify ? argv[1] : NULL;
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "%s\n", SDL_GetError());
    return verify ? 77 : 1;
  }
  SDL_Window *window =
      SDL_CreateWindow("Church prototype — Master's chair", 1280, 800,
                       (output || verify) ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
  SDL_unsetenv_unsafe("AR_SDL_GPU_ORDERED");
  ArRenderDevice device = {0};
  if (!window || !ArSdlRenderBackend_CreateForWindow(&device, window, NULL)) return verify ? 77 : 1;
  SimBackgroundVoxelScene *town = calloc(1, sizeof(*town));
  if (!town) return 1;
  ExampleTown(town);
  Sim3DDepthPass_PreparePipelines(&device);
  bool running = !verify, valid = !verify || Verify(&device, town);
  while (running && valid) {
    SDL_Event event;
    while (SDL_PollEvent(&event))
      if (event.type == SDL_EVENT_QUIT ||
          (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE))
        running = false;
    int width, height;
    valid = ArRenderDevice_GetOutputSize(&device, &width, &height);
    const char *reference = getenv("AR_CHURCH_ALTAR_REFERENCE");
    ChurchSceneOptions options = {.brightness = 1,
                                  .reference_altar = reference && !strcmp(reference, "1"),
                                  .seconds = output ? 22.0 : SDL_GetTicks() / 1000.0,
                                  .shading = kSimBackgroundVoxelShading_MaterialAware,
                                  .style = kSimBackgroundVoxelStyle_Varied,
                                  .light_azimuth_deg = 225,
                                  .light_elevation_deg = 38,
                                  .landscape_height_pct = 0};
    valid = valid &&
            ChurchScene_Draw(&device, (ArRenderRectI){0, 0, width, height}, town, NULL, &options);
    if (output) {
      SDL_Surface *surface = SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&device), NULL);
      valid = valid && surface && SDL_SaveBMP(surface, output);
      SDL_DestroySurface(surface);
      running = false;
    } else {
      valid = valid && ArRenderDevice_Present(&device);
      SDL_Delay(16);
    }
  }
  if (!valid)
    fprintf(stderr, "church preview: %s / %s\n", ArRenderDevice_LastError(&device),
            Sim3DDepthPass_LastError());
  ChurchScene_Reset(&device);
  Sim3DDepthPass_Reset(&device);
  SimBackgroundVoxelModelCache_Reset();
  free(town);
  ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return valid ? 0 : 1;
}
