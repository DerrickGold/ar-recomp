#define _POSIX_C_SOURCE 200809L
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

static void ClearSettingsEnv(void) {
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

  CHECK(g_setting_desc_count == 22);
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *a = &g_setting_descs[i];
    CHECK(a->key && a->key[0] && a->label && a->tooltip && a->field);
    CHECK(Settings_Find(a->key) == a);
    for (int j = i + 1; j < g_setting_desc_count; j++) {
      CHECK(strcmp(a->key, g_setting_descs[j].key) != 0);
      CHECK(a->field != g_setting_descs[j].field);
    }
    char formatted[512];
    Settings_FormatValue(a, formatted, sizeof(formatted));
    CHECK(Settings_SetText(a, formatted) == kSettingChange_Unchanged);
  }
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);
  CHECK(g_settings.hud_scale_percent == 0);
  CHECK(g_settings.ws_action && g_settings.ws_sim && g_settings.ws_sprites);
  CHECK(g_settings.cheat_inf_mp == 0);
  CHECK(g_settings.cheat_moonjump_button == 0x8000);
  CHECK(Settings_VisibleX0() == 0);
  CHECK(Settings_VisibleWidth() == 342);

  const SettingDesc *display = Settings_Find("display_mode");
  const SettingDesc *hp = Settings_Find("cheat_inf_hp");
  CHECK(display && display->type == kSettingType_Enum);
  CHECK(display && display->enum_count == kDisplayMode_PresetCount);
  CHECK(hp && !Settings_IsAvailable(hp));
  g_ram[0x18] = 1;
  CHECK(hp && Settings_IsAvailable(hp));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Widescreen), "Widescreen"));
  CHECK(!strcmp(Settings_ApplyKindName(kApply_Restart), "Restart required"));
  CHECK(!strcmp(Settings_ChangeResultName(kSettingChange_Applied), "applied"));
}

static void TestLegacySeedEncodings(void) {
  ClearSettingsEnv();
  setenv("AR_INF_MP", "1", 1);
  setenv("AR_INF_HP", "0x20", 1); /* leading zero historically disables */
  setenv("AR_MOONJUMP", "1", 1);
  setenv("AR_NO_KNOCKBACK", "10", 1); /* legacy parser is hexadecimal */
  setenv("AR_PIN", "7E00210A,7F1234AA", 1);
  setenv("AR_WS_SPRITES", "0", 1);
  g_ws_active = true;
  g_ws_extra = 43;
  Settings_Init();

  CHECK(g_settings.cheat_inf_mp == 10);
  CHECK(g_settings.cheat_inf_hp == 0);
  CHECK(g_settings.cheat_moonjump_speed == 6);
  CHECK(g_settings.cheat_no_knockback == 0x10);
  CHECK(g_settings.pin_count == 2);
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

  const SettingDesc *mp = Settings_Find("cheat_inf_mp");
  CHECK(Settings_SetLong(mp, 999) == kSettingChange_Applied);
  CHECK(g_settings.cheat_inf_mp == 255);
  char value[512];
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
  TestLegacySeedEncodings();
  TestMutationApi();
  TestNoWideBudget();
  ClearSettingsEnv();
  Settings_SetChangeObserver(NULL);
  if (s_failures) {
    fprintf(stderr, "settings tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings tests: pass\n");
  return 0;
}
