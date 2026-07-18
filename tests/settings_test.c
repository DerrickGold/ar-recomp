#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool g_ws_active;
int g_ws_extra;
uint8 g_ram[0x20000];

static int s_failures;
static int s_observer_calls;
static const SettingDesc *s_observer_desc;
static SettingChangeResult s_observer_result;
static int s_action_calls;
static const SettingDesc *s_action_desc;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void ChangeObserved(const SettingDesc *desc,
                           SettingChangeResult result) {
  s_observer_calls++;
  s_observer_desc = desc;
  s_observer_result = result;
}

static bool ActionObserved(const SettingDesc *desc) {
  s_action_calls++;
  s_action_desc = desc;
  return true;
}

static void ClearSettingsEnv(void) {
  Settings_ClearConfigLayer();
  for (int i = 0; i < g_setting_desc_count; i++) {
    if (g_setting_descs[i].env)
      unsetenv(g_setting_descs[i].env);
  }
}

static void TestDefaultsAndMetadata(void) {
  ClearSettingsEnv();
  memset(g_ram, 0, sizeof(g_ram));
  g_ws_active = true;
  g_ws_extra = 43;
  Settings_Init();

  CHECK(g_setting_desc_count == 101);
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *a = &g_setting_descs[i];
    CHECK(a->key && a->key[0] && a->label && a->tooltip);
    CHECK((a->type == kSettingType_Action) == (a->field == NULL));
    CHECK(Settings_Find(a->key) == a);
    for (int j = i + 1; j < g_setting_desc_count; j++) {
      CHECK(strcmp(a->key, g_setting_descs[j].key) != 0);
      if (a->field && g_setting_descs[j].field)
        CHECK(a->field != g_setting_descs[j].field);
    }
    char formatted[512];
    Settings_FormatValue(a, formatted, sizeof(formatted));
    if (a->type == kSettingType_Action) {
      CHECK(!strcmp(formatted, "RUN"));
      CHECK(Settings_SetText(a, formatted) == kSettingChange_Rejected);
    } else {
      CHECK(Settings_SetText(a, formatted) == kSettingChange_Unchanged);
    }
  }
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);
  CHECK(g_settings.hud_scale_percent == 0);
  CHECK(g_settings.menu_scale_percent == 0);
  CHECK(g_settings.extended_aspect == 0);
  CHECK(g_settings.pixel_aspect == kPixelAspect_Crt43);
  CHECK(g_settings.window_scale == 3);
  CHECK(!g_settings.fullscreen && g_settings.new_renderer);
  CHECK(!g_settings.ignore_aspect_ratio);
  CHECK(g_settings.audio_enabled);
  CHECK(g_settings.audio_frequency == kAudioFrequency_44100);
  CHECK(Settings_AudioFrequencyHz() == 44100);
  CHECK(g_settings.audio_samples == 2048);
  CHECK(g_settings.audio_master_volume == 100);
  CHECK(g_settings.audio_dialog_blip);
  CHECK(g_settings.turbo_multiplier == 8);
  CHECK(g_settings.warp_target == 0x0101);
  CHECK(!g_settings.scene_inspector);
  CHECK(g_settings.save_backend == 0);
  CHECK(!g_settings.save_edit_armed && g_settings.save_autobackup);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Progress);
  for (int i = 0; i < 6; i++)
    CHECK(g_settings.save_region_progress[i] == kSaveProgressEdit_LeaveAsIs);
  CHECK(g_settings.save_master_level == 0 &&
        g_settings.save_master_hp == 0 &&
        g_settings.save_master_mp == 0 &&
        g_settings.save_lives == 0 &&
        g_settings.save_angel_sp_current == 0 &&
        g_settings.save_angel_sp_max == 0 &&
        g_settings.save_angel_hp_current == 0 &&
        g_settings.save_angel_hp_max == 0 &&
        g_settings.save_message_speed == 0);
  CHECK(!g_settings.save_player_name[0]);
  CHECK(g_settings.save_professional_mode == 0 &&
        g_settings.save_death_heim_state == 0 &&
        g_settings.save_equipped_magic == 0);
  for (int i = 0; i < 4; i++) CHECK(g_settings.save_magic_slots[i] == 0);
  for (int i = 0; i < 8; i++) CHECK(g_settings.save_item_slots[i] == 0);
  for (int region = 0; region < 6; region++)
    for (int act = 0; act < 2; act++)
      CHECK(g_settings.save_scores[region][act] == 0);
  CHECK(g_settings.ws_action && g_settings.ws_sim && g_settings.ws_sprites);
  CHECK(g_settings.cheat_inf_mp == 0);
  CHECK(!g_settings.cheat_moonjump);
  CHECK(g_settings.cheat_moonjump_speed == 6);
  CHECK(Settings_VisibleX0() == 0);
  CHECK(Settings_VisibleWidth() == 342);

  const SettingDesc *display = Settings_Find("display_mode");
  const SettingDesc *hp = Settings_Find("cheat_inf_hp");
  const SettingDesc *volume = Settings_Find("audio_master_volume");
  const SettingDesc *warp = Settings_Find("warp_target");
  const SettingDesc *warp_action = Settings_Find("warp_now");
  const SettingDesc *save_state_action = Settings_Find("save_state");
  const SettingDesc *load_state_action = Settings_Find("load_state");
  const SettingDesc *pause_action = Settings_Find("toggle_pause");
  const SettingDesc *restart_action = Settings_Find("restart_game");
  const SettingDesc *exit_action = Settings_Find("exit_desktop");
  const SettingDesc *music = Settings_Find("music_replacements");
  const SettingDesc *frequency = Settings_Find("audio_frequency");
  const SettingDesc *screen_ratio = Settings_Find("extended_aspect");
  const SettingDesc *stretch = Settings_Find("ignore_aspect_ratio");
  const SettingDesc *bridge_limit = Settings_Find("fix_bridge_limit");
  const SettingDesc *save_backend = Settings_Find("save_backend");
  const SettingDesc *save_fillmore = Settings_Find("save_prog_fillmore");
  const SettingDesc *save_page = Settings_Find("save_editor_page");
  const SettingDesc *save_apply = Settings_Find("save_apply_persist");
  const SettingDesc *inspector = Settings_Find("scene_inspector");
  const SettingDesc *dump_assets = Settings_Find("dump_scene_assets");
  CHECK(display && display->type == kSettingType_Enum);
  CHECK(display && display->enum_count == kDisplayMode_PresetCount);
  CHECK(volume && volume->category == kSettingCat_Audio);
  CHECK(volume && volume->apply == kApply_Callback);
  CHECK(volume && volume->minval == 0 && volume->maxval == 100 &&
        volume->step == 5);
  CHECK(warp && warp->type == kSettingType_Custom);
  CHECK(!Settings_IsMenuVisible(warp));
  CHECK(!Settings_IsMenuVisible(warp_action));
  CHECK(!Settings_IsMenuVisible(save_state_action));
  CHECK(!Settings_IsMenuVisible(load_state_action));
  CHECK(pause_action && pause_action->type == kSettingType_Action &&
        pause_action->apply == kApply_Action);
  CHECK(restart_action && restart_action->type == kSettingType_Action);
  CHECK(exit_action && exit_action->type == kSettingType_Action);
  CHECK(music && music->apply == kApply_Callback);
  CHECK(frequency && frequency->type == kSettingType_Enum &&
        frequency->enum_count == kAudioFrequency_Count &&
        frequency->apply == kApply_Restart);
  CHECK(screen_ratio && screen_ratio->category == kSettingCat_Display);
  CHECK(stretch && stretch->category == kSettingCat_Display);
  CHECK(bridge_limit && bridge_limit->category == kSettingCat_Extras);
  CHECK(inspector && inspector->category == kSettingCat_Inspector);
  CHECK(dump_assets && dump_assets->category == kSettingCat_Inspector &&
        dump_assets->type == kSettingType_Action);
  CHECK(save_backend && save_backend->category == kSettingCat_Save &&
        save_backend->apply == kApply_Restart);
  CHECK(save_fillmore && save_fillmore->apply == kApply_Save &&
        save_fillmore->enum_count == kSaveProgressEdit_Count);
  CHECK(save_page && save_page->enum_count == kSaveEditorPage_Count &&
        save_page->apply == kApply_Passive);
  CHECK(Settings_IsMenuVisible(save_fillmore));
  CHECK(!Settings_IsMenuVisible(Settings_Find("save_master_level")));
  CHECK(save_apply && save_apply->type == kSettingType_Action &&
        save_apply->category == kSettingCat_Save);
  CHECK(hp && Settings_IsAvailable(hp));
  for (int i = 0; i < g_setting_desc_count; i++)
    if (g_setting_descs[i].category == kSettingCat_Cheats)
      CHECK(Settings_IsAvailable(&g_setting_descs[i]));
  g_ram[0x18] = 1;
  CHECK(hp && Settings_IsAvailable(hp));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Widescreen), "Widescreen"));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Extras), "Extras"));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Inspector), "Inspector"));
  CHECK(!strcmp(Settings_ApplyKindName(kApply_Restart), "Restart required"));
  CHECK(!strcmp(Settings_ApplyKindName(kApply_Save), "Staged save edit"));
  CHECK(!strcmp(Settings_ChangeResultName(kSettingChange_Applied), "applied"));
  Settings_SetActionObserver(ActionObserved);
  s_action_calls = 0;
  CHECK(Settings_InvokeAction(pause_action));
  CHECK(s_action_calls == 1 && s_action_desc == pause_action);
  CHECK(Settings_InvokeAction(restart_action));
  CHECK(s_action_calls == 2 && s_action_desc == restart_action);
  CHECK(Settings_InvokeAction(exit_action));
  CHECK(s_action_calls == 3 && s_action_desc == exit_action);
  Settings_SetActionObserver(NULL);
}

static bool WriteTextFile(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (!file) return false;
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) ok = false;
  return ok;
}

static bool FileContains(const char *path, const char *needle) {
  FILE *file = fopen(path, "r");
  if (!file) return false;
  char buffer[16384];
  size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
  buffer[size] = 0;
  fclose(file);
  return strstr(buffer, needle) != NULL;
}

static void TestConfigSettingsEnvironmentPrecedence(void) {
  static const char config_path[] = "actraiser-settings-config-test.ini";
  static const char settings_path[] = "actraiser-settings-layer-test.ini";
  static const char saved_path[] = "actraiser-settings-saved-test.ini";
  remove(config_path);
  remove(settings_path);
  remove(saved_path);
  remove("actraiser-settings-saved-test.ini.tmp");

  ClearSettingsEnv();
  CHECK(WriteTextFile(config_path,
      "[Graphics]\n"
      "WindowScale = 4\n"
      "Fullscreen = 1\n"
      "ExtendedAspectRatio = 16:9\n"
      "AspectPAR = 4:3\n"
      "[Sound]\n"
      "AudioFreq = 32040\n"
      "AR_AUDIO_VOLUME = 65\n"
      "[Cheats]\n"
      "AR_DISPLAY_MODE = 2\n"
      "AR_WS_SPRITES = 1\n"
      "[KeyMap]\n"
      "Fullscreen = Alt+Return\n"));
  ParseConfigFile(config_path);

  CHECK(WriteTextFile(settings_path,
      "# menu-owned layer\n"
      "window_scale = 5\n"
      "audio_master_volume = 70%\n"
      "extended_aspect = 16:10\n"
      "pixel_aspect = Square pixels\n"
      "ws_sprites = On\n"
      "cheat_moonjump_speed = 9\n"
      "cheat_moonjump_button = $4000\n"
      "unknown_future_key = retained-by-future-version\n"));

  /* Real environment values must remain distinguishable from config.ini's
   * staged AR_* compatibility values and win over both file layers. */
  setenv("AR_AUDIO_VOLUME", "85", 1);
  setenv("AR_WS_SPRITES", "0", 1);
  g_ws_active = true;
  g_ws_extra = 52;
  Settings_InitWithFile(settings_path);
  Settings_FinalizeDisplayMode();

  CHECK(g_settings.window_scale == 5);       /* settings > config */
  CHECK(g_settings.fullscreen);              /* KeyMap did not clobber it */
  CHECK(g_settings.audio_frequency == kAudioFrequency_32040);
  CHECK(Settings_AudioFrequencyHz() == 32040);
  CHECK(g_settings.audio_master_volume == 85); /* env > settings > config */
  CHECK(!g_settings.ws_sprites);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(Settings_ExtendedAspectX() == 16 && Settings_ExtendedAspectY() == 10);
  CHECK(g_settings.pixel_aspect == kPixelAspect_Square);
  CHECK(g_settings.cheat_moonjump);  /* migrated from the old speed-only row */
  CHECK(g_settings.cheat_moonjump_speed == 9);

  CHECK(Settings_SetLong(Settings_Find("window_scale"), 6) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(Settings_Find("audio_master_volume"), 40) ==
        kSettingChange_Applied);
  CHECK(Settings_Save(saved_path));
  CHECK(FileContains(saved_path, "window_scale = 6"));
  CHECK(FileContains(saved_path, "audio_master_volume = 40%"));
  CHECK(FileContains(saved_path, "audio_frequency = 32.04 kHz"));
  CHECK(FileContains(saved_path, "turbo_multiplier = 8"));
  CHECK(FileContains(saved_path, "cheat_moonjump = On"));
  CHECK(FileContains(saved_path, "cheat_moonjump_speed = 9"));
  CHECK(!FileContains(saved_path, "cheat_moonjump_button"));
  CHECK(FileContains(saved_path, "warp_target = 0101"));
  CHECK(FileContains(saved_path, "save_backend = native-srm"));
  CHECK(FileContains(saved_path, "save_prog_fillmore = Leave as-is"));
  CHECK(FileContains(saved_path, "save_master_level = Leave as-is"));
  CHECK(FileContains(saved_path, "save_player_name = Leave as-is"));
  CHECK(FileContains(saved_path, "save_score_northwall_2 = Leave as-is"));
  CHECK(!FileContains(saved_path, "toggle_pause ="));
  CHECK(!FileContains(saved_path, "display_mode ="));
  CHECK(!FileContains("actraiser-settings-saved-test.ini.tmp", "anything"));
  CHECK(Settings_SetLong(Settings_Find("audio_master_volume"), 45) ==
        kSettingChange_Applied);
  CHECK(Settings_Save(saved_path));  /* atomically replace an existing file */
  CHECK(FileContains(saved_path, "audio_master_volume = 45%"));

  ClearSettingsEnv();
  setenv("AR_FULLSCREEN", "false", 1);
  g_ws_active = true;
  g_ws_extra = 52;
  Settings_InitWithFile(saved_path);
  Settings_FinalizeDisplayMode();
  CHECK(g_settings.window_scale == 6);
  CHECK(!g_settings.fullscreen);  /* new Phase-4 env aliases use INI syntax */
  CHECK(g_settings.audio_master_volume == 45);
  CHECK(g_settings.audio_frequency == kAudioFrequency_32040);
  CHECK(Settings_AudioFrequencyHz() == 32040);
  CHECK(!g_settings.ws_sprites);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(Settings_ExtendedAspectX() == 16 && Settings_ExtendedAspectY() == 10);

  remove(config_path);
  remove(settings_path);
  remove(saved_path);
}

static void TestLegacySeedEncodings(void) {
  ClearSettingsEnv();
  setenv("AR_INF_MP", "1", 1);
  setenv("AR_INF_HP", "0x20", 1); /* leading zero historically disables */
  setenv("AR_MOONJUMP", "9", 1);
  setenv("AR_NO_KNOCKBACK", "10", 1); /* legacy parser is hexadecimal */
  setenv("AR_PIN", "7E00210A,7F1234AA", 1);
  setenv("AR_WS_SPRITES", "0", 1);
  setenv("AR_AUDIO_VOLUME", "137", 1);
  setenv("AR_DIALOG_BLIP", "0", 1);
  setenv("AR_TURBO_MULT", "1", 1);
  setenv("AR_WARP", "0605", 1);
  g_ws_active = true;
  g_ws_extra = 43;
  Settings_Init();

  CHECK(g_settings.cheat_inf_mp == 10);
  CHECK(g_settings.cheat_inf_hp == 0);
  CHECK(g_settings.cheat_moonjump);
  CHECK(g_settings.cheat_moonjump_speed == 9);
  CHECK(g_settings.cheat_no_knockback == 0x10);
  CHECK(g_settings.pin_count == 2);
  CHECK(g_settings.audio_master_volume == 100);
  CHECK(!g_settings.audio_dialog_blip);
  CHECK(g_settings.turbo_multiplier == 2);
  CHECK(g_settings.warp_target == 0x0605);
  CHECK(g_settings.pins[0].off == 0x0021 && g_settings.pins[0].val == 0x0a);
  CHECK(g_settings.pins[1].off == 0x11234 && g_settings.pins[1].val == 0xaa);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);

  setenv("AR_DISPLAY_MODE", "1", 1);
  Settings_Init();
  CHECK(g_settings.display_mode == kDisplayMode_WideRaw);
  CHECK(g_settings.ws_action && g_settings.ws_sim);
  CHECK(!g_settings.ws_bgrefresh && !g_settings.ws_sprites);
}

static void TestMutationApi(void) {
  ClearSettingsEnv();
  g_ws_active = true;
  g_ws_extra = 43;
  Settings_Init();
  Settings_SetChangeObserver(ChangeObserved);
  s_observer_calls = 0;

  const SettingDesc *sprites = Settings_Find("ws_sprites");
  CHECK(Settings_SetLong(sprites, 0) == kSettingChange_Applied);
  CHECK(!g_settings.ws_sprites);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(s_observer_calls == 1 && s_observer_desc == sprites);
  CHECK(Settings_SetLong(sprites, 0) == kSettingChange_Unchanged);
  CHECK(s_observer_calls == 1);
  CHECK(Settings_SetLong(sprites, 1) == kSettingChange_Applied);
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);

  const SettingDesc *hud_scale = Settings_Find("hud_scale_percent");
  CHECK(Settings_SetLong(hud_scale, 287) == kSettingChange_Applied);
  CHECK(g_settings.hud_scale_percent == 275);
  char hud_value[32];
  Settings_FormatValue(hud_scale, hud_value, sizeof(hud_value));
  CHECK(!strcmp(hud_value, "2.75x"));

  const SettingDesc *menu_scale = Settings_Find("menu_scale_percent");
  CHECK(menu_scale && menu_scale->maxval == 800);
  CHECK(Settings_SetLong(menu_scale, 162) == kSettingChange_Applied);
  CHECK(g_settings.menu_scale_percent == 150);
  Settings_FormatValue(menu_scale, hud_value, sizeof(hud_value));
  CHECK(!strcmp(hud_value, "1.50x"));
  CHECK(Settings_SetText(menu_scale, "Auto") == kSettingChange_Applied);
  CHECK(g_settings.menu_scale_percent == 0);

  const SettingDesc *volume = Settings_Find("audio_master_volume");
  CHECK(Settings_SetLong(volume, 87) == kSettingChange_Applied);
  CHECK(g_settings.audio_master_volume == 85);
  CHECK(s_observer_desc == volume);
  char volume_value[16];
  Settings_FormatValue(volume, volume_value, sizeof(volume_value));
  CHECK(!strcmp(volume_value, "85%"));
  CHECK(Settings_SetText(volume, "40%") == kSettingChange_Applied);
  CHECK(g_settings.audio_master_volume == 40);
  CHECK(Settings_SetLong(volume, -1) == kSettingChange_Applied);
  CHECK(g_settings.audio_master_volume == 0);

  const SettingDesc *dialog_blip = Settings_Find("audio_dialog_blip");
  CHECK(Settings_SetLong(dialog_blip, 0) == kSettingChange_Applied);
  CHECK(!g_settings.audio_dialog_blip);

  const SettingDesc *frequency = Settings_Find("audio_frequency");
  CHECK(Settings_SetText(frequency, "48 kHz") ==
        kSettingChange_RestartPending);
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_AudioFrequencyHz() == 48000);
  CHECK(Settings_SetText(frequency, "32000") == kSettingChange_Rejected);
  CHECK(Settings_AudioFrequencyHz() == 48000);

  const SettingDesc *music = Settings_Find("music_replacements");
  CHECK(Settings_SetLong(music, 0) == kSettingChange_Applied);
  CHECK(!g_settings.music_replacements && s_observer_desc == music);

  char value[512];
  const SettingDesc *save_backend = Settings_Find("save_backend");
  CHECK(Settings_SetText(save_backend, "ini") ==
        kSettingChange_RestartPending);
  CHECK(g_settings.save_backend == 1);
  const SettingDesc *save_fillmore = Settings_Find("save_prog_fillmore");
  CHECK(Settings_SetText(save_fillmore, "act2-cleared") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_region_progress[0] ==
        kSaveProgressEdit_Act2Cleared);
  Settings_FormatValue(save_fillmore, value, sizeof(value));
  CHECK(!strcmp(value, "Act 2 cleared"));
  const SettingDesc *save_level = Settings_Find("save_master_level");
  CHECK(Settings_SetText(save_level, "17") == kSettingChange_Applied);
  CHECK(g_settings.save_master_level == 17);
  Settings_FormatValue(save_level, value, sizeof(value));
  CHECK(!strcmp(value, "17"));
  const SettingDesc *save_mp = Settings_Find("save_master_mp");
  CHECK(Settings_SetText(save_mp, "0") == kSettingChange_Applied);
  CHECK(g_settings.save_master_mp == 1);
  Settings_FormatValue(save_mp, value, sizeof(value));
  CHECK(!strcmp(value, "0"));
  const SettingDesc *save_page = Settings_Find("save_editor_page");
  CHECK(Settings_SetText(save_page, "Status") == kSettingChange_Applied);
  CHECK(!Settings_IsMenuVisible(save_fillmore));
  CHECK(Settings_IsMenuVisible(save_level));
  CHECK(Settings_IsMenuVisible(Settings_Find("save_player_name")));
  const SettingDesc *save_name = Settings_Find("save_player_name");
  CHECK(Settings_SetText(save_name, "CODEX") == kSettingChange_Applied);
  CHECK(!strcmp(g_settings.save_player_name, "CODEX"));
  CHECK(Settings_SetText(save_name, "TOO-LONG-NAME") ==
        kSettingChange_Rejected);
  const SettingDesc *save_magic = Settings_Find("save_magic_slot_1");
  CHECK(Settings_SetLong(save_page, kSaveEditorPage_Magic) ==
        kSettingChange_Applied);
  CHECK(Settings_IsMenuVisible(save_magic));
  CHECK(!Settings_IsMenuVisible(save_level));
  CHECK(Settings_SetText(save_magic, "Magical Aura") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_magic_slots[0] == 4);
  const SettingDesc *save_equipped = Settings_Find("save_equipped_magic");
  CHECK(Settings_SetText(save_equipped, "Magical Stardust") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_equipped_magic == 3);
  const SettingDesc *save_item = Settings_Find("save_item_slot_8");
  CHECK(Settings_SetLong(save_page, kSaveEditorPage_Items) ==
        kSettingChange_Applied);
  CHECK(Settings_IsMenuVisible(save_item));
  CHECK(Settings_SetText(save_item, "Strength of Angel") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_item_slots[7] == 14);
  const SettingDesc *save_score = Settings_Find("save_score_northwall_2");
  CHECK(Settings_SetLong(save_page, kSaveEditorPage_Scores) ==
        kSettingChange_Applied);
  CHECK(Settings_IsMenuVisible(save_score));
  CHECK(Settings_SetText(save_score, "12340") == kSettingChange_Applied);
  CHECK(g_settings.save_scores[5][1] == 1235);
  Settings_FormatValue(save_score, value, sizeof(value));
  CHECK(!strcmp(value, "12340"));
  CHECK(Settings_SetText(save_score, "12345") == kSettingChange_Rejected);

  const SettingDesc *turbo = Settings_Find("turbo_multiplier");
  CHECK(Settings_SetLong(turbo, 12) == kSettingChange_Applied);
  CHECK(g_settings.turbo_multiplier == 12);
  const SettingDesc *warp = Settings_Find("warp_target");
  CHECK(Settings_SetText(warp, "0303") == kSettingChange_Applied);
  CHECK(g_settings.warp_target == 0x0303);
  Settings_FormatValue(warp, value, sizeof(value));
  CHECK(!strcmp(value, "0303"));
  CHECK(Settings_SetText(warp, "garbage") == kSettingChange_Rejected);
  CHECK(g_settings.warp_target == 0x0303);

  const SettingDesc *renderer = Settings_Find("new_renderer");
  CHECK(Settings_SetLong(renderer, 0) == kSettingChange_Applied);
  CHECK(!g_settings.new_renderer);
  const SettingDesc *aspect = Settings_Find("extended_aspect");
  CHECK(Settings_SetText(aspect, "16:9") == kSettingChange_Applied);
  Settings_FormatValue(aspect, value, sizeof(value));
  CHECK(!strcmp(value, "16:9"));
  CHECK(Settings_SetText(aspect, "21:9") == kSettingChange_Rejected);
  const SettingDesc *pixel_aspect = Settings_Find("pixel_aspect");
  CHECK(Settings_SetText(pixel_aspect, "Square pixels") ==
        kSettingChange_Applied);
  const SettingDesc *window_scale = Settings_Find("window_scale");
  CHECK(Settings_SetLong(window_scale, 4) == kSettingChange_Applied);
  CHECK(g_settings.window_scale == 4);

  const SettingDesc *mp = Settings_Find("cheat_inf_mp");
  CHECK(Settings_SetLong(mp, 999) == kSettingChange_Applied);
  CHECK(g_settings.cheat_inf_mp == 255);
  Settings_FormatValue(mp, value, sizeof(value));
  CHECK(!strcmp(value, "255"));

  const SettingDesc *no_knockback = Settings_Find("cheat_no_knockback");
  CHECK(Settings_SetLong(no_knockback, 0x10) == kSettingChange_Applied);
  Settings_FormatValue(no_knockback, value, sizeof(value));
  CHECK(!strcmp(value, "10"));
  CHECK(Settings_SetText(no_knockback, value) == kSettingChange_Unchanged);

  const SettingDesc *freeze = Settings_Find("cheat_freeze_timer");
  Settings_FormatValue(freeze, value, sizeof(value));
  CHECK(!strcmp(value, "Off"));
  CHECK(Settings_SetText(freeze, value) == kSettingChange_Unchanged);

  const SettingDesc *magic = Settings_Find("cheat_all_magic");
  CHECK(Settings_SetLong(magic, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(magic, 0) == kSettingChange_AppliedStickyDisable);
  CHECK(s_observer_result == kSettingChange_AppliedStickyDisable);

  const SettingDesc *pins = Settings_Find("pins");
  CHECK(Settings_SetText(pins, "7E00210A,7F1234AA") == kSettingChange_Applied);
  Settings_FormatValue(pins, value, sizeof(value));
  CHECK(!strcmp(value, "7E00210A,7F1234AA"));
  CHECK(Settings_Reset(pins) == kSettingChange_AppliedStickyDisable);
  CHECK(g_settings.pin_count == 0);

  const SettingDesc *display = Settings_Find("display_mode");
  CHECK(Settings_SetText(display, "Widescreen raw") == kSettingChange_Applied);
  CHECK(g_settings.display_mode == kDisplayMode_WideRaw);
  CHECK(g_settings.ws_action && !g_settings.ws_sprites);
  CHECK(Settings_Reset(display) == kSettingChange_Applied);
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);
  CHECK(g_settings.ws_action && g_settings.ws_sprites);
}

static void TestCheatsCanBeStagedOutsideTheirRuntimeMode(void) {
  ClearSettingsEnv();
  memset(g_ram, 0, sizeof(g_ram));
  g_ws_active = true;
  g_ws_extra = 43;
  Settings_Init();

  /* $18=00 is the simulation/title/UI family. Action-only effects must remain
   * editable here; the runtime action gate consumes them after transition. */
  CHECK(g_ram[0x18] == 0);
  const SettingDesc *hp = Settings_Find("cheat_inf_hp");
  const SettingDesc *freeze = Settings_Find("cheat_freeze_timer");
  const SettingDesc *moonjump = Settings_Find("cheat_moonjump");
  const SettingDesc *moonjump_speed =
      Settings_Find("cheat_moonjump_speed");
  const SettingDesc *no_knockback = Settings_Find("cheat_no_knockback");
  CHECK(Settings_IsAvailable(hp));
  CHECK(Settings_IsAvailable(freeze));
  CHECK(Settings_IsAvailable(moonjump));
  CHECK(Settings_IsAvailable(moonjump_speed));
  CHECK(Settings_Find("cheat_moonjump_button") == NULL);
  CHECK(Settings_IsAvailable(no_knockback));
  CHECK(Settings_SetLong(hp, 32) == kSettingChange_Applied);
  CHECK(Settings_SetLong(freeze, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(moonjump, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(moonjump_speed, 9) == kSettingChange_Applied);
  CHECK(Settings_SetLong(no_knockback, 1) == kSettingChange_Applied);

  g_ram[0x18] = 1;
  CHECK(g_settings.cheat_inf_hp == 32);
  CHECK(g_settings.cheat_freeze_timer);
  CHECK(g_settings.cheat_moonjump);
  CHECK(g_settings.cheat_moonjump_speed == 9);
  CHECK(g_settings.cheat_no_knockback == 1);
}

static void TestNoWideBudget(void) {
  ClearSettingsEnv();
  setenv("AR_DISPLAY_MODE", "2", 1);
  g_ws_active = false;
  g_ws_extra = 0;
  Settings_Init();
  CHECK(g_settings.display_mode == kDisplayMode_43);
  CHECK(Settings_SetLong(Settings_Find("display_mode"),
                         kDisplayMode_WideRaw) == kSettingChange_Unchanged);
  CHECK(Settings_VisibleX0() == 0);
  CHECK(Settings_VisibleWidth() == 256);
}

int main(void) {
  TestDefaultsAndMetadata();
  TestConfigSettingsEnvironmentPrecedence();
  TestLegacySeedEncodings();
  TestMutationApi();
  TestCheatsCanBeStagedOutsideTheirRuntimeMode();
  TestNoWideBudget();
  ClearSettingsEnv();
  Settings_SetChangeObserver(NULL);
  Settings_SetActionObserver(NULL);
  if (s_failures) {
    fprintf(stderr, "settings tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings tests: pass\n");
  return 0;
}
