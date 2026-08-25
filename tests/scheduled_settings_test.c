#define _POSIX_C_SOURCE 200809L

#include "scheduled_settings.h"

#include "actraiser_game.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
int snes_frame_counter;

static const SettingDesc kAction = {
  .key = "save_state",
  .type = kSettingType_Action,
};
static const SettingDesc kValue = {
  .key = "audio_master_volume",
  .type = kSettingType_Int,
};

static int s_failures;
static int s_action_calls;
static int s_set_calls;
static char s_set_text[32];

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

const SettingDesc *Settings_Find(const char *key) {
  if (!strcmp(key, kAction.key)) return &kAction;
  if (!strcmp(key, kValue.key)) return &kValue;
  return NULL;
}

bool Settings_InvokeAction(const SettingDesc *desc) {
  CHECK(desc == &kAction);
  s_action_calls++;
  return true;
}

SettingChangeResult Settings_SetText(const SettingDesc *desc,
                                     const char *text) {
  CHECK(desc == &kValue);
  s_set_calls++;
  snprintf(s_set_text, sizeof(s_set_text), "%s", text);
  return kSettingChange_Applied;
}

const char *Settings_ChangeResultName(SettingChangeResult result) {
  return result == kSettingChange_Applied ? "applied" : "rejected";
}

static void SetGameFrame(uint16 frame) {
  g_ram[kActRaiserWram_GameFrame] = (uint8)frame;
  g_ram[kActRaiserWram_GameFrame + 1] = (uint8)(frame >> 8);
}

static void Configure(const char *specification, const char *frame) {
  unsetenv("AR_SETTING_SET_2");
  unsetenv("AR_SETTING_AT_GF_2");
  CHECK(setenv("AR_SETTING_SET", specification, 1) == 0);
  CHECK(setenv("AR_SETTING_AT_GF", frame, 1) == 0);
  ScheduledSettings_Init();
}

static void TestScheduledAction(void) {
  s_action_calls = 0;
  snes_frame_counter = 1;
  SetGameFrame(9);
  Configure("save_state=run", "10");
  ScheduledSettings_ApplyIfDue();
  CHECK(s_action_calls == 0);
  SetGameFrame(10);
  ScheduledSettings_ApplyIfDue();
  ScheduledSettings_ApplyIfDue();
  CHECK(s_action_calls == 1);
}

static void TestActionRequiresRunValue(void) {
  s_action_calls = 0;
  SetGameFrame(10);
  Configure("save_state=1", "10");
  ScheduledSettings_ApplyIfDue();
  CHECK(s_action_calls == 0);
}

static void TestOrdinarySettingStillUsesMutationApi(void) {
  s_set_calls = 0;
  s_set_text[0] = '\0';
  SetGameFrame(12);
  Configure("audio_master_volume=35", "12");
  ScheduledSettings_ApplyIfDue();
  CHECK(s_set_calls == 1);
  CHECK(!strcmp(s_set_text, "35"));
}

static void TestSecondScheduledChangeSurvivesFirst(void) {
  s_action_calls = 0;
  s_set_calls = 0;
  CHECK(setenv("AR_SETTING_SET", "save_state=run", 1) == 0);
  CHECK(setenv("AR_SETTING_AT_GF", "10", 1) == 0);
  CHECK(setenv("AR_SETTING_SET_2", "audio_master_volume=45", 1) == 0);
  CHECK(setenv("AR_SETTING_AT_GF_2", "12", 1) == 0);
  ScheduledSettings_Init();
  SetGameFrame(10);
  ScheduledSettings_ApplyIfDue();
  CHECK(s_action_calls == 1);
  CHECK(s_set_calls == 0);
  SetGameFrame(12);
  ScheduledSettings_ApplyIfDue();
  CHECK(s_action_calls == 1);
  CHECK(s_set_calls == 1);
  CHECK(!strcmp(s_set_text, "45"));
}

int main(void) {
  TestScheduledAction();
  TestActionRequiresRunValue();
  TestOrdinarySettingStillUsesMutationApi();
  TestSecondScheduledChangeSurvivesFirst();
  unsetenv("AR_SETTING_SET");
  unsetenv("AR_SETTING_AT_GF");
  unsetenv("AR_SETTING_SET_2");
  unsetenv("AR_SETTING_AT_GF_2");
  if (s_failures) {
    fprintf(stderr, "scheduled_settings_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("scheduled_settings_test: PASS");
  return 0;
}
