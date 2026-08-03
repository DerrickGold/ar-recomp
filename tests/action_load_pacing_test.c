#include <stdint.h>
#include <stdio.h>

#include "action_load_pacing.h"

static int s_failures;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    s_failures++; \
  } \
} while (0)

int main(void) {
  CHECK(kActionLoadPacingInsertedFrames == 315);

  for (uint8_t map_group = 0x01; map_group <= 0x07; map_group++) {
    CHECK(ActionLoadPacing_Frames(
              0x00, map_group,
              kActionLoadPacingForceBlankBlock, 0x8f) == 315);
  }

  CHECK(ActionLoadPacing_Frames(
            0x00, 0x00, kActionLoadPacingForceBlankBlock, 0x80) == 0);
  CHECK(ActionLoadPacing_Frames(
            0x08, 0x08, kActionLoadPacingForceBlankBlock, 0x80) == 0);
  CHECK(ActionLoadPacing_Frames(
            0x01, 0x01, kActionLoadPacingForceBlankBlock, 0x80) == 0);
  CHECK(ActionLoadPacing_Frames(0x00, 0x01, 0x00843D, 0x80) == 0);
  CHECK(ActionLoadPacing_Frames(
            0x00, 0x01, kActionLoadPacingForceBlankBlock, 0x0f) == 0);

  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x01, 0x80, 0, 0xF0, 0) ==
        kActionLoadPacingTrigger_Hold);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            0, 0x01, 0x80, 0, 0xF0, 0) ==
        kActionLoadPacingTrigger_Ignore);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x01, 0x80, 1, 0xF0, 0) ==
        kActionLoadPacingTrigger_Ignore);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x01, 0x80, 0, 0xF1, 0) ==
        kActionLoadPacingTrigger_Ignore);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x01, 0x80, 0, 0xF0, 1) ==
        kActionLoadPacingTrigger_Ignore);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x00, 0x80, 0, 0xF0, 0) ==
        kActionLoadPacingTrigger_Discard);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x01, 0x0f, 0, 0xF0, 0) ==
        kActionLoadPacingTrigger_Discard);
  CHECK(ActionLoadPacing_EvaluateTrigger(
            315, 0x08, 0x80, 0, 0xF0, 0) ==
        kActionLoadPacingTrigger_Discard);

  if (s_failures) {
    fprintf(stderr, "action_load_pacing_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("action_load_pacing_test: all tests passed");
  return 0;
}
