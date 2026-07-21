#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "settings_overlay.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool g_ws_active;
int g_ws_extra;
int g_ws_display_extra;
uint8 g_ram[0x20000];
/* Host-side diorama geometry rebind; no renderer in this harness. */
void Diorama_OnModeChanged(void) {}

static int s_failures;
static int s_action_calls;
static const SettingDesc *s_action_desc;
static int s_inspector_info_calls;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static bool ActionObserved(const SettingDesc *desc) {
  s_action_calls++;
  s_action_desc = desc;
  return true;
}

static void InspectorInfo(char *buffer, size_t buffer_size) {
  s_inspector_info_calls++;
  snprintf(buffer, buffer_size,
           "SCENE Fillmore sim  $18/$19 $00/$01\n"
           "GF $1234  HOST 5678  PAUSE MENU\n"
           "CAM $0080,$0040  MAP 512X512\n"
           "PPU MODE 1  MAIN $17 SUB $00\n"
           "MUSIC Fillmore  SONG $01 AUTH");
}

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
  char settings_path[160];
  char settings_temporary[164];
  snprintf(settings_path, sizeof(settings_path),
           "/tmp/actraiser-overlay-settings-%ld.ini", (long)getpid());
  snprintf(settings_temporary, sizeof(settings_temporary), "%s.tmp",
           settings_path);
  setenv("AR_OVERLAY_TEST_SETTINGS_PATH", settings_path, 1);
  remove(settings_path);
  remove(settings_temporary);
  setenv("SDL_VIDEODRIVER", "dummy", 1);
  setenv("SDL_AUDIODRIVER", "dummy", 1);

  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_ClearConfigLayer();
  Settings_Init();
  Settings_SetActionObserver(ActionObserved);

  int surface_width = 640;
  int surface_height = 480;
  const char *preview_size = getenv("AR_OVERLAY_TEST_SIZE");
  if (preview_size)
    (void)sscanf(preview_size, "%dx%d", &surface_width, &surface_height);
  if (surface_width <= 0) surface_width = 640;
  if (surface_height <= 0) surface_height = 480;

  CHECK(SDL_Init(SDL_INIT_VIDEO));
  SDL_Surface *surface = SDL_CreateSurface(
      surface_width, surface_height, SDL_PIXELFORMAT_ARGB8888);
  CHECK(surface != NULL);
  SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
  CHECK(renderer != NULL);
  size_t rom_size = 0;
  uint8_t *rom_data = ReadOptionalRom(&rom_size);
  CHECK(SettingsOverlay_Init(renderer, rom_data, rom_size));
  SettingsOverlay_SetInspectorInfoProvider(InspectorInfo);
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
    if (preview && preview[0]) CHECK(SDL_SaveBMP(surface, preview));
  }

  /* The overlay opens on primary navigation. B enters Display; only then do
   * Up/Down select rows and Left/Right edit values. */
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
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
  FILE *saved = fopen(settings_path, "rb");
  CHECK(saved != NULL);
  if (saved) fclose(saved);

  /* Aspect rows are part of Display. Move past HD replacements to Screen
   * ratio and verify it remains a bounded enum. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_169);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_1610);

  /* Audio frequency is likewise a bounded preset selector, not an arbitrary
   * integer editor. Audio starts with Enable audio, then Audio frequency. */
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(SettingsOverlay_IsOpen());
  /* Presentation sits between Display and Audio in kCategoryOrder. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_AudioFrequencyHz() == 48000);

  /* Save Editor follows Cheats. Its panel title names the active edit
   * section, while backend/arming stay global rows. */
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.save_backend == 1);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.save_edit_armed);
  CHECK(Settings_SetText(Settings_Find("save_prog_fillmore"),
                         "act2-cleared") == kSettingChange_Applied);
  const char *save_preview = getenv("AR_OVERLAY_SAVE_TEST_BMP");
  if (renderer && save_preview && save_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, save_preview));
  }

  /* The default Progress page is taller than the visible panel. Verify its
   * filtered row list reaches the final conversion action and dispatches it
   * through the normal observer path. We started on row 1 (Allow save edits). */
  for (int i = 0; i < 15; i++)
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("save_export_ini"));
  s_action_calls = 0;
  s_action_desc = NULL;
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Move once more to Extras. Bridge-free limit is first, followed by Turbo
   * multiplier; warp and quick-state rows remain hidden. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.fix_bridge_limit);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("toggle_pause"));

  /* Inspector is a real submenu: its first row makes the enabled state
   * explicit, its second row dispatches the complete scene-asset dump, and
   * the remainder is supplied by the read-only live-info provider. */
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.scene_inspector);
  if (renderer) {
    int calls_before = s_inspector_info_calls;
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    CHECK(s_inspector_info_calls == calls_before + 1);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 2);
  CHECK(s_action_desc == Settings_Find("dump_scene_assets"));

  /* Restart and Exit remain direct primary-navigation leaves. */
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 3);
  CHECK(s_action_desc == Settings_Find("restart_game"));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 4);
  CHECK(s_action_desc == Settings_Find("exit_desktop"));

  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(!SettingsOverlay_IsOpen());
  SettingsOverlay_Open();
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsOpen());

  /* Debug panels avoid the inspected point and can be moved without a click
   * falling through to the tool beneath them. */
  if (renderer) {
    SDL_SetRenderDrawColor(renderer, 22, 28, 34, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_RenderDebugPanel(
        "SCENE INSPECTOR",
        "CLICK 474,170  WORLD $008E,$00C6\n"
        "GF $016C STATE $00/$00 CAM $0080,$0080 MAP $0000,$0000\n"
        "PPU MODE 7 BRIGHT 15 MAIN $01 SUB $00 MARGIN 0/0\n"
        "BG1 T$03A P1 PAL3 PIX2 CENTER MAP$7104\n"
        "OBJ#12 16X16 BASE$80 SUB$91 PAL4 PRI2 PIX7\n"
        "CANDIDATES; WINDOWS/COLOR MATH MAY MASK A LAYER\n"
        "LEFT CLICK INSPECT  RIGHT CLICK CLEAR  F3 DISABLE",
        (SDL_Point){ surface_width / 2, surface_height - 1 });
    SDL_RenderPresent(renderer);
    const char *debug_preview = getenv("AR_OVERLAY_DEBUG_TEST_BMP");
    if (debug_preview && debug_preview[0])
      CHECK(SDL_SaveBMP(surface, debug_preview));
    SDL_Rect panel_before = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_before));
    CHECK(panel_before.y < surface_height / 2);
    CHECK(panel_before.w < surface_width - 40);
    CHECK(!SettingsOverlay_BeginDebugPanelDrag(
        panel_before.x + 4, panel_before.y + panel_before.h - 4));
    CHECK(SettingsOverlay_BeginDebugPanelDrag(
        panel_before.x + 4, panel_before.y + 4));
    CHECK(SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_DragDebugPanel(
        panel_before.x + 4, surface_height / 2);
    SettingsOverlay_EndDebugPanelDrag();
    CHECK(!SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_RenderDebugPanel(
        "DEBUG", "FIRST LINE\nSECOND LINE",
        (SDL_Point){ surface_width / 2, surface_height - 1 });
    SDL_Rect panel_after = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_after));
    CHECK(panel_after.y != panel_before.y);
    CHECK(SettingsOverlay_BeginDebugPanelDrag(
        panel_after.x + panel_after.w - 4,
        panel_after.y + panel_after.h - 4));
    CHECK(SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_DragDebugPanel(
        panel_after.x + panel_after.w - 4 - panel_after.w / 4,
        panel_after.y + panel_after.h - 4 - panel_after.h / 4);
    SettingsOverlay_EndDebugPanelDrag();
    SettingsOverlay_RenderDebugPanel(
        "DEBUG", "FIRST LINE\nSECOND LINE",
        (SDL_Point){ surface_width / 2, surface_height - 1 });
    SDL_Rect panel_resized = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_resized));
    CHECK(panel_resized.w < panel_after.w);
    CHECK(panel_resized.h < panel_after.h);
    SettingsOverlay_HideDebugPanel();
    CHECK(!SettingsOverlay_GetDebugPanelRect(&panel_resized));
    CHECK(!SettingsOverlay_BeginDebugPanelDrag(0, 0));
  }

  SettingsOverlay_Destroy();
  Settings_SetActionObserver(NULL);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  SDL_Quit();
  remove(settings_path);
  remove(settings_temporary);

  if (s_failures) {
    fprintf(stderr, "settings overlay tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings overlay tests: pass\n");
  return 0;
}
