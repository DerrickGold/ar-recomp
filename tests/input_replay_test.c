#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actraiser_game.h"
#include "input_replay.h"

uint8 g_ram[kActRaiserWramSize];
int snes_frame_counter;

static int s_failures;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    s_failures++; \
  } \
} while (0)

static void Write32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static void AppendRecord(FILE *file, uint32_t frame, uint32_t inputs) {
  uint8_t record[8];
  Write32(record, frame);
  Write32(record + 4, inputs);
  CHECK(fwrite(record, 1, sizeof(record), file) == sizeof(record));
}

static char *MakeReplay(void) {
  char *path = strdup("/tmp/actraiser-input-replay-XXXXXX");
  CHECK(path != NULL);
  if (!path) return NULL;
  int descriptor = mkstemp(path);
  CHECK(descriptor >= 0);
  if (descriptor < 0) {
    free(path);
    return NULL;
  }
  FILE *file = fdopen(descriptor, "wb");
  CHECK(file != NULL);
  if (!file) {
    close(descriptor);
    unlink(path);
    free(path);
    return NULL;
  }

  /* Historical recordings begin with the power-on $5555 sentinel. Its high
   * numeric value must not make live handoff fire before replay frame 1. */
  AppendRecord(file, 0x5555, 0);
  AppendRecord(file, 1, 0x11);
  AppendRecord(file, 2, 0x22);
  CHECK(fclose(file) == 0);
  return path;
}

static InputReplayFrameResult ResolveAt(uint16_t frame, uint32_t live) {
  ActRaiser_WriteWram16(kActRaiserWram_GameFrame, frame);
  return InputReplay_Resolve(live);
}

static void ClearEnvironment(void) {
  CHECK(unsetenv("AR_INPUT_REPLAY") == 0);
  CHECK(unsetenv("AR_INPUT_RECORD") == 0);
  CHECK(unsetenv("AR_REPLAY_NOSTOP") == 0);
  CHECK(unsetenv("AR_REPLAY_LIVE_AFTER_END") == 0);
  CHECK(unsetenv("AR_GFLOG") == 0);
}

static void TestDefaultStop(const char *path) {
  CHECK(setenv("AR_INPUT_REPLAY", path, 1) == 0);
  InputReplay_Init();
  CHECK(InputReplay_ShouldProtectSaveData());

  InputReplayFrameResult result = ResolveAt(0x5555, 0xAA);
  CHECK(result.inputs == 0);
  CHECK(!result.stop_requested);
  result = ResolveAt(1, 0xAA);
  CHECK(result.inputs == 0x11);
  CHECK(!result.stop_requested);
  result = ResolveAt(2, 0xAA);
  CHECK(result.inputs == 0x22);
  CHECK(result.stop_requested);

  InputReplay_Shutdown();
  ClearEnvironment();
}

static void TestNoStopHoldsLastInput(const char *path) {
  CHECK(setenv("AR_INPUT_REPLAY", path, 1) == 0);
  CHECK(setenv("AR_REPLAY_NOSTOP", "1", 1) == 0);
  InputReplay_Init();

  CHECK(ResolveAt(1, 0xAA).inputs == 0x11);
  /* The power-on sentinel makes maximum frame $5555, but the last ordered
   * replay frame is 2. No-stop must hold frame 2 immediately, not return
   * zero-filled table entries until the numeric maximum is crossed. */
  InputReplayFrameResult result = ResolveAt(3, 0xAA);
  CHECK(result.inputs == 0x22);
  CHECK(!result.stop_requested);

  InputReplay_Shutdown();
  ClearEnvironment();
}

static void TestLiveHandoffAndCombinedRecording(const char *path) {
  char *record_path = strdup("/tmp/actraiser-input-record-XXXXXX");
  CHECK(record_path != NULL);
  if (!record_path) return;
  int descriptor = mkstemp(record_path);
  CHECK(descriptor >= 0);
  if (descriptor < 0) {
    free(record_path);
    return;
  }
  close(descriptor);

  CHECK(setenv("AR_INPUT_REPLAY", path, 1) == 0);
  CHECK(setenv("AR_INPUT_RECORD", record_path, 1) == 0);
  CHECK(setenv("AR_REPLAY_LIVE_AFTER_END", "1", 1) == 0);
  CHECK(setenv("AR_REPLAY_NOSTOP", "1", 1) == 0);
  InputReplay_Init();

  InputReplayFrameResult result = ResolveAt(0x5555, 0xAA);
  CHECK(result.inputs == 0);
  CHECK(!result.stop_requested);
  result = ResolveAt(1, 0xAA);
  CHECK(result.inputs == 0x11);
  CHECK(!result.stop_requested);
  result = ResolveAt(2, 0xAA);
  CHECK(result.inputs == 0x22);
  CHECK(!result.stop_requested);
  result = ResolveAt(3, 0x33);
  CHECK(result.inputs == 0x33);
  CHECK(!result.stop_requested);
  result = ResolveAt(4, 0x44);
  CHECK(result.inputs == 0x44);
  CHECK(!result.stop_requested);

  InputReplay_Shutdown();

  FILE *record = fopen(record_path, "rb");
  CHECK(record != NULL);
  if (record) {
    uint8_t bytes[5 * 8];
    CHECK(fread(bytes, 1, sizeof(bytes), record) == sizeof(bytes));
    CHECK(fgetc(record) == EOF);
    CHECK(bytes[4] == 0 && bytes[12] == 0x11 && bytes[20] == 0x22);
    CHECK(bytes[28] == 0x33 && bytes[36] == 0x44);
    CHECK(fclose(record) == 0);
  }

  CHECK(unlink(record_path) == 0);
  free(record_path);
  ClearEnvironment();
}

int main(void) {
  char *path = MakeReplay();
  if (!path) return 1;

  TestDefaultStop(path);
  TestNoStopHoldsLastInput(path);
  TestLiveHandoffAndCombinedRecording(path);

  CHECK(unlink(path) == 0);
  free(path);
  if (s_failures) return 1;
  puts("input replay tests: pass");
  return 0;
}
