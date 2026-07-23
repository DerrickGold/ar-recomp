#define _POSIX_C_SOURCE 200809L

#include "input_map.h"
#include "settings.h"
#include "settings_overlay.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

bool g_ws_active;
int g_ws_extra;
int g_ws_display_extra;
uint8 g_ram[0x20000];
/* kSettingCat_Graphics's GpuShadersActive() availability gate reads this
 * (main.c's real runtime state); this harness has no renderer, so it's
 * never actually true here. */
bool g_gpu_shaders_active;
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

/* The overlay's nav column lists SECTIONS and each section has a tab bar, so
 * these walk to a named destination instead of counting keypresses — the old
 * "press Down four times" style broke every time a row landed above the one
 * under test. */
enum {
  kSection_Video = 0,
  kSection_Diorama,
  kSection_Town3D,
  kSection_Audio,
  kSection_Controls,
  kSection_Cheats,
  kSection_Save,
  kSection_System,
};

/* Call from the nav column (not inside a submenu). */
static void NavToSection(int target) {
  for (int guard = 0; guard < 24; guard++) {
    int selected = -1;
    CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
    if (selected == target) return;
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  }
  CHECK(!"section not reachable");
}

/* Tabs are remembered per section, so step forward (wrapping) until the
 * wanted one is active rather than assuming we start at zero. */
static void NavToTab(int target) {
  for (int guard = 0; guard < 12; guard++) {
    int active = -1;
    CHECK(SettingsOverlay_GetTabState(&active, NULL));
    if (active == target) return;
    /* ']' is the layout-independent next-tab key; the primary L/R keys follow
     * the player's own SNES bindings and are exercised separately below. */
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHTBRACKET, true, false));
  }
  CHECK(!"tab not reachable");
}

static void RowToKey(const char *key) {
  for (int guard = 0; guard < 80; guard++) {
    if (!strcmp(SettingsOverlay_SelectedKey(), key)) return;
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  }
  CHECK(!"row not reachable");
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
  /* Most of this test drives every tab and row, including the developer-only
   * ones (the town Light/Weather dials, the inspector). Turn debug settings on
   * so they are all present; a dedicated block below toggles it back off and
   * checks that they collapse. */
  g_settings.show_debug_settings = true;

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

  /* Headless SDL reports no refresh rate; let a preview inject one so the
   * "Vsync NHz" row can be eyeballed. */
  const char *refresh_hz = getenv("AR_OVERLAY_TEST_REFRESH_HZ");
  if (refresh_hz && refresh_hz[0]) Settings_SetHostRefreshHz(atoi(refresh_hz));

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
    /* Scroll to the Refresh rate row and capture it to eyeball the Hz label. */
    const char *rpreview = getenv("AR_OVERLAY_TEST_REFRESH_BMP");
    if (rpreview && rpreview[0]) {
      NavToSection(kSection_Video);
      NavToTab(0);
      CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      RowToKey("refresh_mode");
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      SDL_RenderClear(renderer);
      SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
      SDL_RenderPresent(renderer);
      CHECK(SDL_SaveBMP(surface, rpreview));
      CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
    }
  }

  /* Tab cycling follows the player's OWN SNES L/R keyboard bindings, not
   * hardcoded keys. With the defaults (L=Q, R=W) both directions must work
   * from the nav column — the R half of this regressed once because the
   * overlay guessed Q/E instead of consulting the bindings. */
  NavToSection(kSection_Video);
  {
    int start = -1, count = -1;
    CHECK(SettingsOverlay_GetTabState(&start, &count));
    CHECK(count >= 2);
    CHECK(SettingsOverlay_HandleKey(SDLK_W, true, false));  /* SNES R */
    int after_r = -1;
    CHECK(SettingsOverlay_GetTabState(&after_r, NULL));
    CHECK(after_r == (start + 1) % count);
    CHECK(SettingsOverlay_HandleKey(SDLK_Q, true, false));  /* SNES L */
    int after_l = -1;
    CHECK(SettingsOverlay_GetTabState(&after_l, NULL));
    CHECK(after_l == start);
  }

  /* The overlay opens on primary navigation. B enters the selected section;
   * only then do Up/Down select rows and Left/Right edit values. */
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "menu_scale_percent"));
  /* menu_scale is an Int row: the press applies the step live, and releasing
   * the key flushes the deferred settings.ini write (numeric rows no longer
   * open a text editor). */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  int auto_x = surface_width * 100 / 464;
  int auto_y = surface_height * 100 / 208;
  int auto_scale = auto_x < auto_y ? auto_x : auto_y;
  auto_scale = auto_scale / 25 * 25;
  if (auto_scale < 25) auto_scale = 25;
  if (auto_scale > 800) auto_scale = 800;
  int expected_scale = auto_scale < 800 ? auto_scale + 25 : 800;
  CHECK(g_settings.menu_scale_percent == expected_scale);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));  /* release flushes */
  FILE *saved = fopen(settings_path, "rb");
  CHECK(saved != NULL);
  if (saved) fclose(saved);

  /* Aspect rows share the Video section's General tab. */
  RowToKey("extended_aspect");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_169);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_1610);

  /* Widescreen is now a TAB of Video rather than its own nav entry, and
   * switching tabs must swap the row list without leaving the section. */
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "ws_action"));
  NavToTab(0);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "display_mode"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Drive the Town 3D section through the same key path a user does. This
   * guards both the master toggle and the A/B stage selectors against
   * becoming display-only rows. */
  NavToSection(kSection_Town3D);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_mode"));
  CHECK(!g_settings.sim3d_mode);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.sim3d_mode);
  /* Walk to a stage toggle by key rather than counting rows: the stage list
   * grows every time a render stage lands. Toggling one from the menu must
   * also change what the renderer is asked for, since the fold is the only
   * thing standing between these rows and the frame payload. */
  RowToKey("sim3d_shadows");
  CHECK(g_settings.sim3d_shadows);
  CHECK(Settings_Sim3DRequestedFeatures() & kSimFeature_Shadows);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!g_settings.sim3d_shadows);
  CHECK(!(Settings_Sim3DRequestedFeatures() & kSimFeature_Shadows));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.sim3d_shadows);
  /* The camera/light/weather splits are separate tabs of the same section. */
  NavToTab(1);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_camera_mode"));
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_shadow_opacity_pct"));
  NavToTab(3);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_underlay_haze_pct"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Audio frequency is a bounded preset selector, not an arbitrary integer
   * editor. Audio starts with Enable audio, then Audio frequency. */
  NavToSection(kSection_Audio);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("audio_frequency");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_AudioFrequencyHz() == 48000);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Hold-to-accelerate lives on Camera sensitivity: a wide 10..400 Int with a
   * base step of 1, so the ramp is actually visible (unlike a 0..100 row whose
   * coarse step would equal its base). A tap steps by one and — proven by the
   * value moving at all — never opens a text editor (BeginEditing would leave
   * the value untouched). Holding then releasing flushes one deferred save. */
  NavToSection(kSection_Controls);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  const SettingDesc *sens = Settings_Find("input_cam_sensitivity");
  CHECK(sens && sens->type == kSettingType_Int && sens->step == 1);
  int sens_default = g_settings.input_cam_sensitivity;
  CHECK(Settings_SetLong(sens, 150) >= kSettingChange_Applied);  /* != default */
  RowToKey("input_cam_sensitivity");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));   /* tap down */
  CHECK(g_settings.input_cam_sensitivity == 149);  /* stepped, not text-edited */
  CHECK(!SettingsOverlay_IsEditing());            /* numeric never opens a field */
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, false, false));  /* release */
  /* Confirm (B) on a numeric row is a single fine step up, still no editor. */
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.input_cam_sensitivity == 150);
  CHECK(!SettingsOverlay_IsEditing());

  /* The pure ramp: a fresh hold moves one base step, a long hold moves more. */
  CHECK(SettingsOverlay_HoldStepForTest(sens, 0) == 1);
  CHECK(SettingsOverlay_HoldStepForTest(sens, 4000) > 1);

  /* Drive the tick with an injected clock so acceleration is deterministic:
   * press up, let the initial delay pass, then repeats well past the ramp knee
   * move the value by far more than a tap — and it clamps to the range. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* press up */
  int after_press = g_settings.input_cam_sensitivity;
  CHECK(after_press == 151);  /* one base step up from 150 */
  uint64_t base = SDL_GetTicks();
  for (int i = 1; i <= 40; i++)
    SettingsOverlay_TickAtForTest(base + (uint64_t)i * 60);
  CHECK(g_settings.input_cam_sensitivity > after_press + 5);  /* accelerated */
  CHECK(g_settings.input_cam_sensitivity <= 400);  /* normalized to range */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));  /* release */
  CHECK(Settings_SetLong(sens, sens_default) >= kSettingChange_Applied);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* The genuine string holdouts still open a text field: pins are arbitrary
   * PAR codes. Confirm B enters editing, and Escape leaves it without a value
   * change — this is the one path numeric rows deliberately no longer use. */
  NavToSection(kSection_Cheats);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("pins");
  CHECK(Settings_Find("pins")->type == kSettingType_Custom);
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* B opens the field */
  CHECK(SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Controls: the Devices tab holds device/analog rows, and the Keyboard and
   * Gamepad tabs are the two binding pages — the tab now drives
   * input_bind_page, which no longer lists itself as a row. */
  NavToSection(kSection_Controls);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "input_device"));
  CHECK(!Settings_IsMenuVisible(Settings_Find("input_bind_page")));
  NavToTab(1);
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_key_up"));

  const char *controls_preview = getenv("AR_OVERLAY_CONTROLS_TEST_BMP");
  if (renderer && controls_preview && controls_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, controls_preview));
  }

  /* Drive one full rebind the way a player would: select the row, arm
   * capture, and feed the raw SDL event — the capture path takes scancodes,
   * not keycodes, so it bypasses SettingsOverlay_HandleKey entirely. */
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  /* A gamepad event must not land in a keyboard row. */
  SDL_Event pad = {0};
  pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  pad.gbutton.button = SDL_GAMEPAD_BUTTON_NORTH;
  CHECK(SettingsOverlay_HandleCaptureEvent(&pad));
  CHECK(SettingsOverlay_IsCapturing());

  SDL_Event key = {0};
  key.type = SDL_EVENT_KEY_DOWN;
  key.key.scancode = SDL_SCANCODE_I;
  CHECK(SettingsOverlay_HandleCaptureEvent(&key));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_I, false));

  /* Escape aborts an armed row and leaves the old binding intact. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  key.key.scancode = SDL_SCANCODE_ESCAPE;
  CHECK(SettingsOverlay_HandleCaptureEvent(&key));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_I, false));
  /* Y restores the row default. */
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_UP, false));

  /* Stepping to the Gamepad tab swaps the listed rows to the gamepad set and
   * writes the page setting through, so a reopened menu agrees with it. */
  NavToTab(2);
  CHECK(g_settings.input_bind_page == kInputClass_Gamepad);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_pad_up"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  CHECK(SettingsOverlay_HandleCaptureEvent(&pad));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_NORTH,
                        false));
  NavToTab(1);
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* The Save section's tabs are the Actions page plus five editor pages. The
   * backend/arming controls and apply/export commands now live only on the
   * Actions tab instead of repeating on every page. */
  NavToSection(kSection_Save);
  NavToTab(kSaveEditorPage_Actions);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "save_backend"));
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

  /* Selecting a page tab writes the page setting through, and the row list
   * follows it. */
  NavToTab(kSaveEditorPage_Items);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Items);
  RowToKey("save_item_slot_1");
  NavToTab(kSaveEditorPage_Progress);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Progress);
  /* The export commands live on the Actions tab now; verify the observer path
   * still dispatches the final conversion action from there. */
  NavToTab(kSaveEditorPage_Actions);
  RowToKey("save_export_ini");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("save_export_ini"));
  s_action_calls = 0;
  s_action_desc = NULL;
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* System > Tools carries the debug-settings switch and the host commands
   * (pause, restart, exit); Restart and Exit are the last two rows of this
   * tab, no longer permanent nav-column slots. */
  NavToSection(kSection_System);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "show_debug_settings"));
  RowToKey("toggle_pause");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("toggle_pause"));
  RowToKey("restart_game");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 2);
  CHECK(s_action_desc == Settings_Find("restart_game"));
  RowToKey("exit_desktop");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 3);
  CHECK(s_action_desc == Settings_Find("exit_desktop"));

  /* System > Game holds the QoL gameplay enhancements moved off Tools. */
  NavToTab(1);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "fix_bridge_limit"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.fix_bridge_limit);

  /* Inspector is the section's third tab: its first row makes the enabled
   * state explicit, its second dispatches the complete scene-asset dump, and
   * the remainder is supplied by the read-only live-info provider. */
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "scene_inspector"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.scene_inspector);
  if (renderer) {
    int calls_before = s_inspector_info_calls;
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    CHECK(s_inspector_info_calls == calls_before + 1);
  }
  RowToKey("dump_scene_assets");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 4);
  CHECK(s_action_desc == Settings_Find("dump_scene_assets"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Debug-settings gate: with the switch on (set at startup) the town dials,
   * their A/B toggles, and the inspector are all visible. */
  const SettingDesc *debug_row = Settings_Find("show_debug_settings");
  CHECK(debug_row && !Settings_IsDebugOnly(debug_row));  /* never hides itself */
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(Settings_IsMenuVisible(Settings_Find("scene_inspector")));
  NavToSection(kSection_Town3D);
  {
    int tabs = -1;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 4);  /* Scene, Camera, Light, Weather */
  }

  /* Turn it off through the menu the way a player would (System > Tools). */
  NavToSection(kSection_System);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("show_debug_settings");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* on -> off */
  CHECK(!g_settings.show_debug_settings);

  /* The dials, internal A/B toggles, and inspector collapse out... */
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_separated_composite")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("diorama_layer_bg1")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("scene_inspector")));
  /* ...while master toggles, major on/off effects, and camera mode stay. */
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_mode")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_shadows")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_camera_mode")));
  CHECK(Settings_IsMenuVisible(Settings_Find("diorama_skybox")));
  /* System drops the all-debug Inspector tab, leaving Tools and Game. */
  {
    int tabs = -1;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 2);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));  /* back to nav column */
  /* Town 3D collapses to Scene + Camera; cycling never lands on a hidden tab. */
  NavToSection(kSection_Town3D);
  {
    int tabs = -1, active = -1;
    CHECK(SettingsOverlay_GetTabState(&active, &tabs));
    CHECK(tabs == 2);
    for (int i = 0; i < 6; i++) {
      CHECK(SettingsOverlay_HandleKey(SDLK_RIGHTBRACKET, true, false));
      CHECK(SettingsOverlay_GetTabState(&active, NULL));
      CHECK(active >= 0 && active < 2);
    }
  }
  /* Restore for the section-sweep contact sheet below. */
  g_settings.show_debug_settings = true;

  /* Every section stays reachable and its nav row stays inside the scroll
   * window, whatever the panel can fit. With AR_OVERLAY_PREVIEW_DIR set this
   * doubles as a contact sheet: one BMP per (section, tab), which is the only
   * practical way to eyeball a layout change across the whole menu. */
  const char *preview_dir = getenv("AR_OVERLAY_PREVIEW_DIR");
  for (int section = 0; section < kSection_System + 1; section++) {
    NavToSection(section);
    if (!renderer) continue;
    SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
    int selected = -1, top = -1, visible = -1, total = -1;
    CHECK(SettingsOverlay_GetNavigationState(
        &selected, &top, &visible, &total));
    CHECK(selected == section);
    CHECK(total == kSection_System + 1);
    CHECK(selected >= top && selected < top + visible);
    if (!preview_dir || !preview_dir[0]) continue;

    int tabs = 0;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    for (int tab = 0; tab < tabs; tab++) {
      NavToTab(tab);
      char path[256];
      snprintf(path, sizeof(path), "%s/section%d-tab%d.bmp",
               preview_dir, section, tab);
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      SDL_RenderClear(renderer);
      SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
      SDL_RenderPresent(renderer);
      CHECK(SDL_SaveBMP(surface, path));
    }
    CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  }

  /* Second contact sheet with debug settings OFF, so a layout review can see
   * the collapsed menu players actually get (Town 3D without Light/Weather,
   * System without Inspector, the dial rows gone). */
  if (renderer && preview_dir && preview_dir[0]) {
    g_settings.show_debug_settings = false;
    for (int section = 0; section < kSection_System + 1; section++) {
      NavToSection(section);
      int tabs = 0;
      CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
      CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      for (int tab = 0; tab < tabs; tab++) {
        NavToTab(tab);
        char path[256];
        snprintf(path, sizeof(path), "%s/section%d-tab%d-dbgoff.bmp",
                 preview_dir, section, tab);
        SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
        SDL_RenderClear(renderer);
        SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
        SDL_RenderPresent(renderer);
        CHECK(SDL_SaveBMP(surface, path));
      }
      CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
    }
    g_settings.show_debug_settings = true;
  }

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
