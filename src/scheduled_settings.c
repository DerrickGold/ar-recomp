#include "scheduled_settings.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "common_rtl.h"
#include "settings.h"

enum {
  kSettingKeyCapacity = 64,
  kSettingValueCapacity = 256,
};

static bool s_change_pending;
static uint16_t s_target_game_frame;
static char s_setting_key[kSettingKeyCapacity];
static char s_setting_value[kSettingValueCapacity];

static bool ParseTargetGameFrame(const char *text, uint16_t *target) {
  if (!text || !text[0]) {
    *target = 0;
    return true;
  }

  errno = 0;
  char *end;
  const unsigned long parsed = strtoul(text, &end, 0);
  if (errno == ERANGE || end == text || *end != '\0' ||
      parsed > UINT16_MAX) {
    fprintf(stderr,
            "[settings] invalid AR_SETTING_AT_GF='%s' "
            "(want a 16-bit game frame)\n",
            text);
    return false;
  }

  *target = (uint16_t)parsed;
  return true;
}

static bool ParseSettingChange(const char *specification) {
  const char *equals = strchr(specification, '=');
  const size_t key_length =
      equals ? (size_t)(equals - specification) : 0;
  const size_t value_length = equals ? strlen(equals + 1) : 0;

  if (!equals || key_length == 0 ||
      key_length >= sizeof(s_setting_key) ||
      value_length >= sizeof(s_setting_value)) {
    fprintf(stderr,
            "[settings] invalid AR_SETTING_SET='%s' "
            "(want key=value within %u/%u characters)\n",
            specification,
            (unsigned)(sizeof(s_setting_key) - 1),
            (unsigned)(sizeof(s_setting_value) - 1));
    return false;
  }

  memcpy(s_setting_key, specification, key_length);
  s_setting_key[key_length] = '\0';
  memcpy(s_setting_value, equals + 1, value_length + 1);
  return true;
}

void ScheduledSettings_Init(void) {
  s_change_pending = false;
  s_target_game_frame = 0;
  s_setting_key[0] = '\0';
  s_setting_value[0] = '\0';

  const char *specification = getenv("AR_SETTING_SET");
  if (!specification || !specification[0]) return;

  const char *target_text = getenv("AR_SETTING_AT_GF");
  s_change_pending =
      ParseTargetGameFrame(target_text, &s_target_game_frame) &&
      ParseSettingChange(specification);
}

void ScheduledSettings_ApplyIfDue(void) {
  if (!s_change_pending || snes_frame_counter <= 0) return;

  const uint16_t game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  if (game_frame < s_target_game_frame) return;

  s_change_pending = false;
  const SettingDesc *descriptor = Settings_Find(s_setting_key);
  const SettingChangeResult result =
      descriptor
          ? Settings_SetText(descriptor, s_setting_value)
          : kSettingChange_Rejected;
  fprintf(stderr, "[settings] gf=%u %s=%s -> %s%s\n",
          (unsigned)game_frame, s_setting_key, s_setting_value,
          Settings_ChangeResultName(result),
          descriptor ? "" : " (unknown key)");
}
