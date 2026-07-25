#include "input_replay.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common_rtl.h"

enum {
  kGameFrameLowAddress = 0x88,
  kGameFrameHighAddress = 0x89,
  kGameModeAddress = 0x18,
  kCurrentMapAddress = 0x19,
  kFirstActionMode = 0x01,
  kLastActionMode = 0x07,
  kReplayRecordByteCount = 8,
  kReplayFrameCapacity = UINT16_MAX + 1u,
  kGameFrameLogInterval = 100,
};

static uint32_t *s_replay_inputs;
static FILE *s_record_file;
static uint32_t s_maximum_replay_game_frame;
static uint32_t s_last_replay_game_frame;
static bool s_has_replay_records;
static bool s_replay_started;
static bool s_disable_auto_stop;
static bool s_save_data_protected;
static bool s_game_frame_logging_enabled;
static bool s_action_entry_reported;

static uint32_t ReadLittleEndian32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] |
         ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) |
         ((uint32_t)bytes[3] << 24);
}

static void WriteLittleEndian32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static unsigned ReadGameFrame(void) {
  return (unsigned)g_ram[kGameFrameLowAddress] |
         ((unsigned)g_ram[kGameFrameHighAddress] << 8);
}

static void LoadReplayFile(const char *path) {
  FILE *replay_file = fopen(path, "rb");
  if (!replay_file) {
    fprintf(stderr, "[input-replay] cannot open %s\n", path);
    return;
  }

  s_replay_inputs =
      (uint32_t *)calloc(kReplayFrameCapacity, sizeof(*s_replay_inputs));
  if (!s_replay_inputs) {
    fprintf(stderr, "[input-replay] cannot allocate replay table\n");
    fclose(replay_file);
    return;
  }

  uint8_t record[kReplayRecordByteCount];
  size_t record_count = 0;
  size_t invalid_frame_count = 0;
  while (fread(record, 1, sizeof(record), replay_file) == sizeof(record)) {
    record_count++;
    const uint32_t game_frame = ReadLittleEndian32(record);
    const uint32_t inputs = ReadLittleEndian32(record + sizeof(uint32_t));
    if (game_frame >= kReplayFrameCapacity) {
      invalid_frame_count++;
      continue;
    }
    s_replay_inputs[game_frame] = inputs;
    if (!s_has_replay_records ||
        game_frame > s_maximum_replay_game_frame) {
      s_maximum_replay_game_frame = game_frame;
    }
    s_last_replay_game_frame = game_frame;
    s_has_replay_records = true;
  }
  if (ferror(replay_file))
    fprintf(stderr, "[input-replay] read failed for %s\n", path);
  fclose(replay_file);

  fprintf(stderr,
          "[input-replay] %zu records, max gf=%u last gf=%u from %s\n",
          record_count,
          s_has_replay_records ? s_maximum_replay_game_frame : 0,
          s_has_replay_records ? s_last_replay_game_frame : 0, path);
  if (invalid_frame_count) {
    fprintf(stderr,
            "[input-replay] ignored %zu record(s) outside 16-bit "
            "game-frame range\n",
            invalid_frame_count);
  }
}

static void OpenRecordFile(const char *path) {
  s_record_file = fopen(path, "wb");
  if (!s_record_file) {
    fprintf(stderr, "[input-record] cannot open %s\n", path);
    return;
  }
  fprintf(stderr, "[input-record] -> %s\n", path);
}

void InputReplay_Init(void) {
  const char *replay_path = getenv("AR_INPUT_REPLAY");
  s_save_data_protected = replay_path != NULL;
  if (replay_path && replay_path[0])
    LoadReplayFile(replay_path);

  const char *record_path = getenv("AR_INPUT_RECORD");
  if (record_path && record_path[0])
    OpenRecordFile(record_path);

  s_disable_auto_stop = getenv("AR_REPLAY_NOSTOP") != NULL;
  s_game_frame_logging_enabled = getenv("AR_GFLOG") != NULL;
}

static void RecordFrame(unsigned game_frame, uint32_t inputs) {
  if (!s_record_file) return;

  uint8_t record[kReplayRecordByteCount];
  WriteLittleEndian32(record, game_frame);
  WriteLittleEndian32(record + sizeof(uint32_t), inputs);
  if (fwrite(record, 1, sizeof(record), s_record_file) != sizeof(record) ||
      fflush(s_record_file) != 0) {
    fprintf(stderr, "[input-record] write failed; recording stopped\n");
    fclose(s_record_file);
    s_record_file = NULL;
  }
}

static void LogGameFrame(unsigned game_frame) {
  if (s_game_frame_logging_enabled &&
      (snes_frame_counter % kGameFrameLogInterval) == 0) {
    fprintf(stderr, "[gflog] host=%d gf=%u\n",
            snes_frame_counter, game_frame);
  }

  const uint8_t game_mode = g_ram[kGameModeAddress];
  if (!s_action_entry_reported &&
      game_mode >= kFirstActionMode &&
      game_mode <= kLastActionMode) {
    s_action_entry_reported = true;
    fprintf(stderr,
            "[act-enter] $18=%02X $19=%02X at game-frame %u\n",
            game_mode, g_ram[kCurrentMapAddress], game_frame);
  }
}

InputReplayFrameResult InputReplay_Resolve(uint32_t live_inputs) {
  const unsigned game_frame = ReadGameFrame();
  uint32_t resolved_inputs = live_inputs;
  bool stop_requested = false;

  if (s_replay_inputs && s_has_replay_records &&
      game_frame <= s_maximum_replay_game_frame) {
    resolved_inputs = s_replay_inputs[game_frame];
  }

  if (s_replay_inputs && s_has_replay_records && !s_disable_auto_stop) {
    if (game_frame <= s_last_replay_game_frame)
      s_replay_started = true;
    if (s_replay_started && game_frame >= s_last_replay_game_frame) {
      fprintf(stderr,
              "[input-replay] reached end of recording at gf=%u — stopping\n",
              game_frame);
      stop_requested = true;
    }
  }
  if (s_replay_inputs && s_has_replay_records && s_disable_auto_stop &&
      game_frame > s_maximum_replay_game_frame) {
    resolved_inputs = s_replay_inputs[s_last_replay_game_frame];
  }

  RecordFrame(game_frame, resolved_inputs);
  LogGameFrame(game_frame);
  return (InputReplayFrameResult){
      .inputs = resolved_inputs,
      .stop_requested = stop_requested,
  };
}

bool InputReplay_ShouldProtectSaveData(void) {
  return s_save_data_protected;
}

void InputReplay_Shutdown(void) {
  if (s_record_file) fclose(s_record_file);
  free(s_replay_inputs);
  s_record_file = NULL;
  s_replay_inputs = NULL;
}
