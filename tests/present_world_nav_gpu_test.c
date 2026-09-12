/* Frozen-scene integration test: production presenter, model compiler,
 * atlases, SDL GPU shaders and shared D32 depth. No runner, live input,
 * settings persistence or save writes. Optional ROM/WRAM inputs are read-only. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/render_sdl_internal.h"
#include "present_internal.h"
#include "present_sim3d_internal.h"
#include "render/render_output.h"
#include "render/localized_text_presenter.h"
#include "settings.h"
#include "performance_metrics.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim_town_ground_art.h"
#include "sim/sim_background_voxel_model_cache.h"

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
    Sim3DDepthMesh *pressure[4] = {0};
    if (retained == 3) {
      CHECK(Sim3DDepthPass_Begin(&g_render_device, 1792, 1344, kArRenderFilter_Nearest));
      for (int i = 0; i < 4; ++i) {
        pressure[i] = Sim3DDepthPass_CreateGeometryMesh();
        CHECK(pressure[i]);
      }
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
    for (int i = 0; i < 4; ++i) Sim3DDepthPass_DestroyMesh(pressure[i]);
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

static void TestSynthetic(SDL_Renderer *renderer) {
  uint8_t *rom = calloc(kRomBytes, 1);
  CHECK(rom);
  /* A green central continent in blue ocean. Only public immutable decoder
   * inputs are generated: no copyrighted fixture or renderer-only texture. */
  rom[0xE3F93 + 1] = 0x60;
  rom[0xE3F93 + 0x10 * 2 + 1] = 0x60;
  memset(rom + 0x70000, 0x10, 64);
  memset(rom + 0x70000 + 0xAA * 64, 0x10, 64);
  memset(rom + 0x53000, 0x10, 4 * 64);
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
  TestAdventClearance(renderer, slot);
  TestTallModelViewport(renderer, slot);
  free(slot);
  PresentWorldNav_ResetResources();
  Sim3DDepthPass_Reset(&g_render_device);
  SimBackgroundVoxelModelCache_Reset();
  SimWorldMap_Shutdown();
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

static void TestCaptured(SDL_Renderer *renderer, const char *rom_path, const char *wram_path) {
  uint8_t *rom = ReadFile(rom_path, kRomBytes), *wram = ReadFile(wram_path, kWramBytes);
  CHECK(wram[0x18] == 0 && wram[0x19] == 9); /* Navigation dump, not shared action scratch. */
  CHECK(SimWorldMap_Init(rom, kRomBytes) && SimTownGroundArt_Init(rom, kRomBytes));
  SimWorldMap_PublishBuiltTilemap(wram + 0xC000);
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
  TestCapturedMarahnaSanctuary(renderer, slot);
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
  free(slot); free(wram); free(rom);
  PresentWorldNav_ResetResources();
  Sim3DDepthPass_Reset(&g_render_device);
  SimBackgroundVoxelModelCache_Reset();
  SimTownGroundArt_Shutdown();
  SimWorldMap_Shutdown();
}

int main(int argc, char **argv) {
  if (argc != 1 && (argc < 4 || argc > 6)) {
    fprintf(stderr, "usage: %s [ROM navigation-WRAM existing-output-directory [--weather-sequence] [--town-matrix]]\n", argv[0]);
    return 1;
  }
  if (argc >= 4) output_directory = argv[3];
  for (int arg = 4; arg < argc; arg++) {
    if (!strcmp(argv[arg], "--weather-sequence")) weather_sequence_requested = true;
    else if (!strcmp(argv[arg], "--town-matrix")) town_matrix_requested = true;
    else { fprintf(stderr, "unknown option: %s\n", argv[arg]); return 1; }
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "World navigation GPU test skipped: video unavailable: %s\n", SDL_GetError());
    return 77;
  }
  SDL_Window *window = SDL_CreateWindow("World navigation GPU test", kWidth, kHeight, SDL_WINDOW_HIDDEN);
  if (!window) {
    fprintf(stderr, "World navigation GPU test skipped: window unavailable: %s\n", SDL_GetError());
    SDL_Quit(); return 77;
  }
  SDL_PropertiesID properties = SDL_CreateProperties();
  CHECK(properties);
  SDL_SetStringProperty(properties, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
  SDL_SetPointerProperty(properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
  SDL_SetBooleanProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
  SDL_SetBooleanProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
  SDL_SetBooleanProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
  SDL_Renderer *renderer = SDL_CreateRendererWithProperties(properties);
  SDL_DestroyProperties(properties);
  if (!renderer) {
    fprintf(stderr, "World navigation GPU test skipped: %s\n", SDL_GetError());
    SDL_DestroyWindow(window); SDL_Quit(); return 77;
  }
  printf("world navigation renderer=%s gpu=%s\n", SDL_GetRendererName(renderer),
      SDL_GetGPUDeviceDriver(SDL_GetGPURendererDevice(renderer)));
  ArSdlRenderBackend backend = {0};
  CHECK(ArSdlRenderBackend_Bind(&g_render_device, &backend, renderer));
  TestSynthetic(renderer);
  if (argc >= 4) TestCaptured(renderer, argv[1], argv[2]);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  puts("present_world_nav_gpu_test: PASS");
  return 0;
}
