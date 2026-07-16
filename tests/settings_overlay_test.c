#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "settings_overlay.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

bool g_ws_active;
int g_ws_extra;
uint8 g_ram[0x20000];

static int s_failures;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static uint8_t *ReadOptionalRom(size_t *size_out) {
  const char *path = getenv("AR_OVERLAY_TEST_ROM");
  *size_out = 0;
  if (!path || !path[0]) return NULL;
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  uint8_t *data = (uint8_t *)malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size_out = (size_t)size;
  return data;
}

int main(void) {
  remove("settings.ini");
  remove("settings.ini.tmp");
  setenv("SDL_VIDEODRIVER", "dummy", 1);
  setenv("SDL_AUDIODRIVER", "dummy", 1);

  g_ws_active = true;
  g_ws_extra = 43;
  Settings_ClearConfigLayer();
  Settings_Init();

  int surface_width = 640;
  int surface_height = 480;
  const char *preview_size = getenv("AR_OVERLAY_TEST_SIZE");
  if (preview_size)
    (void)sscanf(preview_size, "%dx%d", &surface_width, &surface_height);
  if (surface_width <= 0) surface_width = 640;
  if (surface_height <= 0) surface_height = 480;

  CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
      0, surface_width, surface_height, 32, SDL_PIXELFORMAT_ARGB8888);
  CHECK(surface != NULL);
  SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
  CHECK(renderer != NULL);
  size_t rom_size = 0;
  uint8_t *rom_data = ReadOptionalRom(&rom_size);
  CHECK(SettingsOverlay_Init(renderer, rom_data, rom_size));
  free(rom_data);

  SettingsOverlay_Open();
  CHECK(SettingsOverlay_IsOpen());
  if (renderer) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    const char *preview = getenv("AR_OVERLAY_TEST_BMP");
    if (preview && preview[0]) CHECK(SDL_SaveBMP(surface, preview) == 0);
  }

  /* Display row order begins profile, HUD scale, menu scale. Verify that the
   * descriptor-driven input path reaches the independent scale setting and
   * persists through the same accepted-change path used in the game. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  int auto_x = surface_width * 100 / 464;
  int auto_y = surface_height * 100 / 208;
  int auto_scale = auto_x < auto_y ? auto_x : auto_y;
  auto_scale = auto_scale / 25 * 25;
  if (auto_scale < 25) auto_scale = 25;
  if (auto_scale > 800) auto_scale = 800;
  int expected_scale = auto_scale < 800 ? auto_scale + 25 : 800;
  CHECK(g_settings.menu_scale_percent == expected_scale);
  FILE *saved = fopen("settings.ini", "rb");
  CHECK(saved != NULL);
  if (saved) fclose(saved);

  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsOpen());

  SettingsOverlay_Destroy();
  SDL_DestroyRenderer(renderer);
  SDL_FreeSurface(surface);
  SDL_Quit();
  remove("settings.ini");
  remove("settings.ini.tmp");

  if (s_failures) {
    fprintf(stderr, "settings overlay tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings overlay tests: pass\n");
  return 0;
}
