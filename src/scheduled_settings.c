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
  kScheduledSettingCapacity = 2,
  kSettingKeyCapacity = 64,
  kSettingValueCapacity = 256,
};

typedef struct ScheduledSetting {
  bool pending;
  uint16_t target_game_frame;
  char key[kSettingKeyCapacity];
  char value[kSettingValueCapacity];
} ScheduledSetting;

static ScheduledSetting s_changes[kScheduledSettingCapacity];

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

static bool ParseSettingChange(ScheduledSetting *change,
                               const char *specification) {
  const char *equals = strchr(specification, '=');
  const size_t key_length =
      equals ? (size_t)(equals - specification) : 0;
  const size_t value_length = equals ? strlen(equals + 1) : 0;

  if (!equals || key_length == 0 ||
      key_length >= sizeof(change->key) ||
      value_length >= sizeof(change->value)) {
    fprintf(stderr,
            "[settings] invalid AR_SETTING_SET='%s' "
            "(want key=value within %u/%u characters)\n",
            specification,
            (unsigned)(sizeof(change->key) - 1),
            (unsigned)(sizeof(change->value) - 1));
    return false;
  }

  memcpy(change->key, specification, key_length);
  change->key[key_length] = '\0';
  memcpy(change->value, equals + 1, value_length + 1);
  return true;
}

void ScheduledSettings_Init(void) {
  static const char *const specifications[kScheduledSettingCapacity] = {
    "AR_SETTING_SET", "AR_SETTING_SET_2",
  };
  static const char *const targets[kScheduledSettingCapacity] = {
    "AR_SETTING_AT_GF", "AR_SETTING_AT_GF_2",
  };
  memset(s_changes, 0, sizeof(s_changes));
  for (int i = 0; i < kScheduledSettingCapacity; i++) {
    ScheduledSetting *change = &s_changes[i];
    const char *specification = getenv(specifications[i]);
    if (!specification || !specification[0]) continue;
    change->pending =
        ParseTargetGameFrame(
            getenv(targets[i]), &change->target_game_frame) &&
        ParseSettingChange(change, specification);
  }
}

void ScheduledSettings_ApplyIfDue(void) {
  if (snes_frame_counter <= 0) return;

  const uint16_t game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  for (int i = 0; i < kScheduledSettingCapacity; i++) {
    ScheduledSetting *change = &s_changes[i];
    if (!change->pending || game_frame < change->target_game_frame)
      continue;

    /* Clear before invoking a load-state action. Scheduled diagnostics are
     * host state, not part of the snapshot, so a backwards game-frame jump
     * must not invoke this action again. */
    change->pending = false;
    const SettingDesc *descriptor = Settings_Find(change->key);
    if (descriptor && descriptor->type == kSettingType_Action) {
      const bool invoked = !strcmp(change->value, "run") &&
          Settings_InvokeAction(descriptor);
      fprintf(stderr, "[settings] gf=%u %s=%s -> %s action\n",
              (unsigned)game_frame, change->key, change->value,
              invoked ? "applied" : "rejected");
      continue;
    }
    const SettingChangeResult result =
        descriptor
            ? Settings_SetText(descriptor, change->value)
            : kSettingChange_Rejected;
    fprintf(stderr, "[settings] gf=%u %s=%s -> %s%s\n",
            (unsigned)game_frame, change->key, change->value,
            Settings_ChangeResultName(result),
            descriptor ? "" : " (unknown key)");
  }
}
