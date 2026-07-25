#include "forced_input.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "input_map.h"

enum {
  kMaximumPulseCount = 16,
  kPulseDurationFrames = 4,
};

static int s_force_after_host_frame;
static uint32_t s_forced_input_mask;
static int s_pulse_start_frames[kMaximumPulseCount];
static size_t s_pulse_count;

static bool ParseInt(const char *text, int base, int *value) {
  errno = 0;
  char *end;
  const long parsed = strtol(text, &end, base);
  const bool has_digits = end != text;
  while (isspace((unsigned char)*end)) end++;
  if (errno == ERANGE || !has_digits || *end != '\0' ||
      parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

static bool ParseInputMask(const char *text, uint32_t *mask) {
  errno = 0;
  char *end;
  const unsigned long parsed = strtoul(text, &end, 0);
  const bool has_digits = end != text;
  while (isspace((unsigned char)*end)) end++;
  if (errno == ERANGE || !has_digits || *end != '\0' ||
      parsed > UINT32_MAX) {
    return false;
  }
  *mask = (uint32_t)parsed;
  return true;
}

static void ParsePulseFrames(const char *list) {
  const char *cursor = list;
  while (*cursor && s_pulse_count < kMaximumPulseCount) {
    while (isspace((unsigned char)*cursor)) cursor++;

    errno = 0;
    char *end;
    const long parsed = strtol(cursor, &end, 10);
    if (errno == ERANGE || end == cursor ||
        parsed < INT_MIN || parsed > INT_MAX) {
      fprintf(stderr,
              "[forced-input] invalid AR_FORCE_PULSES='%s' "
              "(want comma-separated host frames)\n",
              list);
      s_pulse_count = 0;
      return;
    }
    s_pulse_start_frames[s_pulse_count++] = (int)parsed;

    cursor = end;
    while (isspace((unsigned char)*cursor)) cursor++;
    if (!*cursor) return;
    if (*cursor != ',') {
      fprintf(stderr,
              "[forced-input] invalid AR_FORCE_PULSES='%s' "
              "(want comma-separated host frames)\n",
              list);
      s_pulse_count = 0;
      return;
    }
    cursor++;
  }

  while (isspace((unsigned char)*cursor)) cursor++;
  if (*cursor) {
    fprintf(stderr,
            "[forced-input] AR_FORCE_PULSES has more than %u entries; "
            "extra entries ignored\n",
            (unsigned)kMaximumPulseCount);
  }
}

void ForcedInput_Init(void) {
  s_force_after_host_frame = -1;
  s_forced_input_mask = 1u << kInputAction_B;
  s_pulse_count = 0;

  const char *force_after = getenv("AR_FORCE_INPUT_AFTER");
  if (force_after) {
    /* Preserve the original atoi("") behavior for explicitly empty values. */
    if (!force_after[0]) {
      s_force_after_host_frame = 0;
    } else if (!ParseInt(force_after, 10, &s_force_after_host_frame)) {
      fprintf(stderr,
              "[forced-input] invalid AR_FORCE_INPUT_AFTER='%s' "
              "(want a host-frame number)\n",
              force_after);
      s_force_after_host_frame = -1;
    }
  }

  const char *input_mask = getenv("AR_FORCE_INPUT_MASK");
  if (input_mask) {
    /* Preserve the original strtoul("") behavior for explicitly empty values. */
    if (!input_mask[0]) {
      s_forced_input_mask = 0;
    } else if (!ParseInputMask(input_mask, &s_forced_input_mask)) {
      fprintf(stderr,
              "[forced-input] invalid AR_FORCE_INPUT_MASK='%s' "
              "(want a 32-bit integer)\n",
              input_mask);
      s_forced_input_mask = 1u << kInputAction_B;
    }
  }

  const char *pulse_frames = getenv("AR_FORCE_PULSES");
  if (pulse_frames && pulse_frames[0])
    ParsePulseFrames(pulse_frames);
}

uint32_t ForcedInput_Apply(uint32_t live_inputs, int host_frame) {
  uint32_t resolved_inputs = live_inputs;
  if (s_force_after_host_frame >= 0 &&
      host_frame >= s_force_after_host_frame) {
    resolved_inputs |= s_forced_input_mask;
  }

  for (size_t index = 0; index < s_pulse_count; index++) {
    const int64_t elapsed_frames =
        (int64_t)host_frame - s_pulse_start_frames[index];
    if (elapsed_frames >= 0 && elapsed_frames < kPulseDurationFrames)
      resolved_inputs |= s_forced_input_mask;
  }
  return resolved_inputs;
}
