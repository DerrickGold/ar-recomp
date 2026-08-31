#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actraiser_game.h"
#include "input_replay.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/runner/replay.h"

uint8 g_ram[kActRaiserWramSize];
int snes_frame_counter;

static SnesRunnerApi s_fake_api;
static bool s_use_fake_api;
static uint64_t s_fake_frame_counter;
static uint32_t s_fake_inputs;
static uint8_t s_fake_digest_byte;
static int s_fake_runner_storage;

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version) {
  (void)requested_abi_version;
  return s_use_fake_api ? &s_fake_api : NULL;
}

static SrResult FakeQueryGenerations(
    SrRunnerHandle *runner, SrGenerationSnapshot *out) {
  (void)runner;
  if (!out) return SR_RESULT_INVALID_ARGUMENT;
  out->lifetime_generation = 7u;
  return SR_RESULT_OK;
}

static SrResult FakeQueryInputState(
    SrRunnerHandle *runner, SrInputStateSnapshot *out) {
  (void)runner;
  if (!out) return SR_RESULT_INVALID_ARGUMENT;
  out->lifetime_generation = 7u;
  out->frame_counter = s_fake_frame_counter;
  out->packed_buttons[0] = (uint16_t)(s_fake_inputs & 0x0fffu);
  out->packed_buttons[1] = (uint16_t)((s_fake_inputs >> 12) & 0x0fffu);
  return SR_RESULT_OK;
}

static SrResult FakeQuerySemanticDigest(
    SrRunnerHandle *runner, const SrSemanticDigestRequest *request,
    SrSemanticDigestResult *out) {
  (void)runner;
  if (!request || !out || request->lifetime_generation != 7u)
    return SR_RESULT_INVALID_ARGUMENT;
  out->lifetime_generation = 7u;
  out->frame_counter = s_fake_frame_counter;
  out->schema_version = SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION;
  memset(out->sha256, s_fake_digest_byte, sizeof(out->sha256));
  return SR_RESULT_OK;
}

static SrRunnerHandle *FakeRunner(void) {
  return (SrRunnerHandle *)&s_fake_runner_storage;
}

static void ConfigureFakeApi(void) {
  s_fake_api = (SnesRunnerApi){
    .abi_version = SR_RUNNER_ABI_VERSION,
    .struct_size = SNES_RUNNER_API_SEMANTIC_DIGEST_SIZE,
    .capabilities = SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_INPUT_STATE | SR_RUNNER_CAP_SEMANTIC_DIGEST,
    .query_generations = FakeQueryGenerations,
    .query_input_state = FakeQueryInputState,
    .query_semantic_digest = FakeQuerySemanticDigest,
  };
  s_use_fake_api = true;
}

static SrResult ReadExact(void *user_data, uint8_t *bytes,
                          uint32_t byte_count) {
  FILE *file = (FILE *)user_data;
  size_t count = fread(bytes, 1, byte_count, file);
  if (count == byte_count) return SR_RESULT_OK;
  return count == 0 && feof(file)
      ? SR_RESULT_UNAVAILABLE : SR_RESULT_INVALID_ARGUMENT;
}

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
  InputReplayFrameResult result = InputReplay_Resolve(live);
  if (s_use_fake_api) s_fake_inputs = result.inputs;
  CHECK(InputReplay_CompleteTick(s_use_fake_api ? FakeRunner() : NULL));
  return result;
}

static void ClearEnvironment(void) {
  CHECK(unsetenv("AR_INPUT_REPLAY") == 0);
  CHECK(unsetenv("AR_INPUT_RECORD") == 0);
  CHECK(unsetenv("AR_REPLAY_NOSTOP") == 0);
  CHECK(unsetenv("AR_REPLAY_LIVE_AFTER_END") == 0);
  CHECK(unsetenv("AR_GFLOG") == 0);
  CHECK(unsetenv("AR_REPLAY_CHECKPOINT_INTERVAL") == 0);
  s_use_fake_api = false;
}

static void TestDefaultStop(const char *path) {
  CHECK(setenv("AR_INPUT_REPLAY", path, 1) == 0);
  InputReplay_Init();
  CHECK(InputReplay_BeginSession(NULL, "actraiser"));
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
  CHECK(InputReplay_BeginSession(NULL, "actraiser"));

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
  ConfigureFakeApi();
  s_fake_frame_counter = 0u;
  s_fake_inputs = 0u;
  s_fake_digest_byte = 0x44u;
  InputReplay_Init();
  CHECK(InputReplay_BeginSession(FakeRunner(), "actraiser"));

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
    SrInputReplayReader reader = SR_INPUT_REPLAY_READER_INIT;
    SrInputReplayHeader header = {
      .struct_size = SR_INPUT_REPLAY_HEADER_V1_SIZE,
    };
    CHECK(sr_input_replay_reader_begin(
              &reader, ReadExact, record, &header) == SR_RESULT_OK);
    CHECK(!strcmp(header.game_id, "actraiser"));
    const uint16_t expected[] = {0, 0x11, 0x22, 0x33, 0x44};
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
         index++) {
      SrInputReplayRecord item = {
        .struct_size = SR_INPUT_REPLAY_RECORD_V1_SIZE,
      };
      CHECK(sr_input_replay_reader_next(&reader, &item) == SR_RESULT_OK);
      CHECK(item.type == SR_INPUT_REPLAY_RECORD_FRAME);
      CHECK(item.frame.frame_ordinal == index);
      CHECK(item.frame.packed_buttons[0] == expected[index]);
    }
    SrInputReplayRecord end = {
      .struct_size = SR_INPUT_REPLAY_RECORD_V1_SIZE,
    };
    CHECK(sr_input_replay_reader_next(&reader, &end) == SR_RESULT_UNAVAILABLE);
    CHECK(fclose(record) == 0);
  }

  /* The application must also consume the artifact it just wrote. Canonical
   * replay is host-tick ordered, so the game's logical frame counter is
   * deliberately varied to prove it is not used as an index. */
  ClearEnvironment();
  ConfigureFakeApi();
  s_fake_frame_counter = 0u;
  s_fake_inputs = 0u;
  s_fake_digest_byte = 0x44u;
  CHECK(setenv("AR_INPUT_REPLAY", record_path, 1) == 0);
  InputReplay_Init();
  CHECK(InputReplay_BeginSession(FakeRunner(), "actraiser"));
  const uint16_t canonical_expected[] = {0, 0x11, 0x22, 0x33, 0x44};
  for (size_t index = 0;
       index < sizeof(canonical_expected) / sizeof(canonical_expected[0]);
       index++) {
    const InputReplayFrameResult canonical =
        ResolveAt((uint16_t)(400u - index * 7u), 0xAA);
    CHECK(canonical.inputs == canonical_expected[index]);
    CHECK(canonical.stop_requested ==
          (index + 1u == sizeof(canonical_expected) /
                           sizeof(canonical_expected[0])));
  }
  InputReplay_Shutdown();

  CHECK(unlink(record_path) == 0);
  free(record_path);
  ClearEnvironment();
}

static void TestCanonicalIdentityAndCheckpoint(void) {
  char *record_path = strdup("/tmp/actraiser-input-checkpoint-XXXXXX");
  CHECK(record_path != NULL);
  if (!record_path) return;
  int descriptor = mkstemp(record_path);
  CHECK(descriptor >= 0);
  if (descriptor < 0) {
    free(record_path);
    return;
  }
  close(descriptor);

  ConfigureFakeApi();
  s_fake_frame_counter = 10u;
  s_fake_inputs = 0x654321u;
  s_fake_digest_byte = 0x5au;
  CHECK(setenv("AR_INPUT_RECORD", record_path, 1) == 0);
  CHECK(setenv("AR_REPLAY_CHECKPOINT_INTERVAL", "1", 1) == 0);
  InputReplay_Init();
  CHECK(InputReplay_BeginSession(FakeRunner(), "actraiser"));
  InputReplayFrameResult result = InputReplay_Resolve(0x111111u);
  CHECK(result.inputs == 0x111111u);
  CHECK(InputReplay_CompleteTick(FakeRunner()));
  InputReplay_Shutdown();

  FILE *record = fopen(record_path, "rb");
  CHECK(record != NULL);
  if (record) {
    SrInputReplayReader reader = SR_INPUT_REPLAY_READER_INIT;
    SrInputReplayHeader header = {
      .struct_size = SR_INPUT_REPLAY_HEADER_V1_SIZE,
    };
    CHECK(sr_input_replay_reader_begin(
              &reader, ReadExact, record, &header) == SR_RESULT_OK);
    CHECK(header.start_frame_ordinal == 10u);
    CHECK((header.flags & SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID) != 0u);
    CHECK(header.initial_state_sha256[0] == 0x5au);
    SrInputReplayRecord item = {
      .struct_size = SR_INPUT_REPLAY_RECORD_V1_SIZE,
    };
    CHECK(sr_input_replay_reader_next(&reader, &item) == SR_RESULT_OK);
    CHECK(item.type == SR_INPUT_REPLAY_RECORD_FRAME);
    CHECK(item.frame.frame_ordinal == 10u);
    CHECK(item.frame.packed_buttons[0] == 0x321u);
    CHECK(item.frame.packed_buttons[1] == 0x654u);
    item = (SrInputReplayRecord){
      .struct_size = SR_INPUT_REPLAY_RECORD_V1_SIZE,
    };
    CHECK(sr_input_replay_reader_next(&reader, &item) == SR_RESULT_OK);
    CHECK(item.type == SR_INPUT_REPLAY_RECORD_CHECKPOINT);
    CHECK(item.checkpoint.frame_ordinal == 10u);
    CHECK(item.checkpoint.semantic_sha256[0] == 0x5au);
    CHECK(fclose(record) == 0);
  }

  ClearEnvironment();
  CHECK(setenv("AR_INPUT_REPLAY", record_path, 1) == 0);
  InputReplay_Init();
  CHECK(!InputReplay_BeginSession(NULL, "actraiser"));
  CHECK(strstr(InputReplay_LastError(), "input-state service") != NULL);
  InputReplay_Shutdown();

  ClearEnvironment();
  ConfigureFakeApi();
  s_fake_frame_counter = 10u;
  s_fake_inputs = 0u;
  s_fake_digest_byte = 0x5au;
  CHECK(setenv("AR_INPUT_REPLAY", record_path, 1) == 0);
  InputReplay_Init();
  CHECK(InputReplay_BeginSession(FakeRunner(), "actraiser"));
  result = InputReplay_Resolve(0u);
  CHECK(result.inputs == 0x654321u);
  s_fake_digest_byte = 0x33u;
  CHECK(!InputReplay_CompleteTick(FakeRunner()));
  CHECK(InputReplay_Failed());
  CHECK(strstr(InputReplay_LastError(), "semantic checkpoint mismatch") !=
        NULL);
  InputReplay_Shutdown();

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
  TestCanonicalIdentityAndCheckpoint();

  CHECK(unlink(path) == 0);
  free(path);
  if (s_failures) return 1;
  puts("input replay tests: pass");
  return 0;
}
