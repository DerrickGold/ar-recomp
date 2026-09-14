/* Frozen-scene integration test: production presenter, model compiler,
 * atlases, SDL GPU shaders and shared D32 depth. No runner, live input,
 * settings persistence or save writes. Optional ROM/WRAM inputs are read-only. */
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/render_sdl_internal.h"
#include "present_internal.h"
#include "present_sim3d_internal.h"
#include "present_sim_globe.h"
#include "present_sim_globe_mountains.h"
#include "present_sim_globe_project.h"
#include "present_sim_globe_terrain.h"
#include "present_sim_globe_water.h"
#include "present_world_nav_model_mesh.h"
#include "render/render_output.h"
#include "render/localized_text_presenter.h"
#include "settings.h"
#include "performance_metrics.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim3d_camera_limits.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim_town_ground_art.h"
#include "sim/sim_background_voxel_model_cache.h"
#include "sim/sim_world_navigation_mountains.h"
#include "sim/sim_world_navigation_terrain.h"
#include "sim/sim_world_map_compose.h"
#include "sim/sim_town_canvas.h"
#include "sim/sim_background_voxels.h"
#include "sim/sim_background_mountain_render.h"
#include "present_sim3d_terrain.h"
#include "present_sim3d_clouds.h"
#include "session_fatal.h"

enum { kWidth = 800, kHeight = 600, kRomBytes = 0x100000, kWramBytes = 0x20000 };
#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #test, SDL_GetError()); \
  exit(1); \
} } while (0)

ArRenderDevice g_render_device;
uint32_t g_sim_world_navigation_palace_pixels[
    kSimWorldNavigationCompositionWidth * kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_label_pixels[
    kSimWorldNavigationCompositionWidth * kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_plaque_pixels[
    kSimWorldNavigationCompositionWidth * kSimWorldNavigationCompositionHeight];

const ArLocalizationScreenTextRecord *ArLocalizationFrame_FindScreenText(
    const ArLocalizationFrame *frame, uint32_t surface_id) {
  (void)frame;
  (void)surface_id;
  return NULL;
}

bool ArLocalizedTextPresenter_PrepareScreenText(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    uint32_t surface_id, ArRenderRectI bounds,
    ArLocalizedPreparedFrame *prepared) {
  (void)device;
  (void)frame;
  (void)surface_id;
  (void)bounds;
  if (prepared) memset(prepared, 0, sizeof(*prepared));
  return false;
}

bool ArLocalizedTextPresenter_DrawWithBrightness(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared,
    float brightness) {
  (void)device;
  (void)prepared;
  (void)brightness;
  return true;
}

/* Time is the only substituted host service. Baseline images use zero;
 * motion tests advance just the weather clock while scene/game data stay
 * frozen. Profiling time does not advance with those artificial jumps. */
static uint64_t weather_time_ms;
uint64_t HostClock_Milliseconds(void) { return weather_time_ms; }
uint64_t HostClock_Nanoseconds(void) { return 0; }

static const char *output_directory;
static bool weather_sequence_requested;
static bool town_matrix_requested;
static bool sim_globe_prototype_requested;
static const char *sim_town_snapshot;
static bool sim_height_sweep_requested;
static unsigned sim_town_radius_scale = 3;
static unsigned sim_town_landscape_pct = kSimTownTerrainLandscapeHeightDefaultPct;
static void ResizeTestOutput(SDL_Renderer *renderer, int width, int height);

static uint32_t Pixel(const SDL_Surface *surface, int x, int y) {
  uint32_t pixel;
  memcpy(&pixel, (const uint8_t *)surface->pixels + y * surface->pitch + x * 4, 4);
  return pixel;
}

static void SaveImage(const SDL_Surface *surface, const char *name) {
  if (!output_directory || !name) return;
  char path[4096];
  const int length = snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name);
  CHECK(length > 0 && (size_t)length < sizeof(path));
  FILE *file = fopen(path, "wb");
  CHECK(file);
  CHECK(fprintf(file, "P6\n%d %d\n255\n", surface->w, surface->h) > 0);
  uint8_t *row = malloc((size_t)surface->w * 3);
  CHECK(row);
  for (int y = 0; y < surface->h; y++) {
    for (int x = 0; x < surface->w; x++) {
      const uint32_t pixel = Pixel(surface, x, y);
      row[x * 3] = (uint8_t)(pixel >> 16);
      row[x * 3 + 1] = (uint8_t)(pixel >> 8);
      row[x * 3 + 2] = (uint8_t)pixel;
    }
    CHECK(fwrite(row, 3, (size_t)surface->w, file) == (size_t)surface->w);
  }
  free(row);
  CHECK(fclose(file) == 0);
  printf("capture %s\n", path);
}

static SDL_Surface *Render(SDL_Renderer *renderer, const FrameSlot *slot, const char *name) {
  if (slot->sim.view == kSimView_SkyPalace) {
    const ArRenderColorF black = {0, 0, 0, 1};
    ArRenderOutputFrame output;
    CHECK(ArRenderOutputFrame_BeginAspectFit(&g_render_device, slot->ignore_aspect_ratio,
        slot->visible_width * 7, slot->snes_height * 6, black, black, &output));
    CHECK(PresentWorldNavigationBackdrop(slot,
        (ArRenderRectI){0, 0, output.viewport.w, output.viewport.h}) == kPresentationOutcome_Complete);
    CHECK(ArRenderOutputFrame_Finish(&output));
  } else {
    CHECK(PresentWorldNavigation3D(slot) == kPresentationOutcome_Complete);
  }
  int width, height;
  CHECK(ArRenderDevice_GetOutputSize(&g_render_device, &width, &height));
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
  CHECK(readback);
  SDL_Surface *surface = SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(readback);
  CHECK(surface && surface->w == width && surface->h == height);
  /* Match the application's frame lifecycle. In particular, the GPU
   * renderer must release its acquired output before a window resize;
   * readback alone can keep an old-size target alive. */
  CHECK(SDL_RenderPresent(renderer));
  SaveImage(surface, name);
  return surface;
}

static int Differences(const SDL_Surface *a, const SDL_Surface *b) {
  CHECK(a->w == b->w && a->h == b->h);
  int count = 0;
  for (int y = 0; y < a->h; y++)
    for (int x = 0; x < a->w; x++) count += Pixel(a, x, y) != Pixel(b, x, y);
  return count;
}

static int ColorCount(const SDL_Surface *surface, uint32_t color) {
  int count = 0;
  for (int y = 0; y < surface->h; y++)
    for (int x = 0; x < surface->w; x++) count += Pixel(surface, x, y) == color;
  return count;
}

static int FirstColorX(const SDL_Surface *surface, uint32_t color) {
  for (int y = 0; y < surface->h; y++)
    for (int x = 0; x < surface->w; x++) if (Pixel(surface, x, y) == color) return x;
  return -1;
}

static int FirstColorY(const SDL_Surface *surface, uint32_t color) {
  for (int y = 0; y < surface->h; y++)
    for (int x = 0; x < surface->w; x++) if (Pixel(surface, x, y) == color) return y;
  return -1;
}

static void CheckColorMaskEqual(const SDL_Surface *a, const SDL_Surface *b, uint32_t color) {
  CHECK(a->w == b->w && a->h == b->h);
  for (int y = 0; y < a->h; y++)
    for (int x = 0; x < a->w; x++)
      CHECK((Pixel(a, x, y) == color) == (Pixel(b, x, y) == color));
}

static ArRenderRectI ColoredBounds(const SDL_Surface *surface) {
  int left = surface->w, right = -1, top = surface->h, bottom = -1;
  for (int y = 0; y < surface->h; y++)
    for (int x = 0; x < surface->w; x++) {
      if (!(Pixel(surface, x, y) & 0x00ffffff)) continue;
      if (x < left) left = x;
      if (x > right) right = x;
      if (y < top) top = y;
      if (y > bottom) bottom = y;
    }
  CHECK(right >= left && bottom >= top);
  return (ArRenderRectI){left, top, right - left + 1, bottom - top + 1};
}

static void TestFraming(SDL_Renderer *renderer, FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.world_navigation_scene.composition.empty_animation = true;
  probe->sim.world_navigation_models = false;
  probe->sim.world_navigation_clouds = false;
  probe->sim.world_navigation_backdrop = false;
  probe->sim.world_navigation_atmosphere = true;
  probe->sim.projection_pitch_mrad = -575;
  probe->sim.projection_yaw_mrad = 650;
  probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
  UploadWorldNavigationComposition(probe);
  for (int zoom = 3; zoom <= 10; zoom += zoom == 3 ? 2 : 5) {
    probe->sim.projection_distance_x100 = zoom * 100;
    SDL_Surface *travel = Render(renderer, probe, NULL);
    /* Normal travel is centered too. Persisted oblique town poses cannot
     * move the globe or skew the surface under the top-down Palace sprite. */
    for (int step = 0; step <= 4; step++) {
      probe->sim.projection_pitch_mrad = -1300 + step * 300;
      probe->sim.projection_yaw_mrad = -650 + step * 300;
      SDL_Surface *view = Render(renderer, probe, step == 4 && zoom == 3 ? "synthetic-centred" : NULL);
      const ArRenderRectI bounds = ColoredBounds(view);
      const float dx = bounds.x + bounds.w * .5f - kWidth * .5f;
      const float dy = bounds.y + bounds.h * .5f - kHeight * .5f;
      CHECK(fabsf(dx) <= 2 && fabsf(dy) <= 2);
      /* The 2x globe deliberately extends beyond the near viewport. Wide
       * navigation must still reveal its entire centered silhouette. */
      if (zoom >= 5)
        CHECK(bounds.x > 0 && bounds.y > 0 && bounds.x + bounds.w < kWidth && bounds.y + bounds.h < kHeight);
      else
        CHECK(bounds.y == 0 && bounds.h == kHeight);
      CHECK(Differences(travel, view) == 0);
      SDL_DestroySurface(view);
    }
    SDL_Surface *restored = Render(renderer, probe, NULL);
    CHECK(Differences(travel, restored) == 0);
    SDL_DestroySurface(travel);
    SDL_DestroySurface(restored);
  }
  /* The shared depth pass and whole-model horizon test must agree with the
   * centered radial eye, including manual rotation to the far hemisphere. */
  probe->sim.projection_distance_x100 = 300;
  for (int side = 0; side < 2; side++) {
    probe->sim_manual_orbit_yaw = side * kPi;
    probe->sim.world_navigation_models = false;
    SDL_Surface *ground = Render(renderer, probe, NULL);
    probe->sim.world_navigation_models = true;
    SDL_Surface *model = Render(renderer, probe, NULL);
    CHECK(side ? Differences(ground, model) == 0 : Differences(ground, model) > 20);
    SDL_DestroySurface(model);
    SDL_DestroySurface(ground);
  }
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestAtmosphereProfile(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.world_navigation_scene.composition.empty_animation = true;
  probe->sim.world_navigation_relief = probe->sim.world_navigation_models = false;
  probe->sim.world_navigation_clouds = probe->sim.world_navigation_backdrop = false;
  probe->sim.world_navigation_atmosphere = false;
  probe->sim.cloud_altitude_px = 512;
  probe->sim.projection_pitch_mrad = probe->sim.projection_yaw_mrad = 0;
  probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
  UploadWorldNavigationComposition(probe);
  SDL_Surface *off = Render(renderer, probe, NULL);
  const ArRenderRectI ocean = ColoredBounds(off);
  probe->sim.world_navigation_atmosphere = true;
  SDL_Surface *air = Render(renderer, probe, "synthetic-atmosphere-profile");
  int previous = 255, lit = 0, maximum_drop = 0;
  for (int x = ocean.x + ocean.w; x < kWidth; x++) {
    CHECK(!(Pixel(off, x, kHeight / 2) & 0x00ffffff));
    const int blue = Pixel(air, x, kHeight / 2) & 255;
    CHECK(blue <= previous + 1); /* No second bright glass rim outside the sea. */
    if (lit && previous - blue > maximum_drop) maximum_drop = previous - blue;
    if (blue) lit++;
    previous = blue;
  }
  CHECK(lit > 5 && previous == 0 && maximum_drop <= 20);
  probe->sim.world_navigation_atmosphere = false;
  SDL_Surface *restored = Render(renderer, probe, NULL);
  CHECK(Differences(off, restored) == 0);
  SDL_DestroySurface(restored);
  SDL_DestroySurface(air);
  SDL_DestroySurface(off);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void CheckSmallWeatherStep(const SDL_Surface *a, const SDL_Surface *b) {
  uint64_t total = 0;
  unsigned maximum = 0;
  int maximum_x = 0, maximum_y = 0;
  for (int y = 0; y < a->h; y++)
    for (int x = 0; x < a->w; x++) {
      const uint32_t before = Pixel(a, x, y), after = Pixel(b, x, y);
      for (int shift = 0; shift < 24; shift += 8) {
        const unsigned difference = (unsigned)abs((int)((before >> shift) & 255) -
            (int)((after >> shift) & 255));
        total += difference;
        if (difference > maximum) {
          maximum = difference; maximum_x = x; maximum_y = y;
        }
      }
    }
  const double mean = (double)total / (a->w * a->h * 3);
  printf("weather small step: time=%llu max=%u mean=%.5f at=(%d,%d)\n",
      (unsigned long long)weather_time_ms, maximum, mean, maximum_x, maximum_y);
  if (mean >= .25 || maximum > 12) {
    SaveImage(a, "weather-jump-before");
    SaveImage(b, "weather-jump-after");
  }
  CHECK(mean < .25 && maximum <= 12);
}

static void TestWeatherMotion(SDL_Renderer *renderer, const FrameSlot *slot,
                              const char *prefix, bool markers) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.cloud_drift_pct = 100;
  probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = true;
  probe->sim.world_navigation_lighting = true;
  const float views[][2] = {{0, 0}, {kPi, 0}, {0, kPi * .5f}, {0, -kPi * .5f}, {.8f, .6f}};
  const uint64_t boundaries[] = {151000, 153500, 166667, 3600000, UINT64_C(576000000)};
  for (size_t view = 0; view < sizeof(views) / sizeof(views[0]); view++) {
    probe->sim_manual_orbit_yaw = views[view][0];
    probe->sim_manual_orbit_pitch = views[view][1];
    weather_time_ms = 0;
    SDL_Surface *start = Render(renderer, probe, NULL);
    weather_time_ms = 60000;
    char name[96];
    CHECK(snprintf(name, sizeof(name), "%s-weather-%zu", prefix, view) > 0);
    SDL_Surface *moved = Render(renderer, probe, name);
    CHECK(Differences(start, moved) > 1000);
    if (markers) {
      CheckColorMaskEqual(start, moved, 0xffff00ff);
      CheckColorMaskEqual(start, moved, 0xff00ffff);
    }
    SDL_Surface *same = Render(renderer, probe, NULL);
    CHECK(Differences(moved, same) == 0);
    SDL_DestroySurface(same);
    for (size_t at = 0; at < sizeof(boundaries) / sizeof(boundaries[0]); at++) {
      weather_time_ms = boundaries[at] - 8;
      SDL_Surface *before = Render(renderer, probe, NULL);
      weather_time_ms = boundaries[at] + 8;
      SDL_Surface *after = Render(renderer, probe, NULL);
      CheckSmallWeatherStep(before, after);
      SDL_DestroySurface(before); SDL_DestroySurface(after);
    }
    weather_time_ms = 60000;
    PresentWorldNav_ResetResources();
    Sim3DDepthPass_Reset(&g_render_device);
    UploadWorldNavigationComposition(probe);
    SDL_Surface *reset = Render(renderer, probe, NULL);
    CHECK(Differences(moved, reset) == 0);
    SDL_DestroySurface(reset);
    weather_time_ms = 0;
    SDL_Surface *rewound = Render(renderer, probe, NULL);
    CHECK(Differences(start, rewound) == 0);
    SDL_DestroySurface(rewound);
    SDL_DestroySurface(start); SDL_DestroySurface(moved);
  }
  /* Below the cloud deck only displaced shadows remain. They must animate
   * too; turning weather off or drift to zero must stop every time change. */
  probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
  probe->sim.cloud_altitude_px = 256;
  CHECK(SimWorldNavigationScene_CloudVisibility(probe->sim.world_navigation.zoom_current, 256) == 0);
  weather_time_ms = 0;
  SDL_Surface *shadow = Render(renderer, probe, NULL);
  weather_time_ms = 60000;
  SDL_Surface *shadow_moved = Render(renderer, probe, NULL);
  CHECK(Differences(shadow, shadow_moved) > 1000);
  SDL_DestroySurface(shadow); SDL_DestroySurface(shadow_moved);
  for (size_t view = 0; view < sizeof(views) / sizeof(views[0]); view++) {
    probe->sim_manual_orbit_yaw = views[view][0];
    probe->sim_manual_orbit_pitch = views[view][1];
    weather_time_ms = 150992;
    SDL_Surface *before = Render(renderer, probe, NULL);
    weather_time_ms = 151008;
    SDL_Surface *after = Render(renderer, probe, NULL);
    CheckSmallWeatherStep(before, after);
    SDL_DestroySurface(before); SDL_DestroySurface(after);
  }
  probe->sim.cloud_altitude_px = slot->sim.cloud_altitude_px;
  probe->sim.world_navigation_cloud_shadows = false;
  weather_time_ms = 150992;
  SDL_Surface *body_before = Render(renderer, probe, NULL);
  weather_time_ms = 151008;
  SDL_Surface *body_after = Render(renderer, probe, NULL);
  CheckSmallWeatherStep(body_before, body_after);
  SDL_DestroySurface(body_before); SDL_DestroySurface(body_after);
  probe->sim.world_navigation_cloud_shadows = true;
  for (int off = 0; off < 3; off++) {
    probe->sim.world_navigation_clouds = off != 1;
    probe->sim.cloud_drift_pct = off == 0 ? 0 : 100;
    probe->sim.cloud_opacity_pct = off == 2 ? 0 : 35;
    weather_time_ms = 0;
    SDL_Surface *still = Render(renderer, probe, NULL);
    weather_time_ms = 60000;
    SDL_Surface *later = Render(renderer, probe, NULL);
    CHECK(Differences(still, later) == 0);
    SDL_DestroySurface(still); SDL_DestroySurface(later);
  }
  weather_time_ms = 0;
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void CaptureWeatherSequence(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.cloud_drift_pct = 100;
  probe->sim_manual_orbit_yaw = .8f;
  probe->sim_manual_orbit_pitch = .6f;
  /* Thirty seconds of unchanged native geography and camera, including a
   * cloud-bank phase wrap, captured at half-second intervals. Extra adjacent
   * 16 ms probes catch jumps instead of merely producing a smooth-looking
   * low-frame-rate preview. This longer sweep is opt-in, not a CTest cost. */
  for (unsigned frame = 0; frame < 60; frame++) {
    const uint64_t time = 150000 + frame * 500;
    weather_time_ms = time - 8;
    SDL_Surface *before = Render(renderer, probe, NULL);
    weather_time_ms = time + 8;
    SDL_Surface *after = Render(renderer, probe, NULL);
    CheckSmallWeatherStep(before, after);
    SDL_DestroySurface(before); SDL_DestroySurface(after);
    weather_time_ms = time;
    char name[96];
    CHECK(snprintf(name, sizeof(name), "weather-sequence-%03u", frame) > 0);
    SDL_Surface *view = Render(renderer, probe, name);
    SDL_DestroySurface(view);
  }
  weather_time_ms = 0;
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void BuildScene(FrameSlot *slot) {
  CHECK(SimWorldNavigationScene_Build(&slot->sim.world_navigation_scene,
      &slot->sim.world_navigation, SimWorldMap_Serial()));
  slot->sim.world_navigation_scene.composition = (SimWorldNavigationComposition){
      .valid = true, .empty_animation = true};
  slot->sim.underlay_serial = SimWorldMap_Serial();
  UploadWorldNavigationComposition(slot);
}

static void TestSkyPalace(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  *probe = *slot;
  SDL_Surface *navigation = Render(renderer, slot, NULL);
  probe->sim.view = kSimView_SkyPalace;
  probe->sim.sky_palace_volumetric_clouds = true;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int x, y;
    CHECK(SimWorldMap_OriginForTown(town, &x, &y));
    probe->sim.world_navigation.focus_x = (x + 16) * 8;
    probe->sim.world_navigation.focus_y = (y + 16) * 8;
    CHECK(SimWorldNavigationScene_BuildSkyPalace(&probe->sim.world_navigation_scene,
        probe->sim.world_navigation.focus_x, probe->sim.world_navigation.focus_y,
        town, SimWorldMap_Serial()));
    char name[64];
    CHECK(snprintf(name, sizeof(name), "palace-town-%u", town) > 0);
    SDL_Surface *view = Render(renderer, probe, name);
    CHECK(Differences(navigation, view) > 1000);
    const uint16_t focus_x = probe->sim.world_navigation.focus_x;
    const uint16_t focus_y = probe->sim.world_navigation.focus_y;
    probe->sim.world_navigation.focus_x = probe->sim.world_navigation.focus_y = 0;
    SDL_Surface *selected = Render(renderer, probe, NULL);
    CHECK(Differences(view, selected) == 0); /* Captured selection owns the Palace frame. */
    SDL_DestroySurface(selected);
    probe->sim.world_navigation.focus_x = focus_x;
    probe->sim.world_navigation.focus_y = focus_y;
    probe->sim.sky_palace_volumetric_clouds = false;
    SDL_Surface *cheap = Render(renderer, probe, NULL);
    CHECK(Differences(view, cheap) > 100);
    SDL_DestroySurface(cheap);
    probe->sim.sky_palace_volumetric_clouds = true;
    const uint32_t sky = Pixel(view, view->w / 2, 10);
    CHECK((sky & 255) > ((sky >> 16) & 255) + 40); /* Daylight, not black space. */
    weather_time_ms = 8000;
    SDL_Surface *held = Render(renderer, probe, NULL);
    CHECK(Differences(view, held) == 0); /* Drift zero is a real freeze. */
    SDL_DestroySurface(held);
    probe->sim.cloud_drift_pct = 100;
    SDL_Surface *moving = Render(renderer, probe, NULL);
    CHECK(Differences(view, moving) > 100);
    int lower_motion = 0;
    for (int y = view->h / 2; y < view->h * 3 / 4; y++)
      for (int x = view->w / 4; x < view->w * 3 / 4; x++)
        lower_motion += Pixel(view, x, y) != Pixel(moving, x, y);
    CHECK(lower_motion > 100); /* Motion crosses the globe, not only the sky. */
    int upper_motion = 0;
    for (int y = view->h / 6; y < view->h / 3; y++)
      for (int x = 0; x < view->w; x++)
        upper_motion += Pixel(view, x, y) != Pixel(moving, x, y);
    CHECK(upper_motion > 100); /* The added deck crosses the upper windows. */
    SDL_DestroySurface(moving);
    if (town == 1) {
      const uint64_t boundaries[] = {1185, 31620, 62055, /* Upper deck wraps. */
          2318, 24636, 46955, 66664, 100000, UINT64_C(576000000)};
      for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        weather_time_ms = boundaries[i];
        SDL_Surface *before = Render(renderer, probe, NULL);
        weather_time_ms += 8;
        SDL_Surface *after = Render(renderer, probe, NULL);
        CheckSmallWeatherStep(before, after);
        SDL_DestroySurface(before); SDL_DestroySurface(after);
      }
    }
    probe->sim.world_navigation_clouds = false;
    SDL_Surface *clear = Render(renderer, probe, NULL);
    const uint32_t window_sky = Pixel(clear, clear->w / 2, clear->h / 5);
    /* Check visible-window color without requiring a permanent hole in the clouds. */
    CHECK((window_sky & 255) > ((window_sky >> 16) & 255) + 100);
    CHECK((window_sky & 255) > ((window_sky >> 8) & 255) + 70);
    weather_time_ms = 0;
    held = Render(renderer, probe, NULL);
    CHECK(Differences(clear, held) == 0);
    SDL_DestroySurface(clear);
    SDL_DestroySurface(held);
    probe->sim.world_navigation_clouds = true;
    probe->sim.cloud_drift_pct = 0;
    probe->sim.world_navigation_cloud_shadows = false;
    SDL_Surface *no_shadow = Render(renderer, probe, NULL);
    CHECK(Differences(view, no_shadow) > 100);
    probe->sim.world_navigation_cloud_shadows = true;
    SDL_Surface *restored = Render(renderer, probe, NULL);
    CHECK(Differences(view, restored) == 0);
    SDL_DestroySurface(restored);
    /* Switch curvature without a resource reset. Weather UVs, model bounds,
     * cliffs and mountain joins must all return to their own chart metric. */
    UploadWorldNavigationComposition(slot);
    restored = Render(renderer, slot, NULL);
    CHECK(Differences(navigation, restored) == 0);
    SDL_DestroySurface(restored);
    restored = Render(renderer, probe, NULL);
    CHECK(Differences(view, restored) == 0);
    SDL_DestroySurface(restored);
    PresentWorldNav_ResetResources();
    restored = Render(renderer, probe, NULL);
    CHECK(Differences(view, restored) == 0);
    SDL_DestroySurface(restored);
    SDL_DestroySurface(no_shadow);
    SDL_DestroySurface(view);
  }
  UploadWorldNavigationComposition(slot);
  SDL_Surface *restored = Render(renderer, slot, NULL);
  CHECK(Differences(navigation, restored) == 0);
  SDL_DestroySurface(restored);
  SDL_DestroySurface(navigation);
  free(probe);
}

static void InitSlot(FrameSlot *slot) {
  memset(slot, 0, sizeof(*slot));
  slot->pixel_aspect = kPixelAspect_Crt43;
  slot->snes_width = slot->visible_width = kActRaiserAuthenticWidth;
  slot->snes_height = kActRaiserAuthenticHeight;
  slot->sim.view = kSimView_WorldNavigation;
  slot->sim.world_navigation_brightness = 15;
  slot->sim.world_navigation_relief = true;
  slot->sim.landscape_height_pct = slot->sim.height_scale_x100 = 100;
  slot->sim.background_voxel_enabled = true;
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  slot->sim.background_voxel_style = kSimBackgroundVoxelStyle_Varied;
  slot->sim.projection_distance_x100 = 300;
  slot->sim.light_elevation_deg = 70;
  slot->sim.light_azimuth_deg = 225;
  slot->sim.cloud_altitude_px = 96;
  slot->sim.cloud_opacity_pct = slot->sim.shadow_opacity_pct = 35;
  slot->sim.shadow_softness_pct = 50;
  slot->sim.game_frame = 400;
  slot->sim.world_navigation = (SimWorldNavigationFrame){
      .focus_x = 512, .focus_y = 512, .active_location = 2,
      .matrix = {kSimWorldNavigationZoomMiddle, 0, 0, kSimWorldNavigationZoomMiddle},
      .zoom_current = kSimWorldNavigationZoomMiddle,
      .zoom_target = kSimWorldNavigationZoomMiddle};
  BuildScene(slot);
}

static void TestGroundLightDirections(SDL_Renderer *renderer, const FrameSlot *slot,
                                      const char *prefix) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  *probe = *slot;
  probe->sim.world_navigation_models = probe->sim.world_navigation_mountains = false;
  probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = false;
  probe->sim.world_navigation_atmosphere = probe->sim.world_navigation_backdrop = false;
  probe->sim.world_navigation_lighting = false;
  SDL_Surface *unlit = Render(renderer, probe, NULL);
  const int directions[][2] = {
    {0, 45}, {90, 45}, {180, 45}, {270, 45}, {225, 0}, {225, 90},
  };
  for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); i++) {
    probe->sim.light_azimuth_deg = directions[i][0];
    probe->sim.light_elevation_deg = directions[i][1];
    probe->sim.world_navigation_lighting = false;
    SDL_Surface *disabled = Render(renderer, probe, NULL);
    CHECK(Differences(unlit, disabled) == 0);
    SDL_DestroySurface(disabled);
    probe->sim.world_navigation_lighting = true;
    char name[64];
    CHECK(snprintf(name, sizeof(name), "%s-ground-light-%zu", prefix, i) > 0);
    SDL_Surface *lit = Render(renderer, probe, name);
    CHECK(Differences(unlit, lit) > 1000);
    SDL_Surface *held = Render(renderer, probe, NULL);
    CHECK(Differences(lit, held) == 0);
    SDL_DestroySurface(held);
    PresentWorldNav_ResetResources();
    Sim3DDepthPass_Reset(&g_render_device);
    UploadWorldNavigationComposition(probe);
    SDL_Surface *reset = Render(renderer, probe, NULL);
    CHECK(Differences(lit, reset) == 0);
    SDL_DestroySurface(reset);
    SDL_DestroySurface(lit);
  }
  SDL_DestroySurface(unlit);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestOutputResizing(SDL_Renderer *renderer, const FrameSlot *slot) {
  SDL_Surface *original = Render(renderer, slot, NULL);
  const int sizes[][2] = {{1120, 840}, {1280, 720}, {640, 480}, {kWidth, kHeight}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    ResizeTestOutput(renderer, sizes[i][0], sizes[i][1]);
    SDL_Surface *resized = Render(renderer, slot, NULL);
    CHECK(resized->w == sizes[i][0] && resized->h == sizes[i][1]);
    PresentWorldNav_ResetResources();
    Sim3DDepthPass_Reset(&g_render_device);
    UploadWorldNavigationComposition(slot);
    SDL_Surface *reset = Render(renderer, slot, NULL);
    CHECK(Differences(resized, reset) == 0);
    SDL_DestroySurface(resized);
    SDL_DestroySurface(reset);
  }
  SDL_Surface *restored = Render(renderer, slot, NULL);
  CHECK(Differences(original, restored) == 0);
  SDL_DestroySurface(original);
  SDL_DestroySurface(restored);
}

static void TestColdBlackEntry(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  /* PPU-rasterized Palace/UI already carry their own master brightness.
   * This test isolates the host scene, including its clouds and atmosphere. */
  probe->sim.world_navigation_scene.composition.empty_animation = true;
  probe->sim.world_navigation_brightness = 15;
  probe->sim.world_navigation_models = probe->sim.background_voxel_enabled = true;
  probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = true;
  probe->sim.world_navigation_atmosphere = probe->sim.world_navigation_lighting = true;
  UploadWorldNavigationComposition(probe);
  SDL_Surface *full = Render(renderer, probe, NULL);
  PresentWorldNav_ResetResources();
  Sim3DDepthPass_Reset(&g_render_device);
  SimBackgroundVoxelModelCache_Reset();
  UploadWorldNavigationComposition(probe);
  probe->sim.world_navigation_brightness = 0;
  SDL_Surface *previous = Render(renderer, probe, NULL);
  CHECK(ColorCount(previous, 0xff000000) == previous->w * previous->h);
  const SimBackgroundVoxelModelCacheStats cold = SimBackgroundVoxelModelCache_Stats();
  CHECK(cold.misses > 0); /* Actual models were prepared behind black. */
  for (uint8_t level = 1; level <= 15; level++) {
    probe->sim.world_navigation_brightness = level;
    SDL_Surface *visible = Render(renderer, probe, NULL);
    const SimBackgroundVoxelModelCacheStats current = SimBackgroundVoxelModelCache_Stats();
    CHECK(current.misses == cold.misses && current.relights == cold.relights);
    CHECK(ColorCount(visible, 0xff000000) < visible->w * visible->h);
    for (int y = 0; y < visible->h; y++)
      for (int x = 0; x < visible->w; x++) {
        const uint32_t before = Pixel(previous, x, y), after = Pixel(visible, x, y);
        for (int shift = 0; shift < 24; shift += 8) {
          const unsigned value = (after >> shift) & 255;
          CHECK(value >= ((before >> shift) & 255) && value <= level * 17u);
        }
      }
    SDL_DestroySurface(previous);
    previous = visible;
  }
  CHECK(Differences(full, previous) == 0);
  SDL_DestroySurface(full);
  SDL_DestroySurface(previous);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

/* Inspect the selected detail through the shared cache, without a
 * presenter-only diagnostic API. This probe requires a cold cache and one
 * non-animated authored object. It also leaves all four detail keys warm. */
static SimBackgroundVoxelDetail RenderedSingleObjectDetail(const FrameSlot *slot) {
  CHECK(slot->sim.world_navigation_towns.object_count == 1);
  CHECK(SimBackgroundVoxelModelCache_Stats().misses == 1);
  int selected = -1;
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    const SimBackgroundVoxelModelCacheStats before = SimBackgroundVoxelModelCache_Stats();
    const SimBackgroundVoxelModelView *model = SimBackgroundVoxelModelCache_Get(
        &slot->sim.world_navigation_towns.objects[0], (SimBackgroundVoxelDetail)detail,
        (SimBackgroundVoxelStyle)slot->sim.background_voxel_style, NULL, NULL);
    CHECK(model && !model->overflow && model->face_count);
    if (SimBackgroundVoxelModelCache_Stats().hits > before.hits) {
      CHECK(selected == -1);
      selected = detail;
    }
  }
  CHECK(selected >= 0);
  return (SimBackgroundVoxelDetail)selected;
}

static void TestTownLodMotion(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  InitSlot(probe);
  probe->sim.world_navigation_models = true;
  probe->sim.world_navigation_towns.object_count = 1;
  probe->sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
      .town = 2, .kind = kSimBackgroundVoxel_Factory, .cell_x = 15, .cell_y = 15,
      .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished};
  probe->sim.world_navigation.zoom_current = probe->sim.world_navigation.zoom_target =
      kSimWorldNavigationZoomNear;
  probe->sim.world_navigation.matrix[0] = probe->sim.world_navigation.matrix[3] =
      kSimWorldNavigationZoomNear;
  probe->sim.projection_pitch_mrad = -575;
  probe->sim.projection_distance_x100 = 200;
  BuildScene(probe);
  const SimWorldNavigationTowns towns = probe->sim.world_navigation_towns;
  const SimWorldNavigationFrame navigation = probe->sim.world_navigation;
  const uint32_t serial = SimWorldMap_Serial();

  /* The ordinary output already uses the native Low factory. Larger
   * outputs must recover the finer models without changing world scale. */
  SimBackgroundVoxelModelCache_Reset();
  SDL_Surface *small = Render(renderer, probe, NULL);
  CHECK(RenderedSingleObjectDetail(probe) == kSimBackgroundVoxelDetail_Low);
  SDL_DestroySurface(small);
  ResizeTestOutput(renderer, 2688, 2016);
  SimBackgroundVoxelModelCache_Reset();
  SDL_Surface *large = Render(renderer, probe, NULL);
  CHECK(RenderedSingleObjectDetail(probe) == kSimBackgroundVoxelDetail_Ultra);
  SDL_DestroySurface(large);

  ResizeTestOutput(renderer, 1792, 1344);
  /* Include adjacent camera ticks across both measured thresholds. These
   * are discrete authored models, not a geometry-morphing promise. */
  const int distances[] = {200, 205, 206, 409, 410, 600};
  SDL_Surface *frames[sizeof(distances) / sizeof(distances[0])] = {0};
  int previous_detail = kSimBackgroundVoxelDetail_Ultra;
  int transitions = 0;
  for (size_t i = 0; i < sizeof(distances) / sizeof(distances[0]); i++) {
    probe->sim.projection_distance_x100 = distances[i];
    SimBackgroundVoxelModelCache_Reset();
    frames[i] = Render(renderer, probe, NULL);
    const SimBackgroundVoxelDetail detail = RenderedSingleObjectDetail(probe);
    CHECK((int)detail <= previous_detail);
    if (i && (int)detail != previous_detail) transitions++;
    previous_detail = detail;
    /* The user ceiling must produce the same model when it matches the
     * chosen tier, and remove actual detail when lowered further. */
    probe->sim.background_voxel_detail = detail;
    SDL_Surface *capped = Render(renderer, probe, NULL);
    CHECK(Differences(frames[i], capped) == 0);
    SDL_DestroySurface(capped);
    if (detail > kSimBackgroundVoxelDetail_Low) {
      probe->sim.background_voxel_detail = detail - 1;
      capped = Render(renderer, probe, NULL);
      CHECK(Differences(frames[i], capped) > 0);
      SDL_DestroySurface(capped);
    }
    probe->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  }
  CHECK(transitions == 2 && previous_detail == kSimBackgroundVoxelDetail_Low);

  const SimBackgroundVoxelModelCacheStats warm = SimBackgroundVoxelModelCache_Stats();
  for (size_t i = sizeof(distances) / sizeof(distances[0]); i-- > 0;) {
    probe->sim.projection_distance_x100 = distances[i];
    for (unsigned frame = 0; frame < 3; frame++) {
      probe->sim.game_frame++;
      const SimBackgroundVoxelModelCacheStats before_frame = SimBackgroundVoxelModelCache_Stats();
      SDL_Surface *returned = Render(renderer, probe, NULL);
      CHECK(Differences(frames[i], returned) == 0);
      if (frame == 2)
        CHECK(SimBackgroundVoxelModelCache_Stats().hits == before_frame.hits);
      SDL_DestroySurface(returned);
    }
    SDL_DestroySurface(frames[i]);
  }
  const SimBackgroundVoxelModelCacheStats after = SimBackgroundVoxelModelCache_Stats();
  CHECK(after.misses == warm.misses && after.relights == warm.relights);
  /* Each camera change draws once, its first repeat warms projected faces,
   * and the third frame replays them without a compiler-cache lookup. */
  CHECK(after.hits == warm.hits + 2 * sizeof(distances) / sizeof(distances[0]));
  CHECK(!memcmp(&towns, &probe->sim.world_navigation_towns, sizeof(towns)));
  CHECK(!memcmp(&navigation, &probe->sim.world_navigation, sizeof(navigation)));
  CHECK(SimWorldMap_Serial() == serial);
  ResizeTestOutput(renderer, kWidth, kHeight);
  SimBackgroundVoxelModelCache_Reset();
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestAnimatedTownCache(SDL_Renderer *renderer, const FrameSlot *slot, bool dense) {
  ResizeTestOutput(renderer, 1792, 1344);
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  InitSlot(probe);
  probe->sim.world_navigation_models = true;
  probe->sim.world_navigation_towns.object_count = dense ? 144 : 5;
  for (int i = 0; i < probe->sim.world_navigation_towns.object_count; ++i)
    probe->sim.world_navigation_towns.objects[i] = (SimBackgroundVoxelObject){
      .town = 2, .kind = i & 1 ? kSimBackgroundVoxel_Factory : kSimBackgroundVoxel_Windmill,
      .cell_x = dense ? (i % 12) * 2 + 4 : 11 + i * 2,
      .cell_y = dense ? (i / 12) * 2 + 4 : 15,
      .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished,
    };
  probe->sim.world_navigation.zoom_current = probe->sim.world_navigation.zoom_target =
      kSimWorldNavigationZoomNear;
  probe->sim.world_navigation.matrix[0] = probe->sim.world_navigation.matrix[3] =
      kSimWorldNavigationZoomNear;
  probe->sim.projection_distance_x100 = 200;
  if (dense) {
    probe->sim.projection_distance_x100 = 500;
    probe->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
    probe->sim.world_navigation_lighting = true;
    probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = true;
  }
  BuildScene(probe);
  SDL_Surface *reference[3];
  for (int phase = 0; phase < 3; ++phase) {
    PresentWorldNav_ResetResources();
    UploadWorldNavigationComposition(probe);
    probe->sim.game_frame = (uint16_t)(phase * 12);
    reference[phase] = Render(renderer, probe, NULL);
  }
  CHECK(Differences(reference[0], reference[1]) > 0);
  CHECK(Differences(reference[1], reference[2]) > 0);
  const char *incoming = SDL_getenv("AR_SIM3D_RETAINED_SOLIDS");
  char *saved = incoming ? SDL_strdup(incoming) : NULL;
  CHECK(!incoming || saved);
  const char *incoming_ground = SDL_getenv("AR_SIM3D_RETAINED_GROUND");
  char *saved_ground = incoming_ground ? SDL_strdup(incoming_ground) : NULL;
  CHECK(!incoming_ground || saved_ground);
  PerformanceSnapshot measured[5];
  for (int retained = 0; retained < 5; ++retained) {
    CHECK(SDL_setenv_unsafe("AR_SIM3D_RETAINED_SOLIDS", retained ? "1" : "0", 1) == 0);
    CHECK(SDL_setenv_unsafe("AR_SIM3D_RETAINED_GROUND", retained >= 2 ? "1" : "0", 1) == 0);
    if (retained == 4) {
      CHECK(SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_SOLIDS") == 0);
      CHECK(SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_GROUND") == 0);
    }
    PresentWorldNav_ResetResources();
    UploadWorldNavigationComposition(probe);
    Sim3DDepthMesh *pressure[128] = {0};
    unsigned pressure_count = 0;
    if (retained == 3) {
      CHECK(Sim3DDepthPass_Begin(&g_render_device, 1792, 1344, kArRenderFilter_Nearest));
      while (pressure_count < 128 && (pressure[pressure_count] = Sim3DDepthPass_CreateGeometryMesh()))
        ++pressure_count;
      CHECK(pressure_count >= 40 && pressure_count < 128);
      CHECK(!Sim3DDepthPass_CreateGeometryMesh());
    }
    const int phases[] = {0, 0, 0, 1, 2, 1, 0, 2};
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
      if (i == 3) PerformanceMetrics_Configure(true, false);
      probe->sim.game_frame = (uint16_t)(phases[i] * 12);
      const SimBackgroundVoxelModelCacheStats before = SimBackgroundVoxelModelCache_Stats();
      SDL_Surface *image = Render(renderer, probe, NULL);
      CHECK(Differences(image, reference[phases[i]]) == 0);
      SDL_DestroySurface(image);
      if (i == 3 || i == 4) PerformanceMetrics_PresentCompleted(1 + (i - 3) * UINT64_C(1000000000));
      if (i == 4) {
        PerformanceMetrics_Snapshot(&measured[retained]);
        PerformanceMetrics_Configure(false, false);
      }
      if (i >= 2) {
        /* Both static factories stay cached across forward/backward phase
         * changes. Animated models retain their interleaved submission order. */
        const uint64_t hits = SimBackgroundVoxelModelCache_Stats().hits - before.hits;
        CHECK(dense ? hits == 72 : hits == 3);
      }
    }
    CHECK(measured[retained].ready);
    for (unsigned i = 0; i < pressure_count; ++i) Sim3DDepthPass_DestroyMesh(pressure[i]);
  }
  CHECK(dense ? measured[1].counts[kPerformanceCount_DepthUploadBytes] ==
      measured[0].counts[kPerformanceCount_DepthUploadBytes]
      : measured[1].counts[kPerformanceCount_DepthUploadBytes] <
      measured[0].counts[kPerformanceCount_DepthUploadBytes]);
  if (dense) CHECK(measured[1].counts[kPerformanceCount_Draws] == measured[0].counts[kPerformanceCount_Draws]);
  CHECK(measured[1].counts[kPerformanceCount_Vertices] == measured[0].counts[kPerformanceCount_Vertices]);
  CHECK(measured[2].counts[kPerformanceCount_DepthUploadBytes] <
      measured[1].counts[kPerformanceCount_DepthUploadBytes]);
  CHECK(measured[2].counts[kPerformanceCount_Vertices] == measured[1].counts[kPerformanceCount_Vertices]);
  CHECK(measured[2].counts[kPerformanceCount_Draws] == measured[1].counts[kPerformanceCount_Draws]);
  CHECK(measured[3].counts[kPerformanceCount_DepthUploadBytes] == measured[0].counts[kPerformanceCount_DepthUploadBytes]);
  CHECK(measured[3].counts[kPerformanceCount_Draws] == measured[0].counts[kPerformanceCount_Draws]);
  CHECK(measured[3].counts[kPerformanceCount_Vertices] == measured[0].counts[kPerformanceCount_Vertices]);
  CHECK(measured[4].counts[kPerformanceCount_DepthUploadBytes] == measured[2].counts[kPerformanceCount_DepthUploadBytes]);
  CHECK(measured[4].counts[kPerformanceCount_Draws] == measured[2].counts[kPerformanceCount_Draws]);
  CHECK(measured[4].counts[kPerformanceCount_Vertices] == measured[2].counts[kPerformanceCount_Vertices]);
  printf("ordered retained factories (dense=%d): upload %.0f -> %.0f bytes/present; unchanged vertices %.0f\n",
      (int)dense,
      measured[0].counts[kPerformanceCount_DepthUploadBytes],
      measured[1].counts[kPerformanceCount_DepthUploadBytes],
      measured[1].counts[kPerformanceCount_Vertices]);
  printf("retained ground: upload %.0f -> %.0f bytes/present; unchanged draws %.0f\n",
      measured[1].counts[kPerformanceCount_DepthUploadBytes],
      measured[2].counts[kPerformanceCount_DepthUploadBytes],
      measured[2].counts[kPerformanceCount_Draws]);
  if (saved) CHECK(SDL_setenv_unsafe("AR_SIM3D_RETAINED_SOLIDS", saved, 1) == 0);
  else CHECK(SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_SOLIDS") == 0);
  SDL_free(saved);
  if (saved_ground) CHECK(SDL_setenv_unsafe("AR_SIM3D_RETAINED_GROUND", saved_ground, 1) == 0);
  else CHECK(SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_GROUND") == 0);
  SDL_free(saved_ground);
  for (int phase = 0; phase < 3; ++phase) SDL_DestroySurface(reference[phase]);
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(slot);
  ResizeTestOutput(renderer, kWidth, kHeight);
  free(probe);
}

static void TestRadialTownResidency(SDL_Renderer *renderer, const FrameSlot *slot) {
  const char *incoming = SDL_getenv("AR_SIM3D_WORLD_GPU_MODELS");
  char *saved = incoming ? SDL_strdup(incoming) : NULL;
  CHECK(!incoming || saved);
  /* Exercise the shipping default, not an opt-in-only shader path. */
  CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_MODELS"));
  ResizeTestOutput(renderer, 1792, 1344);
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  InitSlot(probe);
  probe->sim.world_navigation_models = probe->sim.world_navigation_lighting = true;
  probe->sim.world_navigation_towns.object_count = 144;
  for (int i = 0; i < 144; ++i) probe->sim.world_navigation_towns.objects[i] = (SimBackgroundVoxelObject){
    .town = 2, .kind = i & 1 ? kSimBackgroundVoxel_Factory : kSimBackgroundVoxel_Windmill,
    .cell_x = (i % 12) * 2 + 4, .cell_y = (i / 12) * 2 + 4,
    .source_cells_w = 2, .source_cells_h = 2, .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  probe->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
  probe->sim.projection_distance_x100 = 500;
  const int phases[] = {0,0,0,1,2,1,0,2};
  for (unsigned view = 0; view < 2; ++view) {
    probe->sim.view = view ? kSimView_SkyPalace : kSimView_WorldNavigation;
    BuildScene(probe);
    SDL_Surface *reference[3];
    for (unsigned phase = 0; phase < 3; ++phase) {
      PresentWorldNav_ResetResources();
      UploadWorldNavigationComposition(probe);
      probe->sim.game_frame = (uint16_t)(phase * 12);
      reference[phase] = Render(renderer, probe, NULL);
    }
    CHECK(Differences(reference[0], reference[1]) > 0);
    CHECK(Differences(reference[1], reference[2]) > 0);
    for (unsigned i = 0; i < sizeof(phases) / sizeof(*phases); ++i) {
      if (i == 3) PerformanceMetrics_Configure(true, false);
      probe->sim.game_frame = (uint16_t)(phases[i] * 12);
      const SimBackgroundVoxelModelCacheStats before = SimBackgroundVoxelModelCache_Stats();
      SDL_Surface *actual = Render(renderer, probe, NULL);
      CHECK(!Differences(actual, reference[phases[i]]));
      SDL_DestroySurface(actual);
      CHECK(before.hits == SimBackgroundVoxelModelCache_Stats().hits);
      CHECK(before.misses == SimBackgroundVoxelModelCache_Stats().misses);
      if (i == 3 || i == 4) PerformanceMetrics_PresentCompleted(1 + (i - 3) * UINT64_C(1000000000));
      if (i == 4) {
        PerformanceSnapshot measured;
        PerformanceMetrics_Snapshot(&measured);
        PerformanceMetrics_Configure(false, false);
        CHECK(measured.ready && measured.counts[kPerformanceCount_GpuReuse] > 0);
        CHECK(measured.counts[kPerformanceCount_GeometryRejected] == 0);
        CHECK(measured.counts[kPerformanceCount_GeometryLimit] == 0);
        CHECK(measured.counts[kPerformanceCount_GeometryPublish] == 0);
        CHECK(measured.counts[kPerformanceCount_Draws] <= 15); /* No per-windmill draws. */
      }
    }
    for (unsigned p = 0; p < 3; ++p) SDL_DestroySurface(reference[p]);
  }
  PresentWorldNav_ResetResources();
  if (saved) CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_MODELS", saved, 1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_MODELS"));
  SDL_free(saved);
  UploadWorldNavigationComposition(slot);
  ResizeTestOutput(renderer, kWidth, kHeight);
  free(probe);
}

static void TestGroundCacheRevisions(SDL_Renderer *renderer, const FrameSlot *slot) {
  enum { kStates = 12 };
  FrameSlot *probe = malloc(sizeof(*probe));
  uint8_t *map = malloc(kSimWorldMapBytes);
  CHECK(probe && map);
  const char *incoming = SDL_getenv("AR_SIM3D_RETAINED_GROUND");
  char *saved = incoming ? SDL_strdup(incoming) : NULL;
  CHECK(!incoming || saved);
  SDL_Surface *reference[kStates] = {0};
  for (int retained = 0; retained < 2; ++retained) {
    CHECK(SDL_setenv_unsafe("AR_SIM3D_RETAINED_GROUND", retained ? "1" : "0", 1) == 0);
    PresentWorldNav_ResetResources();
    InitSlot(probe);
    memcpy(map, SimWorldMap_Baseline(), kSimWorldMapBytes);
    SimWorldMap_PublishBuiltTilemap(map);
    for (int state = 0; state < kStates; ++state) {
      switch (state) {
        case 1: probe->sim.world_navigation_lighting = true; break;
        case 2: probe->sim.light_azimuth_deg = 45; break;
        case 3: probe->sim.landscape_height_pct = 300; break;
        case 4: probe->sim.world_navigation_ground_detail = true; break;
        case 5: probe->sim_manual_orbit_yaw = .4f; break;
        case 6: probe->sim.view = kSimView_SkyPalace; break;
        case 7:
          probe->sim.view = kSimView_WorldNavigation;
          probe->sim_manual_orbit_yaw = 0;
          break;
        case 8:
          /* Replace central synthetic land with ocean. Geography, not just
           * texture content, must invalidate retained opacity and relief. */
          for (int y = 60; y < 68; ++y) memset(map + y * 128 + 60, 0, 8);
          CHECK(SimWorldMap_PublishBuiltTilemap(map) > 0);
          break;
        case 9:
          memcpy(map, SimWorldMap_Baseline(), kSimWorldMapBytes);
          CHECK(SimWorldMap_PublishBuiltTilemap(map) > 0);
          break;
        case 10: probe->sim.world_navigation_relief = false; break;
        case 11: InitSlot(probe); break;
      }
      BuildScene(probe);
      UploadWorldNavigationComposition(probe);
      /* Do not reset between changes. Compare cold publication, first
       * repetition and warm reuse against the ordinary reference path. */
      for (int frame = 0; frame < 4; ++frame) {
        SDL_Surface *actual = Render(renderer, probe, NULL);
        if (!retained && !frame) reference[state] = actual;
        else {
          CHECK(Differences(actual, reference[state]) == 0);
          SDL_DestroySurface(actual);
        }
      }
    }
  }
  CHECK(Differences(reference[0], reference[11]) == 0);
  CHECK(Differences(reference[7], reference[8]) > 100);
  CHECK(Differences(reference[7], reference[9]) == 0);
  for (int state = 0; state < kStates; ++state) SDL_DestroySurface(reference[state]);
  if (saved) CHECK(SDL_setenv_unsafe("AR_SIM3D_RETAINED_GROUND", saved, 1) == 0);
  else CHECK(SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_GROUND") == 0);
  SDL_free(saved);
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(slot);
  free(map); free(probe);
}

static void InitSyntheticBloodpoolArt(bool animated) {
  uint8_t *rom = calloc(kRomBytes, 1);
  CHECK(rom);
  rom[0x1098D] = 0x24; rom[0x1098E] = 8;
  rom[0xE3B95] = 31;
  for (int bank = 0; bank < 2; ++bank) for (int y = 0; y < 8; ++y) {
    rom[0x60000 + bank * 0x4000 + 32 + y * 2] = 255;
    rom[0x60000 + bank * 0x4000 + 0x200 + 32 + y * 2] = 255;
  }
  if (animated) for (int bank = 0; bank < 2; ++bank)
    for (unsigned phase = 0; phase < 4; ++phase) for (unsigned y = 0; y < 8; ++y)
      rom[0x60000 + bank * 0x4000 + phase * 0x100 + y * 2] =
          (uint8_t)(0xffu << (phase + y % 2));
  CHECK(SimTownGroundArt_Init(rom, kRomBytes));
  free(rom);
}

static void TestRetainedMountainSurfaces(SDL_Renderer *renderer, const FrameSlot *slot) {
  enum { kStates = 11 };
  /* Synthetic Bloodpool rock: same public ROM decoder/town inputs as the
   * portable mountain-restoration fixture, no copyrighted pixels. */
  static const uint32_t rows[32] = {
    0xFFFFFFFFu, 0xFFFC3C3Fu, 0xFFF0000Fu, 0xFFC00003u, 0xF0000000u,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x00000600u, 0x00000F00u, 0x00006F00u, 0x0000FF80u, 0x0000FFE0u, 0x0000FFF8u,
  };
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  InitSyntheticBloodpoolArt(false);
  const char *incoming = SDL_getenv("AR_SIM3D_RETAINED_GROUND");
  char *saved = incoming ? SDL_strdup(incoming) : NULL;
  CHECK(!incoming || saved);
  SDL_Surface *reference[kStates] = {0};
  PerformanceSnapshot measured[3];
  for (int mode = 0; mode < 3; ++mode) {
    if (!mode) CHECK(!SDL_setenv_unsafe("AR_SIM3D_RETAINED_GROUND", "0", 1));
    else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_GROUND")); /* Shipping default. */
    PresentWorldNav_ResetResources();
    ResizeTestOutput(renderer, kWidth, kHeight);
    InitSlot(probe);
    probe->sim.world_navigation_mountains = true;
    probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = true;
    probe->sim.cloud_drift_pct = 100;
    SimWorldNavigationTownGround *ground = &probe->sim.world_navigation_towns.ground;
    ground->enabled_town_mask = 2;
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
      ground->terrain[1][y * 32 + x] = rows[y] & (1u << x) ? 0x89 : 8;
    int ox, oy;
    CHECK(SimWorldMap_OriginForTown(2, &ox, &oy));
    probe->sim.world_navigation.focus_x = (ox + 16) * kSimWorldMapTilePixels;
    probe->sim.world_navigation.focus_y = (oy + 16) * kSimWorldMapTilePixels;
    probe->sim.world_navigation.active_location = 2;
    Sim3DDepthMesh *pressure[128] = {0};
    unsigned pressure_count = 0;
    if (mode == 2) {
      CHECK(Sim3DDepthPass_Begin(&g_render_device, kWidth, kHeight, kArRenderFilter_Nearest));
      while (pressure_count < 128 && (pressure[pressure_count] = Sim3DDepthPass_CreateGeometryMesh()))
        ++pressure_count;
      CHECK(pressure_count >= 40 && pressure_count < 128);
      CHECK(!Sim3DDepthPass_CreateGeometryMesh());
    }
    for (int state = 0; state < kStates; ++state) {
      switch (state) {
        case 1: probe->sim.world_navigation_lighting = true; break;
        case 2: weather_time_ms = 1500; break; /* Wind must not invalidate opaque source. */
        case 3: probe->sim.landscape_height_pct = 250; break;
        case 4: probe->sim.world_navigation_mountains = false; break;
        case 5: probe->sim.world_navigation_mountains = true; break;
        case 6: probe->sim_manual_orbit_yaw = .5f; break;
        case 7: probe->sim.view = kSimView_SkyPalace; break;
        case 8: ground->terrain[1][27 * 32 + 8] = 8; break;
        case 9: ground->terrain[1][27 * 32 + 8] = 0x89; break;
        case 10: ResizeTestOutput(renderer, 960, 540); break;
      }
      BuildScene(probe);
      UploadWorldNavigationComposition(probe);
      for (int frame = 0; frame < 4; ++frame) {
        if (!state && frame == 2) PerformanceMetrics_Configure(true, false);
        SDL_Surface *actual = Render(renderer, probe, NULL);
        if (!mode && !frame) reference[state] = actual;
        else { CHECK(Differences(actual, reference[state]) == 0); SDL_DestroySurface(actual); }
        if (!state && frame >= 2) PerformanceMetrics_PresentCompleted(
            1 + (frame - 2) * UINT64_C(1000000000));
      }
      if (!state) {
        PerformanceMetrics_Snapshot(&measured[mode]);
        PerformanceMetrics_Configure(false, false);
        CHECK(measured[mode].ready);
      }
    }
    for (unsigned i = 0; i < pressure_count; ++i) Sim3DDepthPass_DestroyMesh(pressure[i]);
    weather_time_ms = 0;
  }
  CHECK(Differences(reference[3], reference[4]) > 100);
  CHECK(Differences(reference[1], reference[2]) > 100);
  CHECK(Differences(reference[7], reference[8]) > 0);
  CHECK(Differences(reference[3], reference[5]) == 0);
  CHECK(Differences(reference[7], reference[9]) == 0);
  CHECK(measured[1].counts[kPerformanceCount_DepthUploadBytes] <
      measured[0].counts[kPerformanceCount_DepthUploadBytes]);
  for (int mode = 1; mode < 3; ++mode) {
    CHECK(measured[mode].counts[kPerformanceCount_Draws] == measured[0].counts[kPerformanceCount_Draws]);
    CHECK(measured[mode].counts[kPerformanceCount_Vertices] == measured[0].counts[kPerformanceCount_Vertices]);
  }
  CHECK(measured[2].counts[kPerformanceCount_DepthUploadBytes] ==
      measured[0].counts[kPerformanceCount_DepthUploadBytes]);
  printf("retained ground/mountains: upload %.0f -> %.0f; unchanged draws %.0f; exact cold/warm/pressure images\n",
      measured[0].counts[kPerformanceCount_DepthUploadBytes],
      measured[1].counts[kPerformanceCount_DepthUploadBytes], measured[1].counts[kPerformanceCount_Draws]);
  for (int state = 0; state < kStates; ++state) SDL_DestroySurface(reference[state]);
  if (saved) CHECK(!SDL_setenv_unsafe("AR_SIM3D_RETAINED_GROUND", saved, 1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_RETAINED_GROUND"));
  SDL_free(saved);
  PresentWorldNav_ResetResources();
  SimTownGroundArt_Shutdown();
  ResizeTestOutput(renderer, kWidth, kHeight);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestAdventClearance(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  /* Exact camera samples from the ordinary Aitos Act 2 Advent replay. The
   * old full-sphere projection failed at gf1301, before the native fade. */
  const struct { uint16_t frame, zoom; int16_t matrix[4]; uint8_t brightness; } samples[] = {
    {1200, 490, {243, -8, 7, 243}, 15},
    {1250, 290, {52, 134, -135, 52}, 15},
    {1290, 130, {-38, 52, -53, -38}, 15},
    {1300, 90, {-35, 29, -30, -35}, 15},
    {1310, 50, {-23, 11, -12, -23}, 10},
    {1315, 30, {-15, 5, -6, -15}, 5},
    {1320, 10, {-5, 1, -2, -5}, 0},
  };
  const struct { uint16_t x, y; } targets[] = {
    {200, 352}, /* Aitos volcano, actual Advent destination. */
    {768, 512}, /* Fillmore hills. */
    {640, 896}, /* Marahna's raised plateau/cliffs. */
  };
  const uint16_t heights[] = {0, 100, 400};
  probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
  probe->sim.projection_pitch_mrad = -575;
  probe->sim.projection_yaw_mrad = 0;
  probe->sim.projection_distance_x100 = 200;
  probe->sim.world_navigation_atmosphere = true;
  probe->sim.world_navigation_relief = true;
  probe->sim.world_navigation_mountains = true;
  probe->sim.world_navigation_models = true;
  probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = true;
  probe->sim.cloud_altitude_px = 256;
  const SimWorldNavigationTowns towns = probe->sim.world_navigation_towns;
  const uint32_t serial = SimWorldMap_Serial();
  for (size_t target = 0; target < sizeof(targets) / sizeof(targets[0]); target++) {
    probe->sim.world_navigation.focus_x = targets[target].x;
    probe->sim.world_navigation.focus_y = targets[target].y;
    for (size_t height = 0; height < sizeof(heights) / sizeof(heights[0]); height++) {
      probe->sim.landscape_height_pct = heights[height];
      for (size_t sample = 0; sample < sizeof(samples) / sizeof(samples[0]); sample++) {
        probe->sim.game_frame = samples[sample].frame;
        probe->sim.world_navigation.zoom_current = samples[sample].zoom;
        memcpy(probe->sim.world_navigation.matrix, samples[sample].matrix, sizeof(samples[sample].matrix));
        probe->sim.world_navigation_brightness = samples[sample].brightness;
        BuildScene(probe); /* Empty OAM owns the original Advent composition. */
        SDL_Surface *frame = Render(renderer, probe, NULL);
        if (!samples[sample].brightness)
          CHECK(ColorCount(frame, 0xFF000000u) == frame->w * frame->h);
        else CHECK(ColorCount(frame, 0xFF000000u) < frame->w * frame->h);
        SDL_Surface *held = Render(renderer, probe, NULL);
        CHECK(Differences(frame, held) == 0);
        SDL_DestroySurface(frame); SDL_DestroySurface(held);
      }
    }
  }
  CHECK(!memcmp(&towns, &probe->sim.world_navigation_towns, sizeof(towns)));
  CHECK(SimWorldMap_Serial() == serial);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestTallModelViewport(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.view = kSimView_SkyPalace;
  probe->sim.world_navigation_models = true;
  probe->sim.world_navigation_relief = probe->sim.world_navigation_mountains = false;
  probe->sim.world_navigation_ground_detail = probe->sim.world_navigation_atmosphere = false;
  probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = false;
  probe->sim.world_navigation_lighting = probe->sim.world_navigation_backdrop = false;
  probe->sim.cloud_altitude_px = 0;
  probe->sim.height_scale_x100 = 400;
  probe->sim_manual_orbit_pitch = probe->sim_manual_orbit_yaw = 0;
  probe->sim.world_navigation.focus_x = 512;
  probe->sim.world_navigation.focus_y = 136;
  probe->sim.world_navigation_towns.object_count = 1;
  probe->sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
    .town = 2, .kind = kSimBackgroundVoxel_BloodpoolCastle,
    .cell_x = 15, .cell_y = 15,
    .source_cells_w = 2, .source_cells_h = 2,
    .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  BuildScene(probe);
  probe->sim.world_navigation_scene.active_region_valid = false;
  SDL_Surface *visible = Render(renderer, probe, "synthetic-tall-viewport");
  SDL_Surface *held = Render(renderer, probe, NULL);
  CHECK(Differences(visible, held) == 0);
  SDL_DestroySurface(held);
  /* Keep models enabled so the same global safety envelope owns both
   * images. A zero-width footprint preserves that bound but cannot reach the
   * draw list. Ground detail is off in both images. */
  probe->sim.world_navigation_towns.objects[0].footprint_cells_w = 0;
  SDL_Surface *ground = Render(renderer, probe, "synthetic-tall-viewport-ground");
  CHECK(Differences(visible, ground) > 100);
  SDL_DestroySurface(ground);
  probe->sim.world_navigation_towns.objects[0].footprint_cells_w = 2;
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(probe);
  SDL_Surface *restored = Render(renderer, probe, NULL);
  CHECK(Differences(visible, restored) == 0);
  SDL_DestroySurface(restored);
  SDL_DestroySurface(visible);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestGpuGridRevisions(SDL_Renderer *renderer, const FrameSlot *slot) {
  enum { kStates = 21 };
  const char *incoming = SDL_getenv("AR_SIM3D_WORLD_GPU_GRID");
  char *saved = incoming ? SDL_strdup(incoming) : NULL;
  CHECK(!incoming || saved);
  const char *incoming_cull = SDL_getenv("AR_SIM3D_WORLD_GPU_GRID_CULL");
  char *saved_cull = incoming_cull ? SDL_strdup(incoming_cull) : NULL;
  CHECK(!incoming_cull || saved_cull);
  const char *incoming_workers = SDL_getenv("AR_RENDER_WORKERS");
  char *saved_workers = incoming_workers ? SDL_strdup(incoming_workers) : NULL;
  CHECK(!incoming_workers || saved_workers);
  InitSyntheticBloodpoolArt(false);
  SimWorldMap_SetWaterAnimationSource(kWorldWaterSourceFirst);
  FrameSlot *probe = malloc(sizeof(*probe));
  uint8_t *map = malloc(kSimWorldMapBytes);
  CHECK(probe && map);
  SDL_Surface *reference[kStates] = {0};
  bool compacted = false;
  for (unsigned warm = 0; warm < 3; ++warm) {
    CHECK(!SDL_setenv_unsafe("AR_RENDER_WORKERS",warm ? "3" : "0",1));
    /* Default and explicitly enabled source must draw identically. */
    if (warm) CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID"));
    else CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID","1",1));
    if (warm == 1) CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID_CULL")); /* Default culls. */
    else CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID_CULL",warm ? "1" : "0",1));
    PresentWorldNav_ResetResources();
    InitSlot(probe);
    probe->sim.world_navigation_towns.ground.enabled_town_mask = 2;
    memset(probe->sim.world_navigation_towns.ground.terrain[1],8,kSimTownCells*kSimTownCells);
    /* Exercise actual native cutouts, including source removal/restoration. */
    probe->sim.world_navigation_mountains = true;
    probe->sim.world_navigation_towns.ground.terrain[1][6*32+15] = 0x89;
    probe->sim.world_navigation_lighting = true;
    probe->sim.world_navigation_clouds = probe->sim.world_navigation_cloud_shadows = true;
    probe->sim.world_navigation_haze = true;
    probe->sim.underlay_defocus_pct = probe->sim.underlay_haze_pct = 40;
    probe->sim.cull_haze_lead_px = 128;
    memcpy(map,SimWorldMap_Baseline(),kSimWorldMapBytes);
    SimWorldMap_PublishBuiltTilemap(map);
    for (unsigned state = 0; state < kStates; ++state) {
      switch (state) {
        case 1: probe->sim.light_azimuth_deg = 45; break;
        case 2: probe->sim.landscape_height_pct = 300; break;
        case 3: probe->sim.world_navigation_ground_detail = true; break;
        case 4: probe->sim_manual_orbit_yaw = .4f; break;
        case 5: probe->sim.view = kSimView_SkyPalace; break;
        case 6: probe->sim.world_navigation_cloud_shadows = false; break;
        case 7: probe->sim.world_navigation_haze = false; break;
        case 8: probe->sim.world_navigation_relief = false; break;
        case 9:
          for (int y = 60; y < 68; ++y) memset(map+y*128+60,0,8);
          CHECK(SimWorldMap_PublishBuiltTilemap(map) > 0); break;
        case 10:
          memcpy(map,SimWorldMap_Baseline(),kSimWorldMapBytes);
          CHECK(SimWorldMap_PublishBuiltTilemap(map) > 0); break;
        case 11:
          probe->sim.world_navigation_cloud_shadows = probe->sim.world_navigation_haze = true;
          probe->sim.world_navigation_relief = true; break;
        case 12:
          probe->sim.view = kSimView_WorldNavigation;
          probe->sim.world_navigation.zoom_current = 50;
          probe->sim.world_navigation.matrix[0] = probe->sim.world_navigation.matrix[3] = 50;
          break;
        case 13: ResizeTestOutput(renderer,960,540); break;
        case 14: ResizeTestOutput(renderer,kWidth,kHeight); break;
        case 15: probe->sim.world_navigation_lighting = false; break;
        case 16: probe->sim.world_navigation_lighting = true; break;
        case 17: probe->sim.world_navigation_mountains = false; break;
        case 18: probe->sim.world_navigation_mountains = true; break;
        case 19: probe->sim.world_navigation_towns.ground.terrain[1][6*32+15] = 8; break;
        case 20: probe->sim.world_navigation_towns.ground.terrain[1][6*32+15] = 0x89; break;
      }
      if (warm != 2) PresentWorldNav_ResetResources();
      BuildScene(probe); UploadWorldNavigationComposition(probe);
      PerformanceMetrics_Configure(true,false);
      for (unsigned frame = 0; frame < 3; ++frame) {
        SDL_Surface *actual = Render(renderer,probe,NULL);
        if (!warm && !frame) reference[state] = actual;
        else { CHECK(Differences(actual,reference[state]) == 0); SDL_DestroySurface(actual); }
        PerformanceMetrics_PresentCompleted(1 + frame*UINT64_C(1000000000));
        PerformanceSnapshot sample; PerformanceMetrics_Snapshot(&sample);
        compacted |= sample.counts[kPerformanceCount_DepthCopyCalls] > 0;
      }
      PerformanceSnapshot measured; PerformanceMetrics_Snapshot(&measured);
      CHECK(measured.ready && measured.counts[kPerformanceCount_GpuReuse] > 0);
      CHECK(measured.counts[kPerformanceCount_GeometryRejected] == 0);
      CHECK(measured.stages[kPerformance_SimFirst+kSim3DPerformance_WorldOcean].calls == 0);
      CHECK(measured.stages[kPerformance_SimFirst+kSim3DPerformance_DepthMountain].calls == 0);
      if (!warm) CHECK(measured.counts[kPerformanceCount_DepthCopyCalls] == 0);
      PerformanceMetrics_Configure(false,false);
    }
  }
  CHECK(compacted);
  for (unsigned state = 0; state < kStates; ++state) SDL_DestroySurface(reference[state]);
  if (saved) CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID",saved,1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID"));
  SDL_free(saved); free(map); free(probe);
  if (saved_cull) CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID_CULL",saved_cull,1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID_CULL"));
  SDL_free(saved_cull);
  if (saved_workers) CHECK(!SDL_setenv_unsafe("AR_RENDER_WORKERS",saved_workers,1));
  else CHECK(!SDL_unsetenv_unsafe("AR_RENDER_WORKERS"));
  SDL_free(saved_workers);
  SimTownGroundArt_Shutdown();
  PresentWorldNav_ResetResources(); UploadWorldNavigationComposition(slot);
  puts("Default GPU world surfaces: exact serial/helper, explicit/unculled/cold/warm settings, geography, mountain and Advent revisions; no CPU ocean/mountain work");
}

static void TestWorldAtlasVersions(SDL_Renderer *renderer, const FrameSlot *slot) {
  const char *incoming = SDL_getenv("AR_SIM3D_WORLD_GPU_GRID");
  char *saved = incoming ? SDL_strdup(incoming) : NULL;
  CHECK(!incoming || saved);
  CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID"));
  InitSyntheticBloodpoolArt(true);
  FrameSlot *probe = malloc(sizeof(*probe)); CHECK(probe);
  SDL_Surface *reference[16] = {0};
  for (unsigned cohort = 0; cohort < 3; ++cohort) {
    PresentWorldNav_ResetResources();
    InitSlot(probe);
    probe->sim.world_navigation_ground_detail = true;
    probe->sim.world_navigation_towns.ground.enabled_town_mask = 2;
    memset(probe->sim.world_navigation_towns.ground.terrain[1],8,32*32);
    for (unsigned frame = 0; frame < 32; ++frame) {
      unsigned version = frame < 16 ? frame : (frame * 7) % 16;
      if (!cohort) PresentWorldNav_ResetResources(); /* Full-build oracle. */
      SimWorldMap_SetWaterAnimationSource(kWorldWaterSourceFirst +
          (version / 4) * kWorldWaterSourceStride);
      probe->sim.game_frame = (uint16_t)(1 + (version % 4) * kSimTownGroundAnimationTicks);
      BuildScene(probe);
      PerformanceMetrics_Configure(true,false);
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        SDL_Surface *actual = Render(renderer,probe,NULL);
        if (!cohort && frame < 16 && !repeat) reference[version] = actual;
        else { CHECK(Differences(actual,reference[version]) == 0); SDL_DestroySurface(actual); }
        PerformanceMetrics_PresentCompleted(1 + repeat*UINT64_C(1000000000));
      }
      PerformanceSnapshot measured; PerformanceMetrics_Snapshot(&measured);
      CHECK(measured.ready && measured.counts[kPerformanceCount_AtlasReuse] > 0);
      if (cohort && frame >= 16) {
        CHECK(measured.counts[kPerformanceCount_AtlasReuse] == 1);
        CHECK(measured.counts[kPerformanceCount_AtlasCopyCalls] == 0);
        CHECK(measured.stages[kPerformance_SimFirst+kSim3DPerformance_WorldAnimation].calls == 0);
        CHECK(measured.stages[kPerformance_SimFirst+kSim3DPerformance_WorldTransfer].calls == 0);
      }
      PerformanceMetrics_Configure(false,false);
    }
    /* Snapshot revisions must also survive resource teardown/rebuild with
     * the same public world identities (the next cohort starts cold). */
    Sim3DDepthPass_Reset(&g_render_device);
  }
  CHECK(Differences(reference[0],reference[4]) > 100);
  CHECK(Differences(reference[0],reference[1]) > 100);
  for (unsigned i = 0; i < 16; ++i) SDL_DestroySurface(reference[i]);
  PresentWorldNav_ResetResources();
  SimTownGroundArt_Shutdown();
  if (saved) CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID",saved,1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID"));
  SDL_free(saved); free(probe);
  UploadWorldNavigationComposition(slot);
  puts("world atlas: 16 animation pairs, cold/incremental/revisited/reset images exact");
}

static SDL_Surface *RenderSimGlobePresentation(SDL_Renderer *renderer, const FrameSlot *slot,
    const Scene3DCamera *camera, ArRenderRectI source, float radius_scale, bool sim_facades) {
  int width, height;
  CHECK(ArRenderDevice_GetOutputSize(&g_render_device,&width,&height));
  CHECK(ArRenderDevice_UseOutputCoordinates(&g_render_device));
  CHECK(ArRenderDevice_SetRenderTarget(&g_render_device,ArRenderTexture_Invalid()));
  CHECK(ArRenderDevice_SetViewport(&g_render_device,NULL));
  CHECK(ArRenderDevice_SetClipRect(&g_render_device,NULL));
  CHECK(ArRenderDevice_Clear(&g_render_device,(ArRenderColorF){.2f,.4f,.8f,1}));
  float matrix[16];
  Scene3D_BuildViewProjection(camera,width,height,matrix);
  PresentSimGlobeView view = {0};
  const ArRenderRectI viewport = {0,0,width,height};
  const PresentationOutcome outcome = sim_facades
      ? PresentSimGlobe_TestFacingTownScene(slot,source,viewport,camera,matrix,radius_scale,&view)
      : PresentSimGlobe_TestTownScene(slot,source,viewport,camera,matrix,radius_scale,&view);
  CHECK(outcome == kPresentationOutcome_Complete);
  /* Placement is published each frame for the later shroud. */
  CHECK(view.map.town == slot->sim.town && view.map.radius > 0 && view.map.metric > 0);
  CHECK(view.map.origin_x == slot->sim.underlay_origin_tile_x);
  CHECK(view.map.origin_y == slot->sim.underlay_origin_tile_y);
  CHECK(fabsf(Scene3D_ClipDepth(view.matrix,view.camera[0],view.camera[1],view.camera[2])) < .0001f);
  SDL_Surface *readback = SDL_RenderReadPixels(renderer,NULL);
  CHECK(readback);
  SDL_Surface *result = SDL_ConvertSurface(readback,SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(readback);
  CHECK(result && SDL_RenderPresent(renderer));
  return result;
}

static SDL_Surface *RenderSimGlobeScene(SDL_Renderer *renderer, const FrameSlot *slot,
    const Scene3DCamera *camera, ArRenderRectI source, float radius_scale) {
  return RenderSimGlobePresentation(renderer,slot,camera,source,radius_scale,false);
}

static SDL_Surface *RenderSimGlobe(SDL_Renderer *renderer, const FrameSlot *slot,
    const Scene3DCamera *camera, ArRenderRectI source) {
  return RenderSimGlobeScene(renderer,slot,camera,source,3);
}

static void TestNavigationZoomEntry(SDL_Renderer *renderer) {
  FrameSlot *navigation = malloc(sizeof(*navigation));
  FrameSlot *town = malloc(sizeof(*town));
  CHECK(navigation && town);
  PresentWorldNav_ResetResources();
  ResizeTestOutput(renderer, 2688, 2016);
  InitSlot(navigation);
  navigation->sim.world_navigation_models = navigation->sim.world_navigation_lighting = true;
  navigation->sim.world_navigation_towns.object_count = 96;
  for (unsigned i = 0; i < 96; ++i) {
    navigation->sim.world_navigation_towns.objects[i] = (SimBackgroundVoxelObject){
      .town = 2, .kind = kSimBackgroundVoxel_Windmill,
      .cell_x = 4 + (i % 12) * 2, .cell_y = 4 + (i / 12) * 2,
      .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished,
    };
  }
  navigation->sim.world_navigation.zoom_current = navigation->sim.world_navigation.zoom_target =
      kSimWorldNavigationZoomNear;
  navigation->sim.world_navigation.matrix[0] = navigation->sim.world_navigation.matrix[3] =
      kSimWorldNavigationZoomNear;
  BuildScene(navigation);
  *town = *navigation;
  town->sim.view = kSimView_Enhanced;
  town->sim.town = 4;
  int ox, oy; CHECK(SimWorldMap_OriginForTown(4, &ox, &oy));
  town->sim.underlay_origin_tile_x = ox; town->sim.underlay_origin_tile_y = oy;
  const Scene3DCamera camera = {-.575f,0,2,.4f};
  const ArRenderRectI source = {0,0,360,224};
  SDL_Surface *reference = RenderSimGlobe(renderer, town, &camera, source);
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(navigation);
  const int distances[] = {600,450,300,247,200,300,450,200};
  for (unsigned i = 0; i < sizeof(distances)/sizeof(*distances); ++i) {
    navigation->sim.projection_distance_x100 = distances[i];
    SDL_Surface *image = Render(renderer, navigation, NULL);
    SDL_DestroySurface(image);
    CHECK(WorldNavigationModelMesh_Enabled());
  }
  /* Do not reset any resources between close-up navigation and town entry. */
  SDL_Surface *entry = RenderSimGlobe(renderer, town, &camera, source);
  CHECK(Differences(reference, entry) == 0);
  SDL_DestroySurface(entry); SDL_DestroySurface(reference);
  /* The connected camera's far bound must still permit an immediately
   * visible wheel step toward the town. Both renders use the same mapping,
   * selected town and resident geometry; only the camera distance changes. */
  Scene3DCamera zoom = {-.575f,0,4.5f,.4f};
  PresentSimGlobe_ClampCamera(&zoom);
  SDL_Surface *far = RenderSimGlobe(renderer, town, &zoom, source);
  zoom.distance -= .25f;
  PresentSimGlobe_ClampCamera(&zoom);
  SDL_Surface *near = RenderSimGlobe(renderer, town, &zoom, source);
  CHECK(Differences(far, near) > 100);
  SDL_DestroySurface(far); SDL_DestroySurface(near);
  /* More than one upload of visible terrain, plus two equally large sets
   * beyond the bounded SIM neighbourhood on either side. */
  enum { kMountainCopies = 65537 };
  SimWorldNavigationMountainFace *faces = calloc(kMountainCopies * 3, sizeof(*faces));
  CHECK(faces);
  for (unsigned group = 0; group < 3; ++group) {
    SimWorldNavigationMountainFace face = {.town = group == 0 ? town->sim.town : 2};
    for (unsigned p = 0; p < 4; ++p) {
      face.x[p] = ox + (group == 0 ? 80 : group == 1 ? -40 : -2) + ((p == 1 || p == 2) ? .5f : 0);
      face.y[p] = oy + 16 + (p >= 2 ? .5f : 0);
      face.z[p] = .1f; face.brightness[p] = 255;
      face.uv[p] = (SimBackgroundMountainMeshUV){.25f,.25f};
    }
    for (unsigned i = 0; i < kMountainCopies; ++i) faces[group*kMountainCopies+i] = face;
  }
  CHECK(Sim3DDepthPass_Begin(&g_render_device,2688,2016,kArRenderFilter_Nearest));
  size_t published = 0, chunks = 0;
  CHECK(PresentSimGlobe_TestSurfaceSource(town,faces,kMountainCopies*3,&published,&chunks));
  CHECK(published == kMountainCopies && chunks == 2);
  free(faces);
  free(town); free(navigation);
  PresentWorldNav_ResetResources();
  ResizeTestOutput(renderer, kWidth, kHeight);
  puts("close-up navigation -> SIM: 96 animated buildings, eight zoom/LOD changes, cold/transition pixels exact");
}

static void TestContinuousTownScene(SDL_Renderer *renderer) {
#if AR_SIM3D_TERRAIN_ELEVATION
  /* Every native cliff field must fit the top/side receiver partition,
   * including fractional clipped cliff endpoints under fused arithmetic. */
  const unsigned terrain_heights[] = {0,40,100,150};
  for (uint8_t town = 1; town <= kSimTownCount; ++town) {
    int ox,oy; CHECK(SimWorldMap_OriginForTown(town,&ox,&oy));
    for (unsigned h = 0; h < sizeof(terrain_heights)/sizeof(terrain_heights[0]); ++h) {
      SimGlobeMapping map;
      CHECK(SimGlobeMapping_Build(town,ox,oy,288,0,terrain_heights[h]/100.0f,&map));
      CHECK(PresentSimGlobeTerrain_Prepare(&map,SimWorldMap_GeographySerial()));
      CHECK(PresentSimGlobeTerrain_Prepare(&map,SimWorldMap_GeographySerial()));
      PresentSimGlobeTerrain_Reset();
    }
  }
  puts("native curved receivers: all six towns at 0/40/100/150% prepare and reuse PASS");
#endif
  FrameSlot *slot = malloc(sizeof(*slot)); CHECK(slot);
  InitSlot(slot);
  slot->sim.view = kSimView_Enhanced;
  slot->sim.town = 2;
  slot->sim.underlay_origin_tile_x = slot->sim.underlay_origin_tile_y = 48;
  slot->sim.underlay_screen_x0 = 52;
  slot->sim.camera_x = 128; slot->sim.camera_y = 144;
  slot->sim.world_navigation_lighting = true;
  slot->sim.world_navigation_towns.object_count = 1;
  slot->sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
      .town = 2, .kind = kSimBackgroundVoxel_Cathedral,
      .cell_x = 15, .cell_y = 15, .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished};
  const Scene3DCamera camera = {-.575f,0,3,.4f};
  const ArRenderRectI source = {0,0,360,224}, viewport = {0,0,kWidth,kHeight};
  float matrix[16]; Scene3D_BuildViewProjection(&camera,kWidth,kHeight,matrix);
  CHECK(PresentSimGlobe_TestTownScene(slot,source,viewport,&camera,matrix,0,NULL)
      == kPresentationOutcome_CoreFailure);
  CHECK(PresentSimGlobe_TestTownScene(slot,source,viewport,&camera,matrix,NAN,NULL)
      == kPresentationOutcome_CoreFailure);
  CHECK(PresentSimGlobe_TestTownScene(slot,source,viewport,&camera,matrix,5,NULL)
      == kPresentationOutcome_CoreFailure);
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Count;
  CHECK(PresentSimGlobe_TestTownScene(slot,source,viewport,&camera,matrix,2,NULL)
      == kPresentationOutcome_CoreFailure);
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  slot->sim.world_navigation_models = false;
  SDL_Surface *without = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  slot->sim.world_navigation_models = true;
  SDL_Surface *high = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  CHECK(Differences(without,high) == 0); /* Navigation cannot remove the active SIM model. */
  SDL_DestroySurface(without);
  for (unsigned repeat = 0; repeat < 4; ++repeat) {
    SDL_Surface *cached = RenderSimGlobeScene(renderer,slot,&camera,source,2);
    CHECK(Differences(high,cached) == 0);
    SDL_DestroySurface(cached);
  }
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
  SDL_Surface *low = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  CHECK(Differences(high,low) > 20);
  SDL_DestroySurface(low);
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  SDL_Surface *restored = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  CHECK(Differences(high,restored) == 0);
  SDL_DestroySurface(restored);
  SDL_Surface *larger = RenderSimGlobeScene(renderer,slot,&camera,source,3);
  CHECK(Differences(high,larger) > 100);
  SDL_DestroySurface(larger);
  restored = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  CHECK(Differences(high,restored) == 0);
  SDL_DestroySurface(restored); SDL_DestroySurface(high);
  /* A camera-first art-direction pass must not turn into per-frame source
   * projection/republication. Count traffic, not wall time or GPU readback. */
  PerformanceMetrics_Configure(true,false);
  for (unsigned frame = 0; frame < 6; ++frame) {
    Scene3DCamera moving = camera;
    moving.tilt_x -= frame*.08f;
    SDL_Surface *image = RenderSimGlobeScene(renderer,slot,&moving,source,2);
    SDL_DestroySurface(image);
    PerformanceMetrics_PresentCompleted(1+frame*UINT64_C(200000000));
  }
  PerformanceSnapshot traffic; PerformanceMetrics_Snapshot(&traffic);
  CHECK(traffic.ready && traffic.counts[kPerformanceCount_GeometryPublish] == 0);
  CHECK(traffic.counts[kPerformanceCount_Vertices] > 0);
  PerformanceMetrics_Configure(false,false);
  free(slot);
  PresentWorldNav_ResetResources();
  puts("continuous town scene: active cathedral present, Low/Ultra selection, direct/cache/revisited radius exact; camera motion republishes no geometry; invalid inputs rejected");
}

static SDL_Surface *RenderCapturedFacingModels(SDL_Renderer *renderer,
    const WorldNavigationModelSource *sources, size_t count,
    const WorldNavigationModelSourceStyle *style,
    const SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount],
    const float matrix[16], double *uploaded_bytes) {
  PerformanceMetrics_Configure(false,false);
  PerformanceMetrics_Configure(true,false);
  CHECK(Sim3DDepthPass_Begin(&g_render_device,kWidth,kHeight,kArRenderFilter_Nearest));
  CHECK(WorldNavigationModelMesh_DrawFacingTown(sources,count,style,
      kSimBackgroundVoxelShading_MaterialAware,axes,matrix,0));
  ArRenderTexture result = Sim3DDepthPass_Submit(&g_render_device,ArRenderTexture_Invalid());
  CHECK(ArRenderTexture_IsValid(result));
  CHECK(ArRenderDevice_SetRenderTarget(&g_render_device,result));
  SDL_Surface *raw = SDL_RenderReadPixels(renderer,NULL); CHECK(raw);
  SDL_Surface *image = SDL_ConvertSurface(raw,SDL_PIXELFORMAT_ARGB8888); CHECK(image);
  SDL_DestroySurface(raw);
  CHECK(ArRenderDevice_SetRenderTarget(&g_render_device,ArRenderTexture_Invalid()));
  PerformanceMetrics_PresentCompleted(1);
  PerformanceMetrics_PresentCompleted(UINT64_C(1000000001));
  PerformanceSnapshot traffic; PerformanceMetrics_Snapshot(&traffic); CHECK(traffic.ready);
  *uploaded_bytes = traffic.counts[kPerformanceCount_DepthUploadBytes]*2;
  PerformanceMetrics_Configure(false,false);
  return image;
}

static void TestCapturedFacingMotion(SDL_Renderer *renderer, const FrameSlot *slot,
    const Scene3DCamera *camera, ArRenderRectI source) {
  float matrix[16]; Scene3D_BuildViewProjection(camera,kWidth,kHeight,matrix);
  PresentSimGlobeView view;
  CHECK(PresentSimGlobe_TestFacingTownScene(slot,source,(ArRenderRectI){0,0,kWidth,kHeight},
      camera,matrix,3,&view) == kPresentationOutcome_Complete);
  const SimBackgroundVoxelRenderParams params = SimVoxelRenderParams(
      slot,source,(ArRenderRectI){0,0,kWidth,kHeight},matrix);
  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  SimBackgroundVoxelProject_ResolveAxes(&params,axes);
  enum { kStatic = 24, kCount = kStatic+2 };
  WorldNavigationModelSource sources[kCount] = {0};
  for (unsigned i = 0; i < kCount; ++i) {
    const unsigned x = 8+(i%6)*3, y = 8+(i/6)*3;
    sources[i] = (WorldNavigationModelSource){
      .object = {.town=2,.kind=i < kStatic ? kSimBackgroundVoxel_Cathedral : kSimBackgroundVoxel_Windmill,
        .cell_x=x,.cell_y=y,.source_cells_w=2,.source_cells_h=2,
        .footprint_cells_w=2,.footprint_cells_d=2,.visual_state=kSimStructureVisualState_Finished},
      .detail=kSimBackgroundVoxelDetail_Ultra,.object_index=i,
      .source_x=(view.map.origin_x+x)*8,.source_y=(view.map.origin_y+y)*8,
      .centre_x=16,.centre_y=16,.anchor_height=4};
  }
  sources[kCount-1].object.flags = kSimBackgroundVoxel_UnderConstruction;
  sources[kCount-1].object.animation_phase = 1;
  WorldNavigationModelSourceStyle style = {.embedding=view.map,.surface_revision=SimWorldMap_GeographySerial(),
    .chart_radius_tiles=view.map.chart_radius,.tile_world=1/view.map.metric,
    .height_percent=100,.light_azimuth=0,.light_elevation=85,
    .style=kSimBackgroundVoxelStyle_Varied,.lighting=true,.captured_poses=true};
  WorldNavigationModelMesh_Reset();
  double cold_bytes, motion_bytes, held_bytes;
  SDL_Surface *first = RenderCapturedFacingModels(renderer,sources,kCount,&style,axes,view.matrix,&cold_bytes);
  CHECK(cold_bytes > 0);
  for (unsigned phase = 1; phase <= 3; ++phase) {
    sources[kStatic].object.animation_phase = phase%3;
    /* The adjacent scaffold stays at phase 1, not the moving rotor's phase. */
    SDL_Surface *warm = RenderCapturedFacingModels(renderer,sources,kCount,&style,axes,view.matrix,&motion_bytes);
    CHECK(motion_bytes > 0 && motion_bytes < cold_bytes*.20);
    SDL_Surface *held = RenderCapturedFacingModels(renderer,sources,kCount,&style,axes,view.matrix,&held_bytes);
    CHECK(held_bytes == 0 && Differences(warm,held) == 0); SDL_DestroySurface(held);
    WorldNavigationModelMesh_Reset();
    SDL_Surface *cold = RenderCapturedFacingModels(renderer,sources,kCount,&style,axes,view.matrix,&cold_bytes);
    CHECK(Differences(warm,cold) == 0);
    CHECK(phase == 3 ? Differences(first,warm) == 0 : Differences(first,warm) > 0);
    SDL_DestroySurface(warm); SDL_DestroySurface(cold);
  }
  printf("captured facing motion: static + independently phased windmills exact; cold %.0f bytes, rotor tick %.0f bytes, held %.0f bytes\n",
      cold_bytes,motion_bytes,held_bytes);
  SDL_DestroySurface(first); WorldNavigationModelMesh_Reset();
}

static void TestFacingTownScene(SDL_Renderer *renderer) {
  FrameSlot *slot = malloc(sizeof(*slot)); CHECK(slot);
  InitSlot(slot);
  slot->sim.view = kSimView_Enhanced;
  slot->sim.town = 2;
  int ox, oy; CHECK(SimWorldMap_OriginForTown(2,&ox,&oy));
  slot->sim.underlay_origin_tile_x = ox; slot->sim.underlay_origin_tile_y = oy;
  slot->sim.underlay_screen_x0 = 52;
  slot->sim.camera_x = 128; slot->sim.camera_y = 144;
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  slot->sim.background_voxel_facing = kSimBackgroundVoxelFacing_PerModel;
  slot->sim.background_voxel_shading = kSimBackgroundVoxelShading_MaterialAware;
  slot->sim.world_navigation_lighting = true;
  slot->sim.world_navigation_models = true;
  slot->sim.world_navigation_towns.object_count = 2;
  slot->sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
      .town = 2, .kind = kSimBackgroundVoxel_Cathedral,
      .cell_x = 13, .cell_y = 15, .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished};
  slot->sim.world_navigation_towns.objects[1] = slot->sim.world_navigation_towns.objects[0];
  slot->sim.world_navigation_towns.objects[1].kind = kSimBackgroundVoxel_Windmill;
  slot->sim.world_navigation_towns.objects[1].cell_x = 18;
  const Scene3DCamera camera = {-.75f,0,3,.4f};
  const ArRenderRectI source = {0,0,360,224};
  const ArRenderRectI viewport = {0,0,kWidth,kHeight};
  float matrix[16]; Scene3D_BuildViewProjection(&camera,kWidth,kHeight,matrix);
  slot->sim.background_voxel_facing = kSimBackgroundVoxelFacing_Count;
  CHECK(PresentSimGlobe_TestFacingTownScene(slot,source,viewport,&camera,matrix,2,NULL)
      == kPresentationOutcome_CoreFailure);
  slot->sim.background_voxel_facing = kSimBackgroundVoxelFacing_PerModel;
  slot->sim.background_voxel_shading = kSimBackgroundVoxelShading_Count;
  CHECK(PresentSimGlobe_TestFacingTownScene(slot,source,viewport,&camera,matrix,2,NULL)
      == kPresentationOutcome_CoreFailure);
  slot->sim.background_voxel_shading = kSimBackgroundVoxelShading_MaterialAware;
  TestCapturedFacingMotion(renderer,slot,&camera,source);
  SDL_Surface *raw = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  SDL_Surface *facing = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(raw,facing) > 100);
  SaveImage(raw,"synthetic-town-radial");
  SaveImage(facing,"synthetic-town-facades");
  for (unsigned repeat = 0; repeat < 4; ++repeat) {
    SDL_Surface *held = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
    CHECK(Differences(facing,held) == 0); SDL_DestroySurface(held);
  }
  slot->sim.background_voxel_facing = kSimBackgroundVoxelFacing_Shared;
  SDL_Surface *standard = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,standard) > 10); SDL_DestroySurface(standard);
  slot->sim.background_voxel_facing = kSimBackgroundVoxelFacing_PerModel;
  slot->sim.background_voxel_shading = kSimBackgroundVoxelShading_Basic;
  SDL_Surface *basic = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,basic) > 10); SDL_DestroySurface(basic);
  slot->sim.background_voxel_shading = kSimBackgroundVoxelShading_MaterialAware;
  SDL_Surface *restored = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,restored) == 0); SDL_DestroySurface(restored);
  slot->sim.world_navigation_lighting = false;
  SDL_Surface *unlit = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,unlit) == 0); SDL_DestroySurface(unlit);
  slot->sim.world_navigation_lighting = true;
  restored = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,restored) == 0); SDL_DestroySurface(restored);
  PerformanceMetrics_Configure(true,false);
  const uint64_t first_frame = slot->sim.game_frame;
  for (unsigned frame = 0; frame < 6; ++frame) {
    Scene3DCamera moving = camera;
    moving.tilt_x -= frame*.025f; moving.tilt_y += frame*.04f;
    slot->sim.game_frame = first_frame+frame*12;
    SDL_Surface *image = RenderSimGlobePresentation(renderer,slot,&moving,source,2,true);
    SDL_DestroySurface(image);
    PerformanceMetrics_PresentCompleted(1+frame*UINT64_C(200000000));
  }
  PerformanceSnapshot traffic; PerformanceMetrics_Snapshot(&traffic);
  CHECK(traffic.ready && traffic.counts[kPerformanceCount_GeometryPublish] == 0);
  CHECK(traffic.counts[kPerformanceCount_Vertices] > 0);
  PerformanceMetrics_Configure(false,false);
  slot->sim.game_frame = first_frame+12;
  SDL_Surface *animated = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,animated) > 10); SDL_DestroySurface(animated);
  slot->sim.game_frame = first_frame;
  restored = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(facing,restored) == 0); SDL_DestroySurface(restored);
  restored = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  CHECK(Differences(raw,restored) == 0); SDL_DestroySurface(restored);
  SDL_DestroySurface(raw); SDL_DestroySurface(facing);
  /* A bridge stays on the radial path, even when it belongs to this town. */
  slot->sim.world_navigation_towns.object_count = 1;
  slot->sim.world_navigation_towns.objects[0].kind = kSimBackgroundVoxel_Bridge;
  slot->sim.world_navigation_towns.objects[0].bridge_axis = kSimBackgroundBridgeAxis_EastWest;
  slot->sim.world_navigation_towns.objects[0].bridge_bank_a_x = 12;
  slot->sim.world_navigation_towns.objects[0].bridge_bank_b_x = 16;
  raw = RenderSimGlobeScene(renderer,slot,&camera,source,2);
  facing = RenderSimGlobePresentation(renderer,slot,&camera,source,2,true);
  CHECK(Differences(raw,facing) == 0);
  SDL_DestroySurface(raw); SDL_DestroySurface(facing);
  /* A visible cathedral just across the western border belongs to Kasandora,
   * not active Bloodpool. Neither active-town detail nor facades may alter it. */
  slot->sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
      .town = 3, .kind = kSimBackgroundVoxel_Cathedral,
      .cell_x = 29, .cell_y = 1, .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished};
  const Scene3DCamera wide = {-.75f,0,4.5f,.4f};
  raw = RenderSimGlobeScene(renderer,slot,&wide,source,2);
  facing = RenderSimGlobePresentation(renderer,slot,&wide,source,2,true);
  CHECK(Differences(raw,facing) == 0); SDL_DestroySurface(facing);
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
  facing = RenderSimGlobePresentation(renderer,slot,&wide,source,2,true);
  CHECK(Differences(raw,facing) == 0); SDL_DestroySurface(facing);
  slot->sim.world_navigation_models = false;
  SDL_Surface *ground = RenderSimGlobePresentation(renderer,slot,&wide,source,2,true);
  CHECK(Differences(raw,ground) > 20);
  SDL_DestroySurface(raw); SDL_DestroySurface(ground);
  PresentWorldNav_ResetResources(); free(slot);
  puts("active town facades: SIM shading/facing, animated windmill, geometric bridges, Low unchanged neighbours, direct/cache/rewind parity; camera and pose changes republish no geometry");
}

static void TestSynthetic(SDL_Renderer *renderer) {
  /* Legacy projected-geometry/cache oracles intentionally use compatibility.
   * TestGpuGridRevisions below explicitly clears this to verify the default. */
  const char *incoming_grid = SDL_getenv("AR_SIM3D_WORLD_GPU_GRID");
  char *saved_grid = incoming_grid ? SDL_strdup(incoming_grid) : NULL;
  CHECK(!incoming_grid || saved_grid);
  CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID","0",1));
  uint8_t *rom = calloc(kRomBytes, 1);
  CHECK(rom);
  /* A green central continent in blue ocean. Only public immutable decoder
   * inputs are generated: no copyrighted fixture or renderer-only texture. */
  rom[0xE3F93 + 1] = 0x60;
  rom[0xE3F93 + 0x10 * 2 + 1] = 0x60;
  memset(rom + 0x70000, 0x10, 64);
  memset(rom + 0x70000 + 0xAA * 64, 0x10, 64);
  memset(rom + 0x53000, 0x10, 4 * 64);
  rom[0xE3F93 + 0x11 * 2] = 0xe0;
  rom[0xE3F93 + 0x11 * 2 + 1] = 0x70;
  for (unsigned f = 1; f < 4; ++f) for (unsigned p = 0; p < 64; ++p)
    rom[0x53000 + f * 64 + p] = (p + f) % 4 < f ? 0x11 : 0x10;
  rom[0xE3F93 + 2] = 0x04; rom[0xE3F93 + 3] = 0x03;
  memset(rom + 0x70000 + 64, 1, 64);
  for (int y = 40; y < 88; y++)
    memset(rom + 0x33341 + y * 128 + 40, 1, 48);
  CHECK(SimWorldMap_Init(rom, kRomBytes));
  SimWorldMap_PublishBuiltTilemap(SimWorldMap_Baseline());
  free(rom);
  FrameSlot *slot = malloc(sizeof(*slot));
  CHECK(slot);
  InitSlot(slot);
  TestGroundLightDirections(renderer, slot, "synthetic");
  SDL_Surface *front = Render(renderer, slot, "synthetic-front");
  const uint32_t green = Pixel(front, kWidth / 2, kHeight / 2);
  CHECK(((green >> 8) & 255) > (green & 255));
  slot->sim_manual_orbit_yaw = kPi;
  SDL_Surface *back = Render(renderer, slot, "synthetic-back");
  const uint32_t blue = Pixel(back, kWidth / 2, kHeight / 2);
  CHECK((blue & 255) > ((blue >> 8) & 255));
  CHECK(Differences(front, back) > 1000);

  /* An actual Low factory above the front hemisphere, hidden behind the
   * opaque planet after rotation. Tests the complete geometry/depth path. */
  slot->sim.world_navigation_models = true;
  slot->sim.world_navigation_towns.object_count = 1;
  slot->sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
      .town = 2, .kind = kSimBackgroundVoxel_Factory, .cell_x = 15, .cell_y = 15,
      .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
      .visual_state = kSimStructureVisualState_Finished};
  SDL_Surface *hidden = Render(renderer, slot, NULL);
  CHECK(Differences(back, hidden) == 0);
  SDL_DestroySurface(hidden);
  slot->sim_manual_orbit_yaw = 0;
  SDL_Surface *model = Render(renderer, slot, "synthetic-model");
  CHECK(Differences(front, model) > 20);
  slot->sim.world_navigation_models = false;
  SDL_Surface *restored = Render(renderer, slot, NULL);
  CHECK(Differences(front, restored) == 0);
  SDL_DestroySurface(restored);
  SDL_DestroySurface(model);

  /* Full-world cloud cover must still contribute over unmapped ocean, then
   * toggling it off restores every pixel of the frozen background. */
  slot->sim_manual_orbit_yaw = kPi;
  slot->sim.world_navigation_clouds = slot->sim.world_navigation_cloud_shadows = true;
  SDL_Surface *clouds = Render(renderer, slot, "synthetic-back-clouds");
  CHECK(Differences(back, clouds) > 1000);
  slot->sim.world_navigation_clouds = false;
  restored = Render(renderer, slot, NULL);
  CHECK(Differences(back, restored) == 0);
  SDL_DestroySurface(restored);
  SDL_DestroySurface(clouds);

  /* The ROM-free CTest path must exercise poles and mixed-axis rotation too,
   * not just rely on optional local-art screenshots. */
  const float orbit[][2] = {
      {1.57079632679f, 0}, {-1.57079632679f, 0},
      {0, 1.57079632679f}, {0, -1.57079632679f}, {.8f, .6f}};
  for (size_t i = 0; i < sizeof(orbit) / sizeof(orbit[0]); i++) {
    slot->sim_manual_orbit_yaw = orbit[i][0];
    slot->sim_manual_orbit_pitch = orbit[i][1];
    SDL_Surface *view = Render(renderer, slot, NULL);
    CHECK(Differences(front, view) > 1000);
    SDL_DestroySurface(view);
  }
  slot->sim_manual_orbit_yaw = slot->sim_manual_orbit_pitch = 0;
  PresentWorldNav_ResetResources();
  Sim3DDepthPass_Reset(&g_render_device);
  UploadWorldNavigationComposition(slot);
  restored = Render(renderer, slot, NULL);
  CHECK(Differences(front, restored) == 0); /* Device resource rebuild too. */
  SDL_DestroySurface(restored);

  /* Use unmistakable synthetic colours for the original billboard/UI path.
   * Palace moves/hides with geography; the UI stays screen-space. */
  SimWorldNavigationComposition *composition = &slot->sim.world_navigation_scene.composition;
  composition->empty_animation = false;
  composition->palace = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 120, .screen_y = 104, .width = 16, .height = 16};
  composition->plaque = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 8, .screen_y = 8, .width = 32, .height = 8};
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) g_sim_world_navigation_palace_pixels[y * 256 + x] = 0xffff00ff;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 32; x++)
      g_sim_world_navigation_plaque_pixels[y * 256 + x] = 0xff00ffff;
  UploadWorldNavigationComposition(slot);
  slot->sim_manual_orbit_yaw = kPi;
  SDL_Surface *far_marker = Render(renderer, slot, NULL);
  CHECK(ColorCount(far_marker, 0xffff00ff) == 0);
  CHECK(ColorCount(far_marker, 0xff00ffff) > 500);
  slot->sim_manual_orbit_yaw = 0;
  SDL_Surface *near_marker = Render(renderer, slot, "synthetic-markers");
  CHECK(ColorCount(near_marker, 0xffff00ff) > 500);
  CHECK(ColorCount(near_marker, 0xff00ffff) == ColorCount(far_marker, 0xff00ffff));
  slot->sim_manual_orbit_yaw = .6f;
  SDL_Surface *moved_marker = Render(renderer, slot, "synthetic-markers-orbit");
  /* Fractional-size artwork can cover one more/less pixel column as it
   * crosses the raster grid. Its submitted dimensions are checked exactly
   * by the portable backend test; allow only that column here. */
  CHECK(abs(ColorCount(moved_marker, 0xffff00ff) - ColorCount(near_marker, 0xffff00ff)) <=
      (int)ceilf(16 * .75f * kHeight / kActRaiserAuthenticHeight));
  CHECK(abs(FirstColorX(moved_marker, 0xffff00ff) - FirstColorX(near_marker, 0xffff00ff)) > 10);
  CHECK(FirstColorX(moved_marker, 0xff00ffff) == FirstColorX(near_marker, 0xff00ffff));
  CheckColorMaskEqual(moved_marker, near_marker, 0xff00ffff);
  slot->sim_manual_orbit_yaw = 0;
  restored = Render(renderer, slot, NULL);
  CHECK(Differences(near_marker, restored) == 0);
  SDL_DestroySurface(restored);

  /* Zooming preserves radial alignment, while the Palace becomes smaller.
   * The native UI retains its exact pixel mask, unaffected by camera tilt. */
  slot->sim.projection_pitch_mrad = -575;
  SDL_Surface *travel_marker = Render(renderer, slot, "synthetic-markers-travel");
  const uint16_t distance = slot->sim.projection_distance_x100;
  slot->sim.projection_distance_x100 += 200;
  slot->sim.projection_pitch_mrad = -1300;
  slot->sim.projection_yaw_mrad = 650;
  SDL_Surface *centred_marker = Render(renderer, slot, "synthetic-markers-centred");
  const int close_pixels = ColorCount(travel_marker, 0xffff00ff);
  const int wide_pixels = ColorCount(centred_marker, 0xffff00ff);
  CHECK(wide_pixels > close_pixels * .30f && wide_pixels < close_pixels * .42f);
  CHECK(FirstColorX(centred_marker, 0xffff00ff) > FirstColorX(travel_marker, 0xffff00ff));
  CHECK(FirstColorY(centred_marker, 0xffff00ff) > FirstColorY(travel_marker, 0xffff00ff));
  CheckColorMaskEqual(centred_marker, travel_marker, 0xff00ffff);
  slot->sim.projection_distance_x100 = distance;
  restored = Render(renderer, slot, NULL);
  CHECK(Differences(travel_marker, restored) == 0);
  SDL_DestroySurface(restored);
  SDL_DestroySurface(centred_marker);
  SDL_DestroySurface(travel_marker);
  SDL_DestroySurface(moved_marker);
  SDL_DestroySurface(far_marker);
  SDL_DestroySurface(near_marker);
  SDL_DestroySurface(front);
  SDL_DestroySurface(back);
  TestFraming(renderer, slot);
  TestAtmosphereProfile(renderer, slot);
  TestWeatherMotion(renderer, slot, "synthetic", true);
  TestOutputResizing(renderer, slot);
  TestColdBlackEntry(renderer, slot);
  /* These three fixtures specifically certify the CPU projected-cache
   * fallback and its exact legacy counters. Keep their opt-out explicit. */
  const char *incoming_models = SDL_getenv("AR_SIM3D_WORLD_GPU_MODELS");
  char *saved_models = incoming_models ? SDL_strdup(incoming_models) : NULL;
  CHECK(!incoming_models || saved_models);
  CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_MODELS", "0", 1));
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(slot);
  TestTownLodMotion(renderer, slot);
  TestAnimatedTownCache(renderer, slot, false);
  TestAnimatedTownCache(renderer, slot, true);
  if (saved_models) CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_MODELS", saved_models, 1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_MODELS"));
  SDL_free(saved_models);
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(slot);
  TestRadialTownResidency(renderer, slot);
  TestGroundCacheRevisions(renderer, slot);
  TestRetainedMountainSurfaces(renderer, slot);
  TestAdventClearance(renderer, slot);
  TestTallModelViewport(renderer, slot);
  TestGpuGridRevisions(renderer, slot);
  TestWorldAtlasVersions(renderer, slot);
  TestNavigationZoomEntry(renderer);
  TestContinuousTownScene(renderer);
  TestFacingTownScene(renderer);
  free(slot);
  PresentWorldNav_ResetResources();
  Sim3DDepthPass_Reset(&g_render_device);
  SimBackgroundVoxelModelCache_Reset();
  SimWorldMap_Shutdown();
  if (saved_grid) CHECK(!SDL_setenv_unsafe("AR_SIM3D_WORLD_GPU_GRID",saved_grid,1));
  else CHECK(!SDL_unsetenv_unsafe("AR_SIM3D_WORLD_GPU_GRID"));
  SDL_free(saved_grid);
}

static uint8_t *ReadFile(const char *path, size_t size) {
  FILE *file = fopen(path, "rb");
  CHECK(file);
  uint8_t *bytes = malloc(size);
  CHECK(bytes && fread(bytes, 1, size, file) == size && fgetc(file) == EOF);
  CHECK(fclose(file) == 0);
  return bytes;
}

static void ResizeTestOutput(SDL_Renderer *renderer, int width, int height) {
  SDL_Window *window = SDL_GetRenderWindow(renderer);
  CHECK(window && SDL_SetWindowSize(window, width, height));
  CHECK(SDL_SyncWindow(window));
  SDL_PumpEvents();
  /* Retire the hidden GPU renderer's previous-size output before capturing
   * the new view. SDL's size query alone does not prove that the acquired
   * output has changed. This is test plumbing, not a timed game frame. */
  CHECK(SDL_SetRenderViewport(renderer, NULL));
  CHECK(SDL_SetRenderClipRect(renderer, NULL));
  CHECK(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255));
  CHECK(SDL_RenderClear(renderer) && SDL_RenderPresent(renderer));

  /* Verify real drawing in all four corners, not just the readback header.
   * A stale smaller output used to stretch its final row/column across HD
   * captures, letting pixel-difference assertions pass on corrupt images. */
  CHECK(SDL_RenderClear(renderer));
  const uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
  for (int corner = 0; corner < 4; corner++) {
    const SDL_FRect rect = {
      corner & 1 ? (float)width - 16 : 0,
      corner & 2 ? (float)height - 16 : 0, 16, 16};
    const uint32_t color = colors[corner];
    CHECK(SDL_SetRenderDrawColor(renderer, (uint8_t)(color >> 16),
        (uint8_t)(color >> 8), (uint8_t)color, 255));
    CHECK(SDL_RenderFillRect(renderer, &rect));
  }
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
  CHECK(readback);
  SDL_Surface *surface = SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(readback);
  CHECK(surface && surface->w == width && surface->h == height);
  for (int corner = 0; corner < 4; corner++)
    CHECK(Pixel(surface, corner & 1 ? width - 8 : 8,
        corner & 2 ? height - 8 : 8) == colors[corner]);
  CHECK(Pixel(surface, width / 2, height / 2) == 0xff000000);
  SDL_DestroySurface(surface);
  CHECK(SDL_RenderPresent(renderer));
}

static void CaptureTownAcceptanceMatrix(SDL_Renderer *renderer, const FrameSlot *slot) {
  FrameSlot *probe = malloc(sizeof(*probe)), *variant = malloc(sizeof(*variant));
  CHECK(probe && variant);
  const char *town_names[] = {"fillmore", "bloodpool", "kasandora", "aitos", "marahna", "northwall"};
  const struct {
    const char *name;
    uint16_t zoom;
    int distance;
    int width, height;
    bool isolate_models;
  } views[] = {
    {"near", kSimWorldNavigationZoomNear, 200, kWidth, kHeight, false},
    {"middle", kSimWorldNavigationZoomMiddle, 300, kWidth, kHeight, false},
    {"wide", kSimWorldNavigationZoomFar, 600, kWidth, kHeight, false},
    {"near-hd", kSimWorldNavigationZoomNear, 200, 1792, 1344, true},
  };
  const struct { const char *name; size_t offset; } toggles[] = {
    {"lighting", offsetof(SimFrameData, world_navigation_lighting)},
    {"clouds", offsetof(SimFrameData, world_navigation_clouds)},
    {"shadows", offsetof(SimFrameData, world_navigation_cloud_shadows)},
    {"atmosphere", offsetof(SimFrameData, world_navigation_atmosphere)},
    {"models", offsetof(SimFrameData, world_navigation_models)},
    {"relief", offsetof(SimFrameData, world_navigation_relief)},
    {"ground", offsetof(SimFrameData, world_navigation_ground_detail)},
    {"mountains", offsetof(SimFrameData, world_navigation_mountains)},
    {"space", offsetof(SimFrameData, world_navigation_backdrop)},
    {"haze", offsetof(SimFrameData, world_navigation_haze)},
  };
  int effect_views[sizeof(toggles) / sizeof(toggles[0])] = {0};
  int visible_towns = 0, views_checked = 0, low_changed_views = 0;
  const uint32_t serial = SimWorldMap_Serial();
  weather_time_ms = 60000;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(slot->sim.world_navigation_towns.enabled_town_mask & (1u << (town - 1)))) continue;
    int ox, oy;
    CHECK(SimWorldMap_OriginForTown(town, &ox, &oy));
    int town_model_views = 0;
    for (size_t view = 0; view < sizeof(views) / sizeof(views[0]); view++) {
      ResizeTestOutput(renderer, views[view].width, views[view].height);
      memcpy(probe, slot, sizeof(*probe));
      probe->sim.world_navigation.focus_x = (uint16_t)((ox + 16) * kSimWorldMapTilePixels);
      probe->sim.world_navigation.focus_y = (uint16_t)((oy + 16) * kSimWorldMapTilePixels);
      probe->sim.world_navigation.active_location = town;
      probe->sim.world_navigation.zoom_current = probe->sim.world_navigation.zoom_target = views[view].zoom;
      probe->sim.world_navigation.matrix[0] = probe->sim.world_navigation.matrix[3] = (int16_t)views[view].zoom;
      probe->sim.world_navigation.matrix[1] = probe->sim.world_navigation.matrix[2] = 0;
      probe->sim.projection_distance_x100 = views[view].distance;
      probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
      probe->sim.world_navigation_haze = true;
      probe->sim.underlay_haze_pct = probe->sim.underlay_defocus_pct = 25;
      probe->sim.cull_haze_lead_px = 64;
      BuildScene(probe);
      char prefix[96], name[128];
      CHECK(snprintf(prefix, sizeof(prefix), "town-%s-%s", town_names[town - 1], views[view].name) > 0);
      CHECK(snprintf(name, sizeof(name), "%s-full", prefix) > 0);
      SDL_Surface *full = Render(renderer, probe, name);
      CHECK(full->w == views[view].width && full->h == views[view].height);
      const SimWorldNavigationFrame navigation = probe->sim.world_navigation;
      if (views[view].isolate_models) {
        /* Keep model-owned ground cleanup enabled in both images. The only
         * difference is this town's captured objects, so neighboring towns
         * and changes to the 2D source art cannot produce a false pass. */
        memcpy(variant, probe, sizeof(*variant));
        variant->sim.world_navigation_towns.object_count = 0;
        SDL_Surface *empty = Render(renderer, variant, NULL);
        for (uint16_t i = 0; i < probe->sim.world_navigation_towns.object_count; i++) {
          const SimWorldNavigationTownObject *object = &probe->sim.world_navigation_towns.objects[i];
          if (object->town == town)
            variant->sim.world_navigation_towns.objects[variant->sim.world_navigation_towns.object_count++] = *object;
        }
        CHECK(snprintf(name, sizeof(name), "%s-isolated-models", prefix) > 0);
        SDL_Surface *isolated = Render(renderer, variant, name);
        const int changed = Differences(empty, isolated);
        printf("town matrix %s isolated objects=%u changed=%d\n", prefix,
            variant->sim.world_navigation_towns.object_count, changed);
        if (!changed)
          for (uint16_t i = 0; i < variant->sim.world_navigation_towns.object_count; i++) {
            const SimWorldNavigationTownObject *object = &variant->sim.world_navigation_towns.objects[i];
            printf("invisible object kind=%u town=%u cell=(%u,%u) source=(%u,%u) footprint=(%u,%u)\n",
                object->kind, object->town, object->cell_x, object->cell_y,
                object->source_cells_w, object->source_cells_h,
                object->footprint_cells_w, object->footprint_cells_d);
          }
        CHECK(variant->sim.world_navigation_towns.object_count > 0 && changed > 20);
        SDL_Surface *restored = Render(renderer, probe, NULL);
        CHECK(Differences(full, restored) == 0);
        SDL_DestroySurface(empty); SDL_DestroySurface(isolated); SDL_DestroySurface(restored);
      }
      for (int profile = 0; profile < 2; profile++) {
        memcpy(variant, probe, sizeof(*variant));
        variant->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
        if (profile) {
          variant->sim.world_navigation_clouds = variant->sim.world_navigation_cloud_shadows = false;
          variant->sim.world_navigation_atmosphere = variant->sim.world_navigation_haze = false;
        }
        CHECK(snprintf(name, sizeof(name), "%s-%s", prefix, profile ? "effects-off" : "low-models") > 0);
        SDL_Surface *reduced = Render(renderer, variant, name);
        const int changed = Differences(full, reduced);
        if (!profile) low_changed_views += changed != 0;
        printf("town matrix %s %s changed=%d\n", prefix, profile ? "effects-off" : "low-models", changed);
        SDL_Surface *restored = Render(renderer, probe, NULL);
        CHECK(Differences(full, restored) == 0);
        SDL_DestroySurface(reduced); SDL_DestroySurface(restored);
      }
      for (size_t effect = 0; effect < sizeof(toggles) / sizeof(toggles[0]); effect++) {
        memcpy(variant, probe, sizeof(*variant));
        *((uint8_t *)&variant->sim + toggles[effect].offset) = 0;
        CHECK(snprintf(name, sizeof(name), "%s-no-%s", prefix, toggles[effect].name) > 0);
        SDL_Surface *off = Render(renderer, variant, view == 1 ? name : NULL);
        const int changed = Differences(full, off);
        effect_views[effect] += changed != 0;
        if (!strcmp(toggles[effect].name, "models")) town_model_views += changed > 20;
        SDL_Surface *restored = Render(renderer, probe, NULL);
        CHECK(Differences(full, restored) == 0);
        SDL_DestroySurface(off); SDL_DestroySurface(restored);
      }
      PresentWorldNav_ResetResources();
      Sim3DDepthPass_Reset(&g_render_device);
      UploadWorldNavigationComposition(probe);
      SDL_Surface *reset = Render(renderer, probe, NULL);
      CHECK(Differences(full, reset) == 0);
      CHECK(!memcmp(&navigation, &probe->sim.world_navigation, sizeof(navigation)));
      CHECK(!memcmp(&probe->sim.world_navigation_towns, &slot->sim.world_navigation_towns,
          sizeof(slot->sim.world_navigation_towns)));
      CHECK(SimWorldMap_Serial() == serial);
      SDL_DestroySurface(full); SDL_DestroySurface(reset);
      views_checked++;
    }
    CHECK(town_model_views > 0);
    visible_towns++;
  }
  CHECK(visible_towns > 0);
  for (size_t effect = 0; effect < sizeof(toggles) / sizeof(toggles[0]); effect++) {
    CHECK(effect_views[effect] > 0);
    printf("town matrix toggle %s visible=%d/%d; restoration exact\n",
        toggles[effect].name, effect_views[effect], views_checked);
  }
  printf("town matrix towns=%d views=%d low-model-differences=%d; reset/data invariants exact\n",
      visible_towns, views_checked, low_changed_views);
  weather_time_ms = 0;
  ResizeTestOutput(renderer, kWidth, kHeight);
  UploadWorldNavigationComposition(slot);
  free(probe); free(variant);
}

static void TestVolcanoCapViews(SDL_Renderer *renderer, const FrameSlot *slot) {
  if (!(slot->sim.world_navigation_towns.enabled_town_mask & 8)) return;
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.world_navigation.focus_x = 200;
  probe->sim.world_navigation.focus_y = 336;
  probe->sim.world_navigation.zoom_current = 130;
  probe->sim.game_frame = 17;
  probe->sim.world_navigation_brightness = 15;
  probe->sim.projection_distance_x100 = 300;
  probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
  const int16_t matrices[5][4] = {
    {64, 0, 0, 64}, {0, -64, 64, 0}, {-64, 0, 0, -64},
    {0, 64, -64, 0}, {64, 0, 0, 64},
  };
  const uint32_t serial = SimWorldMap_Serial();
  ResizeTestOutput(renderer, 1792, 1344);
  for (int view = 0; view < 5; view++) {
    memcpy(probe->sim.world_navigation.matrix, matrices[view], sizeof(matrices[view]));
    probe->sim.projection_pitch_mrad = view == 4 ? 0 : -575;
    BuildScene(probe);
    char name[32];
    CHECK(snprintf(name, sizeof(name), "captured-volcano-cap-%d", view) > 0);
    SDL_Surface *frame = Render(renderer, probe, name);
    SDL_Surface *held = Render(renderer, probe, NULL);
    CHECK(Differences(frame, held) == 0);
    int lava_pixels = 0;
    for (int y = 0; y < frame->h; y++)
      for (int x = 0; x < frame->w; x++) {
        const uint32_t color = Pixel(frame, x, y);
        const unsigned r = color >> 16 & 255, g = color >> 8 & 255, b = color & 255;
        lava_pixels += r > 40 && g < r / 4 && b < r / 4;
      }
    CHECK(lava_pixels > 100);
    SDL_DestroySurface(frame); SDL_DestroySurface(held);
  }
  CHECK(SimWorldMap_Serial() == serial);
  CHECK(!memcmp(&probe->sim.world_navigation_towns, &slot->sim.world_navigation_towns,
      sizeof(slot->sim.world_navigation_towns)));
  ResizeTestOutput(renderer, kWidth, kHeight);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestMountainFitViews(SDL_Renderer *renderer, const FrameSlot *slot) {
  if (!(slot->sim.world_navigation_towns.enabled_town_mask & 8)) return;
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  const int centres[2][2] = {{14, 15}, {28, 22}};
  const int16_t matrices[3][4] = {{64, 0, 0, 64}, {0, -64, 64, 0}, {-64, 0, 0, -64}};
  int ox, oy;
  CHECK(SimWorldMap_OriginForTown(4, &ox, &oy));
  const uint32_t serial = SimWorldMap_Serial();
  ResizeTestOutput(renderer, 1280, 960);
  for (int peak = 0; peak < 2; peak++) for (int view = 0; view < 3; view++) {
    memcpy(probe, slot, sizeof(*probe));
    probe->sim.world_navigation.focus_x = (ox + centres[peak][0]) * kSimWorldMapTilePixels;
    probe->sim.world_navigation.focus_y = (oy + centres[peak][1]) * kSimWorldMapTilePixels;
    probe->sim.world_navigation.active_location = 4;
    probe->sim.world_navigation.zoom_current = probe->sim.world_navigation.zoom_target = 130;
    memcpy(probe->sim.world_navigation.matrix, matrices[view], sizeof(matrices[view]));
    probe->sim.game_frame = 17;
    probe->sim.world_navigation_brightness = 15;
    probe->sim.projection_distance_x100 = 220;
    probe->sim.projection_pitch_mrad = -575;
    probe->sim_manual_orbit_yaw = probe->sim_manual_orbit_pitch = 0;
    BuildScene(probe);
    char name[48];
    CHECK(snprintf(name, sizeof(name), "captured-mountain-fit-%d-%d", peak, view) > 0);
    SDL_Surface *image = Render(renderer, probe, name);
    SDL_Surface *held = Render(renderer, probe, NULL);
    CHECK(Differences(image, held) == 0);
    SDL_DestroySurface(image); SDL_DestroySurface(held);
  }
  CHECK(SimWorldMap_Serial() == serial);
  ResizeTestOutput(renderer, kWidth, kHeight);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestCapturedMarahnaSanctuary(SDL_Renderer *renderer, const FrameSlot *slot) {
  const SimWorldNavigationTowns *towns = &slot->sim.world_navigation_towns;
  if (!(towns->enabled_town_mask & (1u << 4))) return;
  const SimWorldNavigationTownObject *sanctuary = NULL;
  for (int i = 0; i < towns->object_count; i++) {
    const SimWorldNavigationTownObject *object = &towns->objects[i];
    if (object->town == 5 && (object->kind == kSimBackgroundVoxel_Cathedral ||
        object->kind == kSimBackgroundVoxel_MarahnaTemple)) {
      CHECK(!sanctuary);
      sanctuary = object;
    }
  }
  CHECK(sanctuary);
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  memcpy(probe, slot, sizeof(*probe));
  probe->sim.world_navigation_towns.objects[0] = *sanctuary;
  probe->sim.world_navigation_towns.object_count = 1;
  int ox, oy;
  CHECK(SimWorldMap_OriginForTown(5, &ox, &oy));
  probe->sim.world_navigation.focus_x = (ox + sanctuary->cell_x + 1) * kSimWorldMapTilePixels;
  probe->sim.world_navigation.focus_y = (oy + sanctuary->cell_y + 1) * kSimWorldMapTilePixels;
  probe->sim.world_navigation.active_location = 5;
  probe->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
  probe->sim.projection_distance_x100 = 200;
  probe->sim.world_navigation.zoom_current = kSimWorldNavigationZoomNear;
  probe->sim.world_navigation.matrix[0] = probe->sim.world_navigation.matrix[3] = kSimWorldNavigationZoomNear;
  probe->sim.world_navigation_models = true;
  /* Keep the cleaned footprint and global height bound identical. Removing
   * only drawable geometry prevents a vanished 2D glyph from passing this. */
  probe->sim.world_navigation_towns.objects[0].footprint_cells_w = 0;
  BuildScene(probe);
  SDL_Surface *off = Render(renderer, probe, "captured-marahna-sanctuary-off");
  probe->sim.world_navigation_towns.objects[0].footprint_cells_w = sanctuary->footprint_cells_w;
  SDL_Surface *on = Render(renderer, probe, "captured-marahna-sanctuary-on");
  CHECK(Differences(off, on) > 100);
  SDL_Surface *held = Render(renderer, probe, NULL);
  CHECK(Differences(on, held) == 0);
  SDL_DestroySurface(held);
  probe->sim.world_navigation_towns.objects[0].footprint_cells_w = 0;
  SDL_Surface *restored = Render(renderer, probe, NULL);
  CHECK(Differences(off, restored) == 0);
  SDL_DestroySurface(off); SDL_DestroySurface(on); SDL_DestroySurface(restored);
  UploadWorldNavigationComposition(slot);
  free(probe);
}

static void TestCapturedSimGlobe(SDL_Renderer *renderer, FrameSlot *navigation) {
  const struct { Scene3DCamera camera; bool relief; } views[] = {
    {{-.575f, 0, 3, .4f}, true},
    {{-.85f, .35f, 4.5f, .4f}, true},
    {{-.85f, .35f, 4.5f, .4f}, false},
    {{-1.35f, 0, 3, .4f}, true},
    {{-1.35f, .35f, 4.5f, .4f}, true},
    {{-1.35f, -.35f, 4.5f, .4f}, true},
    {{-1.35f, 0, 2, .4f}, true},
    {{-1.35f, .35f, 4.5f, .4f}, false},
  };
  FrameSlot *probe = malloc(sizeof(*probe));
  CHECK(probe);
  SDL_Surface *before = Render(renderer,navigation,NULL);
  for (uint8_t town = 1; town <= kSimTownCount; ++town) {
    *probe = *navigation;
    probe->sim.view = kSimView_Enhanced;
    probe->sim.town = town;
    int ox, oy; CHECK(SimWorldMap_OriginForTown(town,&ox,&oy));
    probe->sim.underlay_origin_tile_x = ox;
    probe->sim.underlay_origin_tile_y = oy;
    probe->sim.camera_x = 128; probe->sim.camera_y = 144;
    for (unsigned view = 0; view < sizeof(views)/sizeof(views[0]); ++view) {
      Scene3DCamera camera = views[view].camera;
      PresentSimGlobe_ClampCamera(&camera);
      const ArRenderRectI source = {0,0,360,224};
      probe->sim.world_navigation_relief = views[view].relief;
      SDL_Surface *direct = RenderSimGlobe(renderer,probe,&camera,source);
      char name[80];
      snprintf(name,sizeof(name),"captured-sim-underlay-town-%u-view-%u",town,view);
      SaveImage(direct,name);
      for (int repeat = 0; repeat < 4; ++repeat) {
        SDL_Surface *cached = RenderSimGlobe(renderer,probe,&camera,source);
        CHECK(Differences(direct,cached) == 0);
        SDL_DestroySurface(cached);
      }
      SDL_DestroySurface(direct);
    }
    /* This holds navigation's original camera key across an intervening
     * embedded-model publication. Repeat must never draw SIM-local meshes. */
    SDL_Surface *returned = Render(renderer,navigation,NULL);
    CHECK(Differences(before,returned) == 0);
    SDL_DestroySurface(returned);
  }
  SDL_DestroySurface(before); free(probe);
  puts("SIM connected world: six native towns, eight default/low-angle/bounded/relief-off views, direct-cache parity and navigation return exact");
}

static void CaptureSimGlobePrototype(SDL_Renderer *renderer, FrameSlot *navigation) {
  FrameSlot *probe = malloc(sizeof(*probe)), *unchanged = malloc(sizeof(*unchanged));
  CHECK(probe && unchanged);
  const uint32_t serial = SimWorldMap_Serial();
  SDL_Surface *navigation_before = Render(renderer,navigation,NULL);
  const ArRenderRectI source = {0,0,360,224};
  const float pitches[] = {-.575f,-1.15f};
  for (uint8_t town = 1; town <= kSimTownCount; ++town) {
    *probe = *navigation;
    probe->sim.view = kSimView_Enhanced;
    probe->sim.town = town;
    int ox, oy; CHECK(SimWorldMap_OriginForTown(town,&ox,&oy));
    probe->sim.underlay_origin_tile_x = ox;
    probe->sim.underlay_origin_tile_y = oy;
    probe->sim.underlay_screen_x0 = 52;
    probe->sim.camera_x = 128; probe->sim.camera_y = 144;
    probe->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
    for (unsigned view = 0; view < sizeof(pitches)/sizeof(*pitches); ++view) {
      Scene3DCamera camera = {pitches[view],0,4.5f,.4f};
      SDL_Surface *underlay = RenderSimGlobe(renderer,probe,&camera,source);
      SDL_Surface *radii[3] = {0};
      for (unsigned radius = 1; radius <= 3; ++radius) {
        *unchanged = *probe;
        SDL_Surface *direct = RenderSimGlobeScene(renderer,probe,&camera,source,radius);
        char name[96];
        snprintf(name,sizeof(name),"globe-town-%u-view-%u-radius-%u",town,view,radius);
        SaveImage(direct,name);
        for (unsigned repeat = 0; repeat < 4; ++repeat) {
          SDL_Surface *cached = RenderSimGlobeScene(renderer,probe,&camera,source,radius);
          CHECK(Differences(direct,cached) == 0);
          SDL_DestroySurface(cached);
        }
        CHECK(!memcmp(probe,unchanged,sizeof(*probe)));
        radii[radius-1] = direct;
      }
      CHECK(Differences(radii[0],radii[1]) > 100);
      CHECK(Differences(radii[1],radii[2]) > 100);
      /* Revisit both a smaller radius and the standard 3x globe, without a
       * reset. Sources and mountain joins must agree. */
      SDL_Surface *revisit = RenderSimGlobeScene(renderer,probe,&camera,source,1);
      CHECK(Differences(radii[0],revisit) == 0);
      SDL_DestroySurface(revisit);
      SDL_Surface *restored = RenderSimGlobe(renderer,probe,&camera,source);
      CHECK(Differences(underlay,restored) == 0);
      SDL_DestroySurface(restored); SDL_DestroySurface(underlay);
      for (unsigned i = 0; i < 3; ++i) SDL_DestroySurface(radii[i]);
    }
    /* Camera-only A/B: keep radius, scale and the authored models identical.
     * Do not add camera-facing facade deformation before judging this base. */
    const int camera_pitches[] = {-575,-750,-900,-1050};
    for (unsigned angle = 0; angle < sizeof(camera_pitches)/sizeof(*camera_pitches); ++angle) {
      const Scene3DCamera camera = {camera_pitches[angle]/1000.0f,0,4.5f,.4f};
      *unchanged = *probe;
      SDL_Surface *direct = RenderSimGlobeScene(renderer,probe,&camera,source,2);
      char name[96];
      snprintf(name,sizeof(name),"globe-town-%u-camera-%d-radius-2",town,-camera_pitches[angle]);
      SaveImage(direct,name);
      for (unsigned repeat = 0; repeat < 4; ++repeat) {
        SDL_Surface *held = RenderSimGlobeScene(renderer,probe,&camera,source,2);
        CHECK(Differences(direct,held) == 0);
        SDL_DestroySurface(held);
      }
      CHECK(!memcmp(probe,unchanged,sizeof(*probe)));
      SDL_DestroySurface(direct);
    }
    /* Approved pitch, identical camera and radius: raw radial models versus
     * active-town SIM facades/material shading. Neighbours remain Low. */
    const Scene3DCamera camera = {-.75f,0,4.5f,.4f};
    probe->sim.background_voxel_facing = kSimBackgroundVoxelFacing_PerModel;
    probe->sim.background_voxel_shading = kSimBackgroundVoxelShading_MaterialAware;
    *unchanged = *probe;
    SDL_Surface *facing = RenderSimGlobePresentation(renderer,probe,&camera,source,2,true);
    char name[96];
    snprintf(name,sizeof(name),"globe-town-%u-camera-750-radius-2-sim-facades",town);
    SaveImage(facing,name);
    for (unsigned repeat = 0; repeat < 4; ++repeat) {
      SDL_Surface *held = RenderSimGlobePresentation(renderer,probe,&camera,source,2,true);
      CHECK(Differences(facing,held) == 0); SDL_DestroySurface(held);
    }
    CHECK(!memcmp(probe,unchanged,sizeof(*probe)));
    SDL_DestroySurface(facing);
  }
  SDL_Surface *navigation_after = Render(renderer,navigation,NULL);
  CHECK(Differences(navigation_before,navigation_after) == 0);
  CHECK(SimWorldMap_Serial() == serial);
  SDL_DestroySurface(navigation_before); SDL_DestroySurface(navigation_after);
  free(probe); free(unchanged);
  puts("globe town prototype: six towns, two SIM pitches, three radii, four camera-only variants; direct/cache/revisited/navigation parity; captures unchanged");
}

static void TestCurvedProjectionWithView(const PresentSimGlobeView *view,
    ArRenderRectI source, ArRenderRectI viewport, const Scene3DCamera *camera) {
  PresentSimGlobeProjection projection;
  CHECK(PresentSimGlobeProject_Build(&view->map,view->matrix,source,viewport,
      Scene3D_AutoFitDistance(camera->fov_y),&projection));
  /* Sample actual owned cell corners (both sides of cliffs), using precisely
   * the same registered floor and radial encoding as the GPU terrain source. */
  for (int y = 0; y < 32; y += 7) for (int x = 0; x < 32; x += 7) {
    float floors[4];
    CHECK(SimWorldNavigationTerrain_TownCellCorners(view->map.town,x,y,floors));
    for (unsigned corner = 0; corner < 4; ++corner) {
      const float px = (x+(corner == 1 || corner == 2))*16;
      const float py = (y+(corner == 2 || corner == 3))*16;
      PresentSimGlobeProjectedPoint point;
      CHECK(PresentSimGlobeProject_Point(&projection,px,py,floors[corner],0,&point));
      float normal[3], elevation[2], world[3];
      CHECK(SimGlobeMapping_Encode(&view->map,view->map.origin_x+px/16,
          view->map.origin_y+py/16,floors[corner],0,normal,elevation));
      for (unsigned i = 0; i < 3; ++i)
        world[i] = normal[i]*(view->map.radius+elevation[0])-(i == 2 ? view->map.radius : 0);
      Scene3DPoint screen; float depth;
      CHECK(Scene3D_ProjectWorldPointWithDepth(view->matrix,world[0],world[1],world[2],
          viewport.w,viewport.h,&screen,&depth));
      CHECK(fabsf(point.screen.x-viewport.x-screen.x) < .01f);
      CHECK(fabsf(point.screen.y-viewport.y-screen.y) < .01f);
      CHECK(fabsf(point.depth-depth) < .000001f);
    }
  }
}

static void SaveTownHeightProfiles(uint8_t town) {
  if (!output_directory) return;
  char path[4096];
  CHECK(snprintf(path,sizeof(path),"%s/town-%u-height-profiles.txt",output_directory,town) < (int)sizeof(path));
  FILE *file = fopen(path,"w"); CHECK(file);
  fputs("world_x world_y registered_floor fillmore_native bloodpool_native kasandora_native\n",file);
  for (int region = 0; region < 2; ++region) {
    const int y0 = region ? 68 : 48, y1 = region ? 96 : 80;
    const float x0 = region ? 12 : 74, x1 = region ? 32 : 86;
    for (int y = y0; y <= y1; y += 2) for (float x = x0; x <= x1; x += .5f)
      fprintf(file,"%.1f %d %.6f %.6f %.6f %.6f\n",x,y,
          SimWorldNavigationTerrain_FloorHeightUnits(x,y),
          SimTownTerrain_HeightUnitsAt(1,(x-80)*16,(y-48)*16),
          SimTownTerrain_HeightUnitsAt(2,(x-48)*16,(y-48)*16),
          SimTownTerrain_HeightUnitsAt(3,(x-16)*16,(y-64)*16));
  }
  CHECK(!fclose(file));
}

/* Production scene components, not a hand-drawn approximation of the old
 * town. Both sides omit actors/UI, cast shadows and independent eruption
 * effects; they share source state, viewport, camera, detail and clock. */
typedef struct CraterEffectCheck { unsigned count, stop; } CraterEffectCheck;
static bool CheckCraterEffect(void *user, const float x[4], const float y[4],
    const float z[4], const SimBackgroundProjectionAxis *axis, ArRenderColorF color) {
  CraterEffectCheck *check=user;
  for (unsigned i=0;i<4;++i) CHECK(isfinite(x[i]) && isfinite(y[i]) && isfinite(z[i]));
  CHECK(isfinite(axis->height_scale) && color.a>0 && color.a<=1);
  ++check->count;
  return !check->stop || check->count<check->stop;
}

static void TestCurvedCrater(const FrameSlot *slot, ArRenderRectI source,
    ArRenderRectI viewport, const float matrix[16], const PresentSimGlobeView *view) {
  SimBackgroundVoxelRenderParams params=SimVoxelRenderParams(slot,source,viewport,matrix);
  SimBackgroundMountainEffectSource effects;
  CHECK(SimBackgroundMountainRender_EffectSource(&params,&effects));
  SimBackgroundCraterAnchor anchor;
  CHECK(PresentSimGlobeMountains_CraterAnchor(&anchor)==(effects.count!=0));
  if (!effects.count) { CHECK(!anchor.valid); return; }
  const SimBackgroundCraterSource *crater=&effects.craters[effects.count-1];
  const SimGlobeMapping *map=&view->map;
  float floor,actual[3],reconstructed[3],normal[3],metric;
  CHECK(SimWorldNavigationTerrain_RegisterTownFloor(map->town,crater->x/16,crater->y/16,
      SimTownTerrain_HeightUnitsAt(map->town,crater->x,crater->y),&floor));
  CHECK(SimGlobeMapping_Point(map,map->origin_x+crater->x/16,map->origin_y+crater->y/16,floor,0,actual));
  CHECK(SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius,map->origin_x+crater->x/16,
      map->origin_y+crater->y/16,normal,&metric));
  const float rise=crater->z/16*metric/map->metric;
  actual[0]+=rise*crater->axis.x_per_height;
  actual[1]-=rise*crater->axis.y_per_height;
  actual[2]+=rise*crater->axis.height_scale;
  CHECK(SimWorldNavigationTerrain_RegisterTownFloor(map->town,anchor.local_x/16,anchor.local_y/16,
      SimTownTerrain_HeightUnitsAt(map->town,anchor.local_x,anchor.local_y),&floor));
  CHECK(SimGlobeMapping_Point(map,map->origin_x+anchor.local_x/16,map->origin_y+anchor.local_y/16,
      floor,anchor.height_pixels/16,reconstructed));
  for (unsigned i=0;i<3;++i) CHECK(fabsf(actual[i]-reconstructed[i])<.002f);
  for (unsigned frame=0;frame<48;++frame) {
    CraterEffectCheck check={0};
    CHECK(SimBackgroundMountainRender_EmitEffects(&effects,frame,kSimBackgroundVoxelDetail_Ultra,
        kSimBackgroundVoxelStyle_Architectural,CheckCraterEffect,&check));
    CHECK(check.count && check.count<=effects.count*28);
    check=(CraterEffectCheck){0};
    CHECK(SimBackgroundMountainRender_EmitEffects(&effects,frame,kSimBackgroundVoxelDetail_Low,
        kSimBackgroundVoxelStyle_Architectural,CheckCraterEffect,&check));
    CHECK(!check.count);
  }
  CraterEffectCheck check={.stop=2};
  CHECK(!SimBackgroundMountainRender_EmitEffects(&effects,0,kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelStyle_Architectural,CheckCraterEffect,&check) && check.count==2);
  effects.craters[effects.count-1].x=NAN; check=(CraterEffectCheck){0};
  CHECK(!SimBackgroundMountainRender_EmitEffects(&effects,0,kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelStyle_Architectural,CheckCraterEffect,&check) && !check.count);
}

static SDL_Surface *RenderDetailedTownContent(SDL_Renderer *renderer, const FrameSlot *slot,
    const Scene3DCamera *camera, ArRenderRectI source, bool clouds,
    const PresentSimGlobeContent *content) {
  CHECK(!content || sim_town_radius_scale==3);
  int width, height;
  CHECK(ArRenderDevice_GetOutputSize(&g_render_device,&width,&height));
  const ArRenderRectI viewport = {0,0,width,height};
  CHECK(ArRenderDevice_UseOutputCoordinates(&g_render_device));
  CHECK(ArRenderDevice_SetRenderTarget(&g_render_device,ArRenderTexture_Invalid()));
  CHECK(ArRenderDevice_SetViewport(&g_render_device,NULL));
  CHECK(ArRenderDevice_SetClipRect(&g_render_device,NULL));
  CHECK(ArRenderDevice_Clear(&g_render_device,(ArRenderColorF){0,0,0,1}));
  float matrix[16];
  Scene3D_BuildViewProjection(camera,width,height,matrix);
  DrawSimBackdrop(slot,viewport,matrix);
  PresentSimGlobeView view;
  CHECK((content
      ? PresentSimGlobeTown(slot,source,viewport,camera,matrix,content,&view)
      : PresentSimGlobe_TestDetailedTownScene(slot,source,viewport,camera,matrix,sim_town_radius_scale,&view)) ==
      kPresentationOutcome_Complete);
  TestCurvedProjectionWithView(&view,source,viewport,camera);
  TestCurvedCrater(slot,source,viewport,matrix,&view);
  CHECK(PresentSimGlobeWater_QuadCount()<=(40*40-32*32)*128);
  if (slot->sim.town==5) CHECK(PresentSimGlobeWater_QuadCount()>0);
  if (clouds) CHECK(DrawSimCloudShroud(slot,source,viewport,matrix,&view) == kPresentationOutcome_Complete);
  CHECK(!SessionFatal_Requested());
  SDL_Surface *readback = SDL_RenderReadPixels(renderer,NULL);
  CHECK(readback);
  SDL_Surface *result = SDL_ConvertSurface(readback,SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(readback);
  CHECK(result && SDL_RenderPresent(renderer));
  return result;
}

static SDL_Surface *RenderDetailedTown(SDL_Renderer *renderer, const FrameSlot *slot,
    const Scene3DCamera *camera, ArRenderRectI source, bool clouds) {
  return RenderDetailedTownContent(renderer,slot,camera,source,clouds,NULL);
}

static bool CheckCraterContent(void *user, const PresentSimGlobeView *view) {
  CHECK(view->map.town != 0);
  ++*(unsigned *)user;
  return true;
}

/* Numeric mouth placement alone cannot prove that the live composition
 * actually submits its smoke/glow. Exercise the real append seam separately
 * from the deliberately background-only visual comparison. */
static void TestLiveCraterComposition(SDL_Renderer *renderer, FrameSlot *slot,
    ArRenderRectI source) {
  if (slot->sim.town!=4 || sim_town_radius_scale!=3) return;
  FrameSlot *saved=malloc(sizeof(*saved)); CHECK(saved); *saved=*slot;
  slot->sim.camera_x=128; slot->sim.camera_y=64;
  slot->sim.background_voxel_detail=kSimBackgroundVoxelDetail_Ultra;
  slot->sim.background_voxel_style=kSimBackgroundVoxelStyle_Architectural;
  slot->sim.game_frame=0;
  const Scene3DCamera camera={-.75f,0,3.2f,.4f};
  unsigned calls=0;
  const PresentSimGlobeContent content={.append=CheckCraterContent,.userdata=&calls};
  SDL_Surface *bare=RenderDetailedTown(renderer,slot,&camera,source,false);
  const uint64_t revision=PresentSimGlobeMountains_Revision();
  SDL_Surface *glow=RenderDetailedTownContent(renderer,slot,&camera,source,false,&content);
  CHECK(Differences(bare,glow)>0);
  SDL_Surface *held=RenderDetailedTownContent(renderer,slot,&camera,source,false,&content);
  CHECK(Differences(glow,held)==0); SDL_DestroySurface(held);
  SaveImage(glow,"paired-town-4-live-crater-glow-smoke");
  slot->sim.game_frame=8;
  SDL_Surface *moving=RenderDetailedTownContent(renderer,slot,&camera,source,false,&content);
  CHECK(Differences(glow,moving)>0 && PresentSimGlobeMountains_Revision()==revision);
  CHECK(calls==3); /* Live content must never be skipped by color-image reuse. */
  SDL_DestroySurface(bare); SDL_DestroySurface(glow); SDL_DestroySurface(moving);
  slot->sim.background_voxel_detail=kSimBackgroundVoxelDetail_Low;
  bare=RenderDetailedTown(renderer,slot,&camera,source,false);
  held=RenderDetailedTownContent(renderer,slot,&camera,source,false,&content);
  CHECK(Differences(bare,held)==0 && calls==4);
  SDL_DestroySurface(bare); SDL_DestroySurface(held);
  *slot=*saved; free(saved);
  puts("live curved crater: visible glow/smoke, deterministic motion, resident relief, Low omission PASS");
}

static void TestDetailedMountainReuse(SDL_Renderer *renderer, FrameSlot *slot,
    ArRenderRectI source) {
  FrameSlot *saved = malloc(sizeof(*saved)); CHECK(saved); *saved = *slot;
  Scene3DCamera camera = {-.75f,0,4.5f,.4f};
  SDL_Surface *warm = RenderDetailedTown(renderer,slot,&camera,source,false);
  SDL_DestroySurface(warm);
  uint64_t revision = PresentSimGlobeMountains_Revision();
  /* Focus is spatial material state: toggling it changes the surroundings,
   * not native relief or its source-cache revision. Image keys must include
   * it, including an exact return to the original unmodified setting. */
  SDL_Surface *clear=RenderDetailedTown(renderer,slot,&camera,source,false);
  slot->sim.effective_features |= kSimFeature_CullHaze;
  slot->sim.cull_dim_pct=30; slot->sim.underlay_haze_pct=20; slot->sim.cull_haze_lead_px=16;
  SDL_Surface *focus=RenderDetailedTown(renderer,slot,&camera,source,false);
  CHECK(Differences(clear,focus)>0 && PresentSimGlobeMountains_Revision()==revision);
  for (unsigned repeat=0;repeat<3;++repeat) {
    SDL_Surface *held=RenderDetailedTown(renderer,slot,&camera,source,false);
    CHECK(Differences(focus,held)==0); SDL_DestroySurface(held);
  }
  char name[80]; snprintf(name,sizeof(name),"paired-town-%u-focus",slot->sim.town);
  SaveImage(focus,name); SDL_DestroySurface(focus);
  *slot=*saved;
  SDL_Surface *restored=RenderDetailedTown(renderer,slot,&camera,source,false);
  CHECK(Differences(clear,restored)==0);
  SDL_DestroySurface(clear); SDL_DestroySurface(restored);
  for (unsigned step = 0; step < 12; ++step) {
    slot->sim.camera_x = 112+step*3; slot->sim.camera_y = 128+step*4;
    camera.distance = 3.2f+.1f*step;
    warm = RenderDetailedTown(renderer,slot,&camera,source,false);
    CHECK(PresentSimGlobeMountains_Revision() == revision);
    /* Force the old full-source path as an oracle at every viewpoint. */
    PresentSimGlobeMountains_Reset();
    SDL_Surface *cold = RenderDetailedTown(renderer,slot,&camera,source,false);
    CHECK(Differences(warm,cold) == 0);
    revision = PresentSimGlobeMountains_Revision();
    SDL_DestroySurface(warm); SDL_DestroySurface(cold);
  }
  for (unsigned change = 0; change < 3; ++change) {
    if (!change) camera.tilt_y = .2f;
    else if (change == 1) camera.tilt_x = -.9f;
    else slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Low;
    warm = RenderDetailedTown(renderer,slot,&camera,source,false);
    CHECK(PresentSimGlobeMountains_Revision() > revision);
    PresentSimGlobeMountains_Reset();
    SDL_Surface *cold = RenderDetailedTown(renderer,slot,&camera,source,false);
    CHECK(Differences(warm,cold) == 0);
    revision = PresentSimGlobeMountains_Revision();
    SDL_DestroySurface(warm); SDL_DestroySurface(cold);
  }
  *slot = *saved; free(saved);
  puts("detailed mountains: pan/fixed-detail zoom reuse; facing/detail invalidation; all warm/cold images exact");
}

static void TestDetailedCameraLimits(SDL_Renderer *renderer, FrameSlot *slot,
    ArRenderRectI source) {
  FrameSlot *saved = malloc(sizeof(*saved)); CHECK(saved); *saved = *slot;
  FrameSlot *unchanged = malloc(sizeof(*unchanged)); CHECK(unchanged);
  const float pitches[] = {kSim3DCameraPitchMinimumMrad/1000.0f,
      kSim3DCameraPitchMaximumMrad/1000.0f};
  const float yaw = kSim3DConnectedCameraYawMaximumMrad/1000.0f;
  const float distances[] = {kSim3DCameraDistanceMinimumX100/100.0f,
      kSim3DConnectedCameraDistanceMaximumX100/100.0f};
  /* Exercise every camera-bound corner, then change captured settings while
   * keeping a fixed view. No gameplay/asset state is modified by this sweep. */
  for (unsigned sample=0;sample<16;++sample) {
    *slot=*saved;
    Scene3DCamera camera={-.75f,0,4.5f,.4f};
    if (sample<8) {
      camera.tilt_x=pitches[sample&1];
      camera.tilt_y=(sample&2) ? yaw : -yaw;
      camera.distance=distances[(sample>>2)&1];
      slot->sim.camera_x=(sample&2) ? 224 : 32;
      slot->sim.camera_y=(sample&4) ? 256 : 64;
    } else {
      slot->sim.camera_x=128; slot->sim.camera_y=144;
      switch (sample) {
        case 8: slot->sim.landscape_height_pct=0; break;
        case 9: slot->sim.landscape_height_pct=150; break;
        case 10: slot->sim.background_voxel_detail=kSimBackgroundVoxelDetail_Low; break;
        case 11:
          slot->sim.world_navigation_relief=false;
          slot->sim.world_navigation_models=false;
          slot->sim.world_navigation_lighting=false;
          break;
        case 12: slot->sim.cloud_altitude_px=0; slot->sim.cloud_opacity_pct=100; break;
        case 13: slot->sim.cloud_altitude_px=256; slot->sim.cloud_opacity_pct=100; break;
        case 14:
          slot->sim.effective_features |= kSimFeature_CullHaze;
          slot->sim.cull_dim_pct=100; slot->sim.underlay_haze_pct=100;
          break;
        case 15: slot->sim.cloud_opacity_pct=0; break;
      }
    }
    Scene3DCamera clamped=camera; PresentSimGlobe_ClampCamera(&clamped);
    CHECK(clamped.tilt_x==camera.tilt_x && clamped.tilt_y==camera.tilt_y &&
        clamped.distance==camera.distance);
    weather_time_ms=12000;
    *unchanged=*slot;
    SDL_Surface *warm=RenderDetailedTown(renderer,slot,&camera,source,true);
    SDL_Surface *held=RenderDetailedTown(renderer,slot,&camera,source,true);
    CHECK(Differences(warm,held)==0); SDL_DestroySurface(held);
    PresentSimGlobeTerrain_Reset(); PresentSimGlobeMountains_Reset();
    PresentSimGlobeWater_Reset();
    SDL_Surface *cold=RenderDetailedTown(renderer,slot,&camera,source,true);
    if (Differences(warm,cold)) {
      fprintf(stderr,"camera/settings mismatch town=%u sample=%u pixels=%u\n",
          slot->sim.town,sample,Differences(warm,cold));
      SaveImage(warm,"limits-mismatch-warm"); SaveImage(cold,"limits-mismatch-cold");
    }
    CHECK(Differences(warm,cold)==0 && !memcmp(slot,unchanged,sizeof(*slot)));
    if (sample==0 || sample==5) {
      char name[96]; snprintf(name,sizeof(name),"paired-town-%u-limits-%u",slot->sim.town,sample);
      SaveImage(warm,name);
      /* Keep an identical-camera clear reference so cloud coverage can be
       * distinguished from coastline/terrain material discontinuities. */
      SDL_Surface *clear=RenderDetailedTown(renderer,slot,&camera,source,false);
      snprintf(name,sizeof(name),"paired-town-%u-limits-%u-clear",slot->sim.town,sample);
      SaveImage(clear,name); SDL_DestroySurface(clear);
    }
    SDL_DestroySurface(warm); SDL_DestroySurface(cold);
  }
  *slot=*saved; free(saved); free(unchanged);
  puts("continuous SIM limits: 8 camera corners and 8 settings variants; held/cold exact, immutable capture PASS");
}

static void CaptureLandscapeHeightSweep(SDL_Renderer *renderer, FrameSlot *slot,
    ArRenderRectI source) {
  FrameSlot *saved = malloc(sizeof(*saved)); CHECK(saved); *saved = *slot;
  FrameSlot *unchanged = malloc(sizeof(*unchanged)); CHECK(unchanged);
  /* Change only the existing ground-height control. In particular, keep
   * model rise, camera, radius, detail, lighting and weather time identical.
   * Zero tests flattening/invalidation; the last 100 tests exact restoration. */
  const uint16_t heights[] = {100,75,50,0,100};
  const struct { const char *name; int y; float distance; } views[] = {
    {"overview",144,4.5f},{"north-close",64,3.2f},{"south-edge",256,4.5f}};
  for (unsigned view = 0; view < sizeof(views)/sizeof(*views); ++view) {
    slot->sim.camera_x = 128; slot->sim.camera_y = views[view].y;
    const Scene3DCamera camera = {-.75f,0,views[view].distance,.4f};
    for (int clouds = 0; clouds < 2; ++clouds) {
      weather_time_ms = 12000;
      SDL_Surface *baseline = NULL;
      for (unsigned step = 0; step < sizeof(heights)/sizeof(*heights); ++step) {
        slot->sim.landscape_height_pct = heights[step];
        *unchanged = *slot;
        /* The first draw after a setting change must not reuse old geometry.
         * Compare it with explicitly rebuilt detailed sources. */
        SDL_Surface *warm = RenderDetailedTown(renderer,slot,&camera,source,clouds);
        for (int repeat = 0; repeat < 4; ++repeat) {
          SDL_Surface *held = RenderDetailedTown(renderer,slot,&camera,source,clouds);
          if (Differences(warm,held)) {
            fprintf(stderr,"height sweep mismatch town=%u view=%u clouds=%d height=%u repeat=%d pixels=%u\n",
                slot->sim.town,view,clouds,heights[step],repeat,Differences(warm,held));
            SaveImage(warm,"height-mismatch-first"); SaveImage(held,"height-mismatch-repeat");
          }
          CHECK(Differences(warm,held) == 0); SDL_DestroySurface(held);
        }
        PresentSimGlobeTerrain_Reset(); PresentSimGlobeMountains_Reset();
        SDL_Surface *cold = RenderDetailedTown(renderer,slot,&camera,source,clouds);
        CHECK(Differences(warm,cold) == 0); SDL_DestroySurface(cold);
        CHECK(!memcmp(slot,unchanged,sizeof(*slot)));
        if (!step) baseline = warm;
        if (step < 3) {
          char name[128];
          snprintf(name,sizeof(name),"height-town-%u-%s-%s-%u",slot->sim.town,
              views[view].name,clouds ? "clouds" : "clear",heights[step]);
          SaveImage(warm,name);
        }
        if (step == sizeof(heights)/sizeof(*heights)-1)
          CHECK(Differences(warm,baseline) == 0);
        if (step) SDL_DestroySurface(warm);
      }
      SDL_DestroySurface(baseline);
    }
  }
  const float maximum = SimTownTerrain_MaximumUnits(slot->sim.town);
  printf("landscape sweep: town=%u radius=%ux navigation maximum=%.6f units; 100/75/50%%=%.3f/%.3f/%.3f native pixels; warm/cold/zero/restoration PASS\n",
      slot->sim.town,sim_town_radius_scale,maximum,SimTownTerrain_ScaledHeightPixels(maximum,100),
      SimTownTerrain_ScaledHeightPixels(maximum,75),SimTownTerrain_ScaledHeightPixels(maximum,50));
  *slot = *saved; free(saved); free(unchanged);
}

static void CaptureTownPresentation(SDL_Renderer *renderer, FrameSlot *slot,
    const uint8_t *rom, const uint8_t *wram) {
  /* Match video boot: all production paths are prepared before capture. */
  const Sim3DPreparedPipelines prepared = Sim3DDepthPass_PreparePipelines(&g_render_device);
  CHECK(prepared.depth && prepared.linear_models && prepared.surfaces && prepared.radial);
  CHECK(wram[0x18] == 0 && wram[0x19] >= 1 && wram[0x19] <= 6);
  const uint8_t town = wram[0x19];
  char path[4096];
  CHECK(snprintf(path,sizeof(path),"%s.wram.bin",sim_town_snapshot) < (int)sizeof(path));
  uint8_t *paired_wram = ReadFile(path,kWramBytes);
  CHECK(!memcmp(paired_wram,wram,kWramBytes)); free(paired_wram);
  CHECK(snprintf(path,sizeof(path),"%s.vram.bin",sim_town_snapshot) < (int)sizeof(path));
  uint8_t *vb = ReadFile(path,65536);
  CHECK(snprintf(path,sizeof(path),"%s.cgram.bin",sim_town_snapshot) < (int)sizeof(path));
  uint8_t *cb = ReadFile(path,512);
  uint16_t vram[32768], cgram[256];
  for (int i = 0; i < 32768; ++i) vram[i] = vb[i*2] | (uint16_t)vb[i*2+1] << 8;
  for (int i = 0; i < 256; ++i) cgram[i] = cb[i*2] | (uint16_t)cb[i*2+1] << 8;
  free(vb); free(cb);
  SimWorldMapRomTables tables;
  CHECK(SimWorldMap_LoadRomTables(&tables,rom,kRomBytes));
  uint8_t developed[kSimWorldMapBytes]; uint16_t enabled[kSimTownCount];
  for (int i = 0; i < kSimTownCount; ++i)
    enabled[i] = wram[0x16b18+i*2] | (uint16_t)wram[0x16b19+i*2] << 8;
  CHECK(SimWorldMap_ComposeDeveloped(developed,SimWorldMap_Baseline(),
      (const uint8_t (*)[kSimWorldMapTownCells])(wram+0x12000),enabled,wram[0x19101],&tables));
  SimWorldMap_PublishBuiltTilemap(developed);
  SimWorldMap_SetWaterAnimationSource(kWorldWaterSourceFirst);
  SimTownCanvas_Render(town,wram,vram,cgram,15,0xff000000);
  SimBackgroundVoxels_Build(town,wram,SimTownCanvas_Pixels(),SimTownCanvas_SourceOpacity(),
      SimTownCanvas_Serial(),SimTownCanvas_TilemapSerial(),false);
  SimBackgroundVoxelRenderer_Upload(&g_render_device);
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  CHECK(!scene->overflow && scene->object_count && scene->town == town);
  slot->sim.view = kSimView_Enhanced; slot->sim.town = town;
  int ox,oy; CHECK(SimWorldMap_OriginForTown(town,&ox,&oy));
  slot->sim.underlay_origin_tile_x = ox; slot->sim.underlay_origin_tile_y = oy;
  slot->sim.underlay_serial = SimWorldMap_Serial();
  slot->sim.underlay_screen_x0 = 52;
  slot->sim.town_canvas_serial = SimTownCanvas_Serial();
  slot->sim.background_voxel_serial = SimBackgroundVoxels_Serial();
  slot->sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  slot->sim.background_voxel_lod = kSimBackgroundVoxelLod_Fixed;
  slot->sim.background_voxel_render_scale = kSimBackgroundVoxelRenderScale_Native;
  slot->sim.background_voxel_facing = kSimBackgroundVoxelFacing_PerModel;
  slot->sim.background_voxel_shading = kSimBackgroundVoxelShading_MaterialAware;
  slot->sim.landscape_height_pct = sim_town_landscape_pct;
  slot->sim.game_frame = 1500;
  slot->sim.light_azimuth_deg = 0; slot->sim.light_elevation_deg = 85;
  slot->sim.separated_backdrop_argb = 0xff305888;
  slot->sim.backdrop_strength_pct = 100; slot->sim.backdrop_horizon_pct = 50;
  slot->sim.cloud_opacity_pct = 55; slot->sim.cloud_altitude_px = 96;
  slot->sim.cloud_clear_x0 = 52; slot->sim.cloud_clear_x1 = 308;
  slot->sim.cloud_clear_y0 = 0; slot->sim.cloud_clear_y1 = 224;
  slot->sim.cloud_inset_px = 24; slot->sim.cloud_falloff_px = 96;
  slot->sim.cloud_drift_pct = 100;
  slot->sim.world_navigation_clouds = slot->sim.world_navigation_cloud_shadows = false;
  slot->sim.world_navigation_atmosphere = slot->sim.world_navigation_backdrop = false;
  CHECK(SimBackgroundVoxelRenderer_Ready(slot->sim.background_voxel_serial));
  const ArRenderRectI source = {0,0,360,224};
  if (sim_height_sweep_requested) {
    CaptureLandscapeHeightSweep(renderer,slot,source);
    goto cleanup;
  }
  const struct { const char *name; int y; float distance; } views[] = {
    {"overview",144,4.5f},{"north-close",64,3.2f},{"south-edge",256,4.5f}};
  FrameSlot *unchanged = malloc(sizeof(*unchanged)); CHECK(unchanged);
  for (unsigned i = 0; i < sizeof(views)/sizeof(*views); ++i) {
    slot->sim.camera_x = 128; slot->sim.camera_y = views[i].y;
    *unchanged = *slot;
    Scene3DCamera camera = {-.75f,0,views[i].distance,.4f};
    for (int clouds = 0; clouds < 2; ++clouds) {
      weather_time_ms = 12000;
      if (!i && !clouds) SaveTownHeightProfiles(town);
      char name[128];
      SDL_Surface *globe = RenderDetailedTown(renderer,slot,&camera,source,clouds);
      snprintf(name,sizeof(name),"paired-town-%u-%s-%s-continuous",town,views[i].name,clouds?"clouds":"clear");
      SaveImage(globe,name);
      for (int repeat = 0; repeat < 4; ++repeat) {
        SDL_Surface *held = RenderDetailedTown(renderer,slot,&camera,source,clouds);
        CHECK(Differences(globe,held) == 0); SDL_DestroySurface(held);
      }
      CHECK(!memcmp(slot,unchanged,sizeof(*slot)));
      SDL_DestroySurface(globe);
    }
  }
  TestDetailedMountainReuse(renderer,slot,source);
  TestLiveCraterComposition(renderer,slot,source);
  TestDetailedCameraLimits(renderer,slot,source);
  /* Synchronous camera/cloud sweep from an explicit clock and camera, with
   * simulation and individual model poses
   * frozen. This is a presentation clip, not recorded gameplay. */
  for (unsigned frame = 0; frame < 24; ++frame) {
    const float phase = frame * (6.28318530718f/24);
    slot->sim.camera_y = (uint16_t)(144 + lroundf(24*sinf(phase)));
    const Scene3DCamera camera = {-.75f,.20f*sinf(phase),4.2f+.3f*cosf(phase),.4f};
    weather_time_ms = 12000+frame*166;
    *unchanged = *slot;
    {
      SDL_Surface *frame_image = RenderDetailedTown(renderer,slot,&camera,source,true);
      char name[128];
      snprintf(name,sizeof(name),"paired-town-%u-motion-%02u-continuous",town,frame);
      SaveImage(frame_image,name); SDL_DestroySurface(frame_image);
    }
    CHECK(!memcmp(slot,unchanged,sizeof(*slot)));
  }
  free(unchanged);
cleanup:
  printf("SIM globe presentation: town=%u models=%u mountain-cells=%u radius=%ux navigation landscape=%u%% PASS\n",
      town,scene->object_count,scene->mountains.cell_count,sim_town_radius_scale,slot->sim.landscape_height_pct);
  PresentSim3DClouds_ResetResources();
  SimBackgroundVoxelRenderer_Reset(&g_render_device);
  SimBackgroundVoxels_Reset(); SimTownCanvas_Reset();
}

static void TestCaptured(SDL_Renderer *renderer, const char *rom_path, const char *wram_path) {
  uint8_t *rom = ReadFile(rom_path, kRomBytes), *wram = ReadFile(wram_path, kWramBytes);
  CHECK(wram[0x18] == 0 && (sim_town_snapshot || wram[0x19] == 9));
  CHECK(SimWorldMap_Init(rom, kRomBytes) && SimTownGroundArt_Init(rom, kRomBytes));
  if (!sim_town_snapshot) SimWorldMap_PublishBuiltTilemap(wram + 0xC000);
  SimWorldMap_SetWaterAnimationSource((uint16_t)(wram[0xD7] | wram[0xD8] << 8));
  FrameSlot *slot = malloc(sizeof(*slot));
  CHECK(slot);
  InitSlot(slot);
  SimWorldNavigationTowns_Capture(wram, &slot->sim.world_navigation_towns);
  CHECK(slot->sim.world_navigation_towns.object_count > 0 && !slot->sim.world_navigation_towns.overflow);
  printf("captured towns mask=%02x objects=%u\n", slot->sim.world_navigation_towns.enabled_town_mask,
      slot->sim.world_navigation_towns.object_count);
  slot->sim.world_navigation.focus_x = 768; slot->sim.world_navigation.focus_y = 512;
  slot->sim.world_navigation.active_location = 1;
  slot->sim.projection_pitch_mrad = -575;
  slot->sim.world_navigation_models = slot->sim.world_navigation_mountains = true;
  slot->sim.world_navigation_ground_detail = slot->sim.world_navigation_lighting = true;
  slot->sim.world_navigation_clouds = slot->sim.world_navigation_cloud_shadows = true;
  slot->sim.world_navigation_atmosphere = slot->sim.world_navigation_backdrop = true;
  BuildScene(slot);
  if (sim_town_snapshot) {
    CaptureTownPresentation(renderer,slot,rom,wram);
    goto cleanup;
  }
  if (sim_globe_prototype_requested) {
    CaptureSimGlobePrototype(renderer,slot);
    goto cleanup;
  }
  TestCapturedMarahnaSanctuary(renderer, slot);
  TestCapturedSimGlobe(renderer, slot);
  TestMountainFitViews(renderer, slot);
  TestGroundLightDirections(renderer, slot, "captured");
  TestColdBlackEntry(renderer, slot);
  TestVolcanoCapViews(renderer, slot);
  const SimWorldNavigationFrame navigation = slot->sim.world_navigation;
  TestAdventClearance(renderer, slot);
  SDL_Surface *front = Render(renderer, slot, "captured-front");
  const struct { const char *name; float yaw, pitch; } views[] = {
    {"captured-centred-front", 0, 0},
    {"captured-east", 1.57079632679f, 0}, {"captured-back", 3.14159265359f, 0},
    {"captured-west", -1.57079632679f, 0}, {"captured-north", 0, 1.57079632679f},
    {"captured-south", 0, -1.57079632679f}, {"captured-tilted", .8f, .6f},
  };
  for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
    slot->sim_manual_orbit_yaw = views[i].yaw;
    slot->sim_manual_orbit_pitch = views[i].pitch;
    SDL_Surface *view = Render(renderer, slot, views[i].name);
    CHECK(i ? Differences(front, view) > 1000 : Differences(front, view) == 0);
    SDL_DestroySurface(view);
  }
  slot->sim_manual_orbit_yaw = slot->sim_manual_orbit_pitch = 0;
  SDL_Surface *restored = Render(renderer, slot, "captured-restored");
  CHECK(Differences(front, restored) == 0);
  CHECK(!memcmp(&navigation, &slot->sim.world_navigation, sizeof(navigation)));
  SDL_DestroySurface(restored);
  SDL_DestroySurface(front);
  TestWeatherMotion(renderer, slot, "captured", false);
  if (weather_sequence_requested) CaptureWeatherSequence(renderer, slot);
  if (town_matrix_requested) CaptureTownAcceptanceMatrix(renderer, slot);
  TestSkyPalace(renderer, slot);
cleanup:
  free(slot); free(wram); free(rom);
  PresentWorldNav_ResetResources();
  Sim3DDepthPass_Reset(&g_render_device);
  SimBackgroundVoxelModelCache_Reset();
  SimTownGroundArt_Shutdown();
  SimWorldMap_Shutdown();
}

#if AR_SIM3D_TERRAIN_ELEVATION
typedef struct TerrainSourceAudit {
  uint8_t town;
  SimGlobeMapping map;
  unsigned tops, skirts, calls;
  bool stop;
} TerrainSourceAudit;

static bool AuditTerrainSource(void *user, const float xy[4][2],
    const float height[4], const float uv[4][2], const float shade[4]) {
  TerrainSourceAudit *a = user;
  ++a->calls;
  if (a->stop) return false;
  const bool top = xy[1][0] > xy[0][0] && xy[3][1] > xy[0][1];
  if (top) ++a->tops; else ++a->skirts;
  for (int p = 0; p < 4; ++p) {
    CHECK(isfinite(height[p]) && isfinite(shade[p]) && shade[p] > 0 && shade[p] <= 1);
    for (int axis = 0; axis < 2; ++axis) {
      CHECK(xy[p][axis] >= 0 && xy[p][axis] <= 512);
      CHECK(uv[p][axis] > 0 && uv[p][axis] < 1);
    }
    if (top) {
      const int cx = (int)xy[0][0]/16, cy = (int)xy[0][1]/16;
      CHECK(height[p] == SimTownTerrain_CornerUnits(a->town,cx,cy,p));
      /* The source exports unscaled height, and registration substitutes it
       * for this town's sampled floor. Neither stage may stack another copy
       * of town relief, even when a non-100% presentation scale is selected. */
      const float x = xy[p][0]/16, y = xy[p][1]/16;
      float floor, point[3], normal[3], encoded[3], elevation[2];
      CHECK(SimWorldNavigationTerrain_RegisterTownFloor(a->town,x,y,height[p],&floor));
      if (cx >= 4 && cx < 28 && cy >= 4 && cy < 28) {
        const float datum[] = {0,3,4,4,0,4};
        CHECK(fabsf(floor-height[p]-datum[a->town-1]) < .00001f);
      }
      const SimGlobeMapping *map = &a->map;
      CHECK(SimGlobeMapping_Point(map,map->origin_x+x,map->origin_y+y,floor,0,point));
      CHECK(SimGlobeMapping_Encode(map,map->origin_x+x,map->origin_y+y,floor,0,encoded,elevation));
      CHECK(SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius,
          map->origin_x+x,map->origin_y+y,normal,NULL));
      SimWorldNavigationGlobe_TransformNormal(&map->frame,normal,normal);
      const float radius = map->radius+floor*map->landscape/map->metric;
      const float epsilon = 8*FLT_EPSILON*(map->radius+16);
      for (int axis = 0; axis < 3; ++axis) {
        const float offset = axis == 2
            ? map->radius+map->reference_height*map->landscape/map->metric : 0;
        CHECK(fabsf(point[axis]-(normal[axis]*radius-offset)) < epsilon);
        CHECK(fabsf(encoded[axis]*(map->radius+elevation[0])-
            (axis == 2 ? map->radius : 0)-point[axis]) < epsilon);
      }
      const float inset_x = p == 1 || p == 2 ? -.5f : .5f;
      const float inset_y = p >= 2 ? -.5f : .5f;
      CHECK(uv[p][0] == (xy[p][0]+inset_x)/512);
      CHECK(uv[p][1] == (xy[p][1]+inset_y)/512);
    }
  }
  return true;
}

static void TestTerrainSource(void) {
  for (uint8_t town = 1; town <= 6; ++town) for (int scale = 0; scale <= 150; scale += 25) {
    TerrainSourceAudit a = {.town=town};
    int ox,oy; CHECK(SimWorldMap_OriginForTown(town,&ox,&oy));
    CHECK(SimGlobeMapping_Build(town,ox,oy,192,1.5f,scale/100.0f,&a.map));
    CHECK(EmitSimTownTerrainSource(town,scale,AuditTerrainSource,&a));
    CHECK(a.tops == 1024 && a.skirts <= 4096 && a.calls == a.tops+a.skirts);
  }
  TerrainSourceAudit a = {.town=4,.stop=true};
  CHECK(!EmitSimTownTerrainSource(4,100,AuditTerrainSource,&a) && a.calls == 1);
  CHECK(!EmitSimTownTerrainSource(0,100,AuditTerrainSource,&a));
  CHECK(!EmitSimTownTerrainSource(7,100,AuditTerrainSource,&a));
  CHECK(!EmitSimTownTerrainSource(4,151,AuditTerrainSource,&a));
  CHECK(!EmitSimTownTerrainSource(4,100,NULL,&a));
  CHECK(a.calls == 1);
  puts("terrain source: six towns, seven scales; unscaled source, single registered elevation, single scale, GPU radial round trip PASS");
}
#endif

int main(int argc, char **argv) {
  if (argc != 1 && (argc < 4 || argc > 10)) {
    fprintf(stderr, "usage: %s [ROM WRAM existing-output-directory [--weather-sequence] [--town-matrix] [--sim-globe-prototype] | --sim-town SIM-snapshot-prefix [--sim-height-sweep | --sim-landscape-height 0..150] [--sim-radius-scale 1..4]]\n", argv[0]);
    return 1;
  }
  if (argc >= 4) output_directory = argv[3];
  bool radius_requested = false;
  bool landscape_requested = false;
  for (int arg = 4; arg < argc; arg++) {
    if (!strcmp(argv[arg], "--weather-sequence")) weather_sequence_requested = true;
    else if (!strcmp(argv[arg], "--town-matrix")) town_matrix_requested = true;
    else if (!strcmp(argv[arg], "--sim-globe-prototype")) sim_globe_prototype_requested = true;
    else if (!strcmp(argv[arg], "--sim-town") && arg+1 < argc) sim_town_snapshot = argv[++arg];
    else if (!strcmp(argv[arg], "--sim-height-sweep")) sim_height_sweep_requested = true;
    else if (!strcmp(argv[arg], "--sim-landscape-height") && arg+1 < argc) {
      const char *value = argv[++arg];
      char *end;
      const long height = strtol(value,&end,10);
      if (end == value || *end || height < kSimTownTerrainLandscapeHeightMinimumPct ||
          height > kSimTownTerrainLandscapeHeightMaximumPct) {
        fprintf(stderr,"--sim-landscape-height requires a whole percentage from 0 to 150\n");
        return 1;
      }
      sim_town_landscape_pct = (unsigned)height;
      landscape_requested = true;
    }
    else if (!strcmp(argv[arg], "--sim-radius-scale") && arg+1 < argc) {
      const char *value = argv[++arg];
      if (value[0] < '1' || value[0] > '4' || value[1]) return 1;
      sim_town_radius_scale = (unsigned)(value[0]-'0');
      radius_requested = true;
    }
    else { fprintf(stderr, "unknown option: %s\n", argv[arg]); return 1; }
  }
  if (sim_globe_prototype_requested && (weather_sequence_requested || town_matrix_requested)) {
    fprintf(stderr,"--sim-globe-prototype is a separate scene-only capture mode\n");
    return 1;
  }
  if (sim_town_snapshot && (sim_globe_prototype_requested || weather_sequence_requested || town_matrix_requested)) return 1;
  if ((sim_height_sweep_requested || radius_requested || landscape_requested) && !sim_town_snapshot) return 1;
  if (sim_height_sweep_requested && landscape_requested) return 1;
#if AR_SIM3D_TERRAIN_ELEVATION
  TestTerrainSource();
#endif
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "World navigation GPU test skipped: video unavailable: %s\n", SDL_GetError());
    return 77;
  }
  SDL_Window *window = SDL_CreateWindow("World navigation GPU test", kWidth, kHeight, SDL_WINDOW_HIDDEN);
  if (!window) {
    fprintf(stderr, "World navigation GPU test skipped: window unavailable: %s\n", SDL_GetError());
    SDL_Quit(); return 77;
  }
  SDL_Renderer *renderer = NULL;
  ArSdlRenderBackend backend = {0};
  if (sim_town_snapshot) {
    /* The detailed ground borrows a streaming SDL texture in a custom GPU
     * pass. Use the game's ordered adapter so its first upload is submitted
     * before consumption; a legacy Flush-only binding needs a prior present
     * and would hide cold-entry problems behind reference-view warmup. */
    SDL_unsetenv_unsafe("AR_SDL_GPU_ORDERED");
    if (ArSdlRenderBackend_CreateForWindow(&g_render_device,window))
      renderer = ArSdlRenderBackend_Renderer(&g_render_device);
  } else {
    SDL_PropertiesID properties = SDL_CreateProperties();
    CHECK(properties);
    SDL_SetStringProperty(properties, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
    SDL_SetPointerProperty(properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetBooleanProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
    renderer = SDL_CreateRendererWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (renderer) CHECK(ArSdlRenderBackend_Bind(&g_render_device,&backend,renderer));
  }
  if (!renderer) {
    fprintf(stderr, "World navigation GPU test skipped: %s\n", SDL_GetError());
    SDL_DestroyWindow(window); SDL_Quit(); return 77;
  }
  printf("world navigation renderer=%s gpu=%s\n", SDL_GetRendererName(renderer),
      SDL_GetGPUDeviceDriver(SDL_GetGPURendererDevice(renderer)));
  /* The ROM-free suite remains the default CTest entry. A frozen SIM capture
   * is independent and can be iterated without re-running orbital weather. */
  if (!sim_town_snapshot) TestSynthetic(renderer);
  if (argc >= 4) TestCaptured(renderer, argv[1], argv[2]);
  if (sim_town_snapshot) ArSdlRenderBackend_Destroy(&g_render_device);
  else SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  puts("present_world_nav_gpu_test: PASS");
  return 0;
}
