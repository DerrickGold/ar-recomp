#include "input_replay.h"

#include "byte_order.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/runner/replay.h"

enum {
  kLegacyReplayRecordByteCount = 8,
  kLegacyReplayFrameCapacity = UINT16_MAX + 1u,
  kGameFrameLogInterval = 100,
  kReplayErrorCapacity = 256,
};

typedef enum ReplaySource {
  kReplaySource_None,
  kReplaySource_LegacyGameFrame,
  kReplaySource_CanonicalHostTick,
} ReplaySource;

typedef struct CanonicalReplayFrame {
  uint64_t ordinal;
  uint32_t inputs;
  SrInputReplayCheckpoint checkpoint;
  bool has_checkpoint;
} CanonicalReplayFrame;

static uint32_t *s_legacy_replay_inputs;
static uint32_t s_maximum_legacy_game_frame;
static uint32_t s_last_legacy_game_frame;
static bool s_has_legacy_records;

static CanonicalReplayFrame *s_canonical_frames;
static size_t s_canonical_frame_count;
static size_t s_canonical_frame_capacity;
static size_t s_canonical_frame_index;
static SrInputReplayHeader s_canonical_header;

static FILE *s_record_file;
static SrInputReplayWriter s_writer;
static bool s_writer_open;
static bool s_writer_failed;

static ReplaySource s_replay_source;
static bool s_replay_started;
static bool s_disable_auto_stop;
static bool s_live_after_replay;
static bool s_live_handoff_reported;
static bool s_save_data_protected;
static bool s_game_frame_logging_enabled;
static bool s_action_entry_reported;
static bool s_session_started;

static bool s_tick_pending;
static uint64_t s_pending_ordinal;
static uint32_t s_pending_inputs;
static size_t s_pending_canonical_index;
static bool s_pending_has_canonical_frame;
static uint64_t s_next_ordinal;
static uint64_t s_checkpoint_interval;

static char s_last_error[kReplayErrorCapacity];
static bool s_failed;

static void SetError(const char *message) {
  s_failed = true;
  snprintf(s_last_error, sizeof(s_last_error), "%s",
           message ? message : "input replay failed");
  fprintf(stderr, "[input-replay] %s\n", s_last_error);
}

const char *InputReplay_LastError(void) {
  return s_last_error[0] ? s_last_error : "input replay failed";
}

bool InputReplay_Failed(void) {
  return s_failed;
}

static unsigned ReadGameFrame(void) {
  return ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
}

static SrResult ReplayRead(void *user_data, uint8_t *bytes,
                           uint32_t byte_count) {
  FILE *file = (FILE *)user_data;
  const size_t count = fread(bytes, 1, byte_count, file);
  if (count == byte_count) return SR_RESULT_OK;
  if (count == 0u && feof(file)) return SR_RESULT_UNAVAILABLE;
  return SR_RESULT_INVALID_ARGUMENT;
}

static SrResult ReplayWrite(void *user_data, const uint8_t *bytes,
                            uint32_t byte_count) {
  FILE *file = (FILE *)user_data;
  return fwrite(bytes, 1, byte_count, file) == byte_count
      ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static bool AppendCanonicalFrame(const SrInputReplayFrame *frame) {
  if (s_canonical_frame_count == s_canonical_frame_capacity) {
    size_t capacity = s_canonical_frame_capacity
        ? s_canonical_frame_capacity * 2u : 1024u;
    if (capacity < s_canonical_frame_capacity ||
        capacity > SIZE_MAX / sizeof(*s_canonical_frames)) {
      SetError("canonical replay is too large");
      return false;
    }
    CanonicalReplayFrame *frames = (CanonicalReplayFrame *)realloc(
        s_canonical_frames, capacity * sizeof(*frames));
    if (!frames) {
      SetError("cannot allocate canonical replay frames");
      return false;
    }
    s_canonical_frames = frames;
    s_canonical_frame_capacity = capacity;
  }
  CanonicalReplayFrame *entry =
      &s_canonical_frames[s_canonical_frame_count++];
  memset(entry, 0, sizeof(*entry));
  entry->ordinal = frame->frame_ordinal;
  entry->inputs = (uint32_t)frame->packed_buttons[0] |
      ((uint32_t)frame->packed_buttons[1] << 12);
  return true;
}

static bool LoadCanonicalReplay(FILE *file, const char *path) {
  SrInputReplayReader reader = SR_INPUT_REPLAY_READER_INIT;
  s_canonical_header = (SrInputReplayHeader){
    .struct_size = SR_INPUT_REPLAY_HEADER_V1_SIZE,
  };
  if (sr_input_replay_reader_begin(
          &reader, ReplayRead, file, &s_canonical_header) != SR_RESULT_OK) {
    SetError("canonical replay header is malformed or truncated");
    return false;
  }

  for (;;) {
    SrInputReplayRecord record = {
      .struct_size = SR_INPUT_REPLAY_RECORD_V1_SIZE,
    };
    const SrResult result = sr_input_replay_reader_next(&reader, &record);
    if (result == SR_RESULT_UNAVAILABLE) break;
    if (result != SR_RESULT_OK) {
      SetError("canonical replay record stream is malformed or truncated");
      return false;
    }
    if (record.type == SR_INPUT_REPLAY_RECORD_FRAME) {
      if (!AppendCanonicalFrame(&record.frame)) return false;
    } else if (record.type == SR_INPUT_REPLAY_RECORD_CHECKPOINT) {
      if (s_canonical_frame_count == 0u) {
        SetError("canonical replay checkpoint has no preceding frame");
        return false;
      }
      CanonicalReplayFrame *entry =
          &s_canonical_frames[s_canonical_frame_count - 1u];
      entry->checkpoint = record.checkpoint;
      entry->has_checkpoint = true;
    }
  }
  if (s_canonical_frame_count == 0u) {
    SetError("canonical replay contains no input frames");
    return false;
  }
  s_replay_source = kReplaySource_CanonicalHostTick;
  fprintf(stderr,
          "[input-replay] %zu canonical host-tick records, start=%llu from %s\n",
          s_canonical_frame_count,
          (unsigned long long)s_canonical_header.start_frame_ordinal, path);
  return true;
}

static bool LoadLegacyReplay(FILE *file, const char *path) {
  s_legacy_replay_inputs = (uint32_t *)calloc(
      kLegacyReplayFrameCapacity, sizeof(*s_legacy_replay_inputs));
  if (!s_legacy_replay_inputs) {
    SetError("cannot allocate legacy replay table");
    return false;
  }

  uint8_t record[kLegacyReplayRecordByteCount];
  size_t record_count = 0;
  size_t invalid_frame_count = 0;
  for (;;) {
    const size_t count = fread(record, 1, sizeof(record), file);
    if (count == 0u && feof(file)) break;
    if (count != sizeof(record)) {
      SetError("legacy replay has a truncated final record");
      return false;
    }
    record_count++;
    const uint32_t game_frame = ByteOrder_ReadLe32(record);
    const uint32_t inputs = ByteOrder_ReadLe32(record + sizeof(uint32_t));
    if (game_frame >= kLegacyReplayFrameCapacity) {
      invalid_frame_count++;
      continue;
    }
    s_legacy_replay_inputs[game_frame] = inputs;
    if (!s_has_legacy_records ||
        game_frame > s_maximum_legacy_game_frame)
      s_maximum_legacy_game_frame = game_frame;
    s_last_legacy_game_frame = game_frame;
    s_has_legacy_records = true;
  }
  if (ferror(file)) {
    SetError("legacy replay read failed");
    return false;
  }
  if (!s_has_legacy_records) {
    SetError("legacy replay contains no usable input records");
    return false;
  }
  s_replay_source = kReplaySource_LegacyGameFrame;
  fprintf(stderr,
          "[input-replay] %zu legacy records, max gf=%u last gf=%u from %s\n",
          record_count, s_maximum_legacy_game_frame,
          s_last_legacy_game_frame, path);
  if (invalid_frame_count) {
    fprintf(stderr,
            "[input-replay] ignored %zu record(s) outside 16-bit "
            "game-frame range\n",
            invalid_frame_count);
  }
  return true;
}

static bool LoadReplayFile(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    SetError("cannot open configured replay file");
    return false;
  }
  uint8_t magic[SR_INPUT_REPLAY_MAGIC_SIZE] = {0};
  const size_t magic_size = fread(magic, 1, sizeof(magic), file);
  const bool canonical =
      magic_size == sizeof(magic) &&
      memcmp(magic, SR_INPUT_REPLAY_MAGIC, sizeof(magic)) == 0;
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    SetError("cannot rewind configured replay file");
    return false;
  }
  const bool loaded = canonical
      ? LoadCanonicalReplay(file, path) : LoadLegacyReplay(file, path);
  fclose(file);
  return loaded;
}

static void OpenRecordFile(const char *path) {
  s_record_file = fopen(path, "wb");
  if (!s_record_file) {
    SetError("cannot open configured input recording");
    s_writer_failed = true;
    return;
  }
  fprintf(stderr, "[input-record] canonical host-tick stream -> %s\n", path);
}

static uint64_t ParseCheckpointInterval(void) {
  const char *text = getenv("AR_REPLAY_CHECKPOINT_INTERVAL");
  if (!text || !text[0]) return 0u;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 0);
  if (end == text || *end != '\0' || value == 0u) {
    fprintf(stderr,
            "[input-replay] ignored invalid "
            "AR_REPLAY_CHECKPOINT_INTERVAL='%s'\n", text);
    return 0u;
  }
  return (uint64_t)value;
}

void InputReplay_Init(void) {
  s_last_error[0] = '\0';
  s_failed = false;
  const char *replay_path = getenv("AR_INPUT_REPLAY");
  s_save_data_protected = replay_path != NULL;
  if (replay_path && replay_path[0] && !LoadReplayFile(replay_path))
    s_replay_source = kReplaySource_None;

  const char *record_path = getenv("AR_INPUT_RECORD");
  if (record_path && record_path[0]) OpenRecordFile(record_path);

  s_disable_auto_stop = getenv("AR_REPLAY_NOSTOP") != NULL;
  s_live_after_replay = getenv("AR_REPLAY_LIVE_AFTER_END") != NULL;
  if (s_live_after_replay && s_disable_auto_stop) {
    fprintf(stderr,
            "[input-replay] AR_REPLAY_LIVE_AFTER_END takes precedence over "
            "AR_REPLAY_NOSTOP\n");
  }
  s_game_frame_logging_enabled = getenv("AR_GFLOG") != NULL;
  s_checkpoint_interval = ParseCheckpointInterval();
}

static bool QueryInputState(SrRunnerHandle *runner,
                            SrInputStateSnapshot *state) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  if (!runner || !api ||
      api->struct_size < SNES_RUNNER_API_INPUT_STATE_SIZE ||
      (api->capabilities & SR_RUNNER_CAP_INPUT_STATE) == 0u ||
      !api->query_input_state)
    return false;
  *state = (SrInputStateSnapshot){
    .struct_size = SR_INPUT_STATE_SNAPSHOT_V2_SIZE,
  };
  return api->query_input_state(runner, state) == SR_RESULT_OK;
}

static bool QuerySemanticDigest(SrRunnerHandle *runner,
                                SrSemanticDigestResult *digest) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrGenerationSnapshot generations = {
    .struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE,
  };
  if (!runner || !api ||
      api->struct_size < SNES_RUNNER_API_SEMANTIC_DIGEST_SIZE ||
      (api->capabilities & (SR_RUNNER_CAP_GENERATION_COUNTERS |
                            SR_RUNNER_CAP_SEMANTIC_DIGEST)) !=
          (SR_RUNNER_CAP_GENERATION_COUNTERS |
           SR_RUNNER_CAP_SEMANTIC_DIGEST) ||
      !api->query_generations || !api->query_semantic_digest ||
      api->query_generations(runner, &generations) != SR_RESULT_OK)
    return false;
  SrSemanticDigestRequest request = {
    .struct_size = SR_SEMANTIC_DIGEST_REQUEST_V2_SIZE,
    .lifetime_generation = generations.lifetime_generation,
  };
  *digest = (SrSemanticDigestResult){
    .struct_size = SR_SEMANTIC_DIGEST_RESULT_V2_SIZE,
  };
  return api->query_semantic_digest(runner, &request, digest) == SR_RESULT_OK;
}

bool InputReplay_BeginSession(SrRunnerHandle *runner, const char *game_id) {
  if (s_session_started) return true;
  const char *replay_path = getenv("AR_INPUT_REPLAY");
  if (s_last_error[0] &&
      ((replay_path && replay_path[0]) || s_writer_failed))
    return false;
  if (!game_id || !game_id[0] ||
      strlen(game_id) >= SR_INPUT_REPLAY_GAME_ID_SIZE) {
    SetError("game id does not fit the canonical replay header");
    return false;
  }

  SrInputStateSnapshot input_state;
  const bool has_input_state = QueryInputState(runner, &input_state);
  SrSemanticDigestResult initial_digest;
  const bool has_initial_digest =
      QuerySemanticDigest(runner, &initial_digest);
  const bool canonical_session =
      s_replay_source == kReplaySource_CanonicalHostTick ||
      s_record_file != NULL;
  if (canonical_session && !has_input_state) {
    SetError("runner input-state service is required for canonical replay");
    return false;
  }
  if (s_record_file && s_checkpoint_interval != 0u &&
      !has_initial_digest) {
    SetError("runner semantic digest is required for replay checkpoints");
    return false;
  }

  if (s_replay_source == kReplaySource_CanonicalHostTick) {
    if (strcmp(s_canonical_header.game_id, game_id) != 0) {
      SetError("canonical replay belongs to a different game");
      return false;
    }
    if ((s_canonical_header.flags &
         SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID) != 0u) {
      if (!has_initial_digest ||
          memcmp(s_canonical_header.initial_state_sha256,
                 initial_digest.sha256,
                 SR_INPUT_REPLAY_SHA256_SIZE) != 0) {
        SetError("canonical replay initial semantic state does not match");
        return false;
      }
    }
    if (has_input_state &&
        input_state.frame_counter !=
            s_canonical_header.start_frame_ordinal) {
      SetError("canonical replay start ordinal does not match runner state");
      return false;
    }
    s_next_ordinal = s_canonical_header.start_frame_ordinal;
  } else {
    s_next_ordinal = has_input_state ? input_state.frame_counter : 0u;
  }

  if (s_record_file) {
    SrInputReplayHeader header = {
      .struct_size = SR_INPUT_REPLAY_HEADER_V1_SIZE,
      .start_frame_ordinal = s_next_ordinal,
    };
    if (has_initial_digest) {
      header.flags |= SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID;
      memcpy(header.initial_state_sha256, initial_digest.sha256,
             sizeof(header.initial_state_sha256));
    }
    snprintf(header.game_id, sizeof(header.game_id), "%s", game_id);
    memset(&s_writer, 0, sizeof(s_writer));
    if (sr_input_replay_writer_begin(
            &s_writer, ReplayWrite, s_record_file, &header) != SR_RESULT_OK) {
      SetError("cannot write canonical replay header");
      s_writer_failed = true;
      return false;
    }
    s_writer_open = true;
  }
  s_session_started = true;
  return true;
}

static void LogGameFrame(unsigned game_frame) {
  if (s_game_frame_logging_enabled &&
      (snes_frame_counter % kGameFrameLogInterval) == 0) {
    fprintf(stderr, "[gflog] host=%d gf=%u\n",
            snes_frame_counter, game_frame);
  }

  const uint8_t game_mode = g_ram[kActRaiserWram_MapGroup];
  if (!s_action_entry_reported &&
      game_mode >= kActRaiserActionMapGroup_First &&
      game_mode <= kActRaiserActionMapGroup_Last) {
    s_action_entry_reported = true;
    fprintf(stderr,
            "[act-enter] $18=%02X $19=%02X at game-frame %u\n",
            game_mode, g_ram[kActRaiserWram_CurrentMap], game_frame);
  }
}

static uint32_t ResolveCanonical(uint32_t live_inputs,
                                 bool *stop_requested) {
  if (s_canonical_frame_index < s_canonical_frame_count) {
    const size_t index = s_canonical_frame_index++;
    const CanonicalReplayFrame *frame = &s_canonical_frames[index];
    if (frame->ordinal != s_next_ordinal) {
      SetError("canonical replay ordinal diverged from runner tick");
      *stop_requested = true;
      return live_inputs;
    }
    s_replay_started = true;
    s_pending_has_canonical_frame = true;
    s_pending_canonical_index = index;
    if (!s_live_after_replay && !s_disable_auto_stop &&
        s_canonical_frame_index == s_canonical_frame_count)
      *stop_requested = true;
    return frame->inputs;
  }

  if (s_live_after_replay) {
    if (!s_live_handoff_reported) {
      fprintf(stderr,
              "[input-replay] reached end of canonical recording at "
              "host tick %llu — handing control to live input\n",
              (unsigned long long)s_next_ordinal);
      s_live_handoff_reported = true;
    }
    return live_inputs;
  }
  if (s_disable_auto_stop && s_canonical_frame_count)
    return s_canonical_frames[s_canonical_frame_count - 1u].inputs;
  *stop_requested = true;
  return live_inputs;
}

static uint32_t ResolveLegacy(uint32_t live_inputs, unsigned game_frame,
                              bool *stop_requested) {
  uint32_t resolved_inputs = live_inputs;
  const bool replay_continuation =
      s_replay_started && game_frame > s_last_legacy_game_frame;
  const bool live_handoff = s_live_after_replay && replay_continuation;

  if (!live_handoff &&
      !(s_disable_auto_stop && replay_continuation) &&
      s_has_legacy_records &&
      game_frame <= s_maximum_legacy_game_frame)
    resolved_inputs = s_legacy_replay_inputs[game_frame];

  if (s_has_legacy_records && game_frame <= s_last_legacy_game_frame)
    s_replay_started = true;

  if (s_has_legacy_records && s_live_after_replay) {
    if (live_handoff && !s_live_handoff_reported) {
      fprintf(stderr,
              "[input-replay] reached end of legacy recording at gf=%u — "
              "handing control to live input\n", game_frame);
      s_live_handoff_reported = true;
    }
  } else if (s_has_legacy_records && !s_disable_auto_stop &&
             s_replay_started && game_frame >= s_last_legacy_game_frame) {
    fprintf(stderr,
            "[input-replay] reached end of legacy recording at gf=%u — "
            "stopping\n", game_frame);
    *stop_requested = true;
  }
  if (s_has_legacy_records && s_disable_auto_stop &&
      !s_live_after_replay && replay_continuation)
    resolved_inputs = s_legacy_replay_inputs[s_last_legacy_game_frame];
  return resolved_inputs;
}

InputReplayFrameResult InputReplay_Resolve(uint32_t live_inputs) {
  const unsigned game_frame = ReadGameFrame();
  bool stop_requested = false;
  uint32_t resolved_inputs = live_inputs;
  s_pending_has_canonical_frame = false;

  if (!s_session_started) {
    SetError("input replay session was not started");
    return (InputReplayFrameResult){
      .inputs = live_inputs,
      .stop_requested = true,
    };
  }
  if (s_tick_pending) {
    SetError("input replay tick completed out of order");
    return (InputReplayFrameResult){
      .inputs = live_inputs,
      .stop_requested = true,
    };
  }

  if (s_replay_source == kReplaySource_CanonicalHostTick)
    resolved_inputs = ResolveCanonical(live_inputs, &stop_requested);
  else if (s_replay_source == kReplaySource_LegacyGameFrame)
    resolved_inputs = ResolveLegacy(
        live_inputs, game_frame, &stop_requested);

  s_tick_pending = true;
  s_pending_ordinal = s_next_ordinal;
  s_pending_inputs = resolved_inputs;
  LogGameFrame(game_frame);
  return (InputReplayFrameResult){
    .inputs = resolved_inputs,
    .stop_requested = stop_requested,
  };
}

static bool CheckSemanticCheckpoint(
    const CanonicalReplayFrame *frame,
    const SrSemanticDigestResult *digest) {
  if (!frame->has_checkpoint ||
      (frame->checkpoint.flags &
       SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID) == 0u)
    return true;
  if (!digest ||
      frame->checkpoint.semantic_schema_version != digest->schema_version ||
      memcmp(frame->checkpoint.semantic_sha256, digest->sha256,
             SR_INPUT_REPLAY_SHA256_SIZE) != 0) {
    char message[kReplayErrorCapacity];
    snprintf(message, sizeof(message),
             "semantic checkpoint mismatch at host tick %llu",
             (unsigned long long)frame->ordinal);
    SetError(message);
    return false;
  }
  return true;
}

bool InputReplay_CompleteTick(SrRunnerHandle *runner) {
  if (!s_tick_pending) {
    SetError("input replay completion has no pending runner tick");
    return false;
  }

  SrInputStateSnapshot input_state;
  uint32_t effective_inputs = s_pending_inputs;
  const bool has_input_state = QueryInputState(runner, &input_state);
  if ((s_replay_source == kReplaySource_CanonicalHostTick || s_writer_open) &&
      !has_input_state) {
    SetError("runner input-state query failed during canonical replay");
    s_writer_failed = s_writer_open;
    return false;
  }
  if (has_input_state) {
    effective_inputs = (uint32_t)input_state.packed_buttons[0] |
        ((uint32_t)input_state.packed_buttons[1] << 12);
  }

  const CanonicalReplayFrame *expected =
      s_pending_has_canonical_frame
          ? &s_canonical_frames[s_pending_canonical_index] : NULL;
  const bool verify_semantic = expected && expected->has_checkpoint &&
      (expected->checkpoint.flags &
       SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID) != 0u;
  const bool write_semantic = s_writer_open && s_checkpoint_interval != 0u &&
      (s_pending_ordinal % s_checkpoint_interval) == 0u;
  SrSemanticDigestResult digest;
  const bool has_digest = (verify_semantic || write_semantic)
      ? QuerySemanticDigest(runner, &digest) : false;

  if (verify_semantic &&
      !CheckSemanticCheckpoint(expected, has_digest ? &digest : NULL))
    return false;

  if (s_writer_open) {
    SrInputReplayFrame frame = {
      .struct_size = SR_INPUT_REPLAY_FRAME_V1_SIZE,
      .frame_ordinal = s_pending_ordinal,
      .packed_buttons = {
        (uint16_t)(effective_inputs & UINT32_C(0x0fff)),
        (uint16_t)((effective_inputs >> 12) & UINT32_C(0x0fff)),
      },
    };
    if (sr_input_replay_writer_append_frame(&s_writer, &frame) !=
        SR_RESULT_OK) {
      SetError("cannot append canonical replay frame");
      s_writer_failed = true;
      return false;
    }
    if (write_semantic) {
      if (!has_digest) {
        SetError("runner semantic digest is unavailable for checkpoint");
        s_writer_failed = true;
        return false;
      }
      SrInputReplayCheckpoint checkpoint = {
        .struct_size = SR_INPUT_REPLAY_CHECKPOINT_V1_SIZE,
        .flags = SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID,
        .frame_ordinal = s_pending_ordinal,
        .semantic_schema_version = digest.schema_version,
      };
      memcpy(checkpoint.semantic_sha256, digest.sha256,
             sizeof(checkpoint.semantic_sha256));
      if (sr_input_replay_writer_append_checkpoint(
              &s_writer, &checkpoint) != SR_RESULT_OK) {
        SetError("cannot append canonical replay checkpoint");
        s_writer_failed = true;
        return false;
      }
    }
    if (fflush(s_record_file) != 0) {
      SetError("cannot flush canonical replay recording");
      s_writer_failed = true;
      return false;
    }
  }

  s_tick_pending = false;
  s_next_ordinal++;
  return true;
}

bool InputReplay_ShouldProtectSaveData(void) {
  return s_save_data_protected;
}

void InputReplay_Shutdown(void) {
  if (s_writer_open && !s_writer_failed) {
    if (sr_input_replay_writer_finish(&s_writer) != SR_RESULT_OK ||
        fflush(s_record_file) != 0)
      fprintf(stderr, "[input-record] could not finish canonical replay\n");
  }
  if (s_record_file) fclose(s_record_file);
  free(s_legacy_replay_inputs);
  free(s_canonical_frames);

  s_legacy_replay_inputs = NULL;
  s_canonical_frames = NULL;
  s_record_file = NULL;
  s_maximum_legacy_game_frame = 0;
  s_last_legacy_game_frame = 0;
  s_has_legacy_records = false;
  s_canonical_frame_count = 0;
  s_canonical_frame_capacity = 0;
  s_canonical_frame_index = 0;
  memset(&s_canonical_header, 0, sizeof(s_canonical_header));
  memset(&s_writer, 0, sizeof(s_writer));
  s_writer_open = false;
  s_writer_failed = false;
  s_replay_source = kReplaySource_None;
  s_replay_started = false;
  s_disable_auto_stop = false;
  s_live_after_replay = false;
  s_live_handoff_reported = false;
  s_save_data_protected = false;
  s_game_frame_logging_enabled = false;
  s_action_entry_reported = false;
  s_session_started = false;
  s_tick_pending = false;
  s_pending_ordinal = 0;
  s_pending_inputs = 0;
  s_pending_canonical_index = 0;
  s_pending_has_canonical_frame = false;
  s_next_ordinal = 0;
  s_checkpoint_interval = 0;
  s_last_error[0] = '\0';
  s_failed = false;
}
