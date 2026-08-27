#include "input_replay.h"

#include "byte_order.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "actraiser_game.h"
#include "snesrecomp/game/runtime.h"

enum {
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
static bool s_live_after_replay;
static bool s_live_handoff_reported;
static bool s_save_data_protected;
static bool s_game_frame_logging_enabled;
static bool s_action_entry_reported;

static unsigned ReadGameFrame(void) {
  return ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
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
    const uint32_t game_frame = ByteOrder_ReadLe32(record);
    const uint32_t inputs = ByteOrder_ReadLe32(record + sizeof(uint32_t));
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
  s_live_after_replay = getenv("AR_REPLAY_LIVE_AFTER_END") != NULL;
  if (s_live_after_replay && s_disable_auto_stop) {
    fprintf(stderr,
            "[input-replay] AR_REPLAY_LIVE_AFTER_END takes precedence over "
            "AR_REPLAY_NOSTOP\n");
  }
  s_game_frame_logging_enabled = getenv("AR_GFLOG") != NULL;
}

static void RecordFrame(unsigned game_frame, uint32_t inputs) {
  if (!s_record_file) return;

  uint8_t record[kReplayRecordByteCount];
  ByteOrder_WriteLe32(record, game_frame);
  ByteOrder_WriteLe32(record + sizeof(uint32_t), inputs);
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

InputReplayFrameResult InputReplay_Resolve(uint32_t live_inputs) {
  const unsigned game_frame = ReadGameFrame();
  uint32_t resolved_inputs = live_inputs;
  bool stop_requested = false;
  const bool replay_continuation =
      s_replay_started && game_frame > s_last_replay_game_frame;
  const bool live_handoff =
      s_live_after_replay && replay_continuation;

  if (!live_handoff &&
      !(s_disable_auto_stop && replay_continuation) &&
      s_replay_inputs && s_has_replay_records &&
      game_frame <= s_maximum_replay_game_frame) {
    resolved_inputs = s_replay_inputs[game_frame];
  }

  if (s_replay_inputs && s_has_replay_records) {
    if (game_frame <= s_last_replay_game_frame)
      s_replay_started = true;
  }

  if (s_replay_inputs && s_has_replay_records && s_live_after_replay) {
    if (live_handoff && !s_live_handoff_reported) {
      fprintf(stderr,
              "[input-replay] reached end of recording at gf=%u — "
              "handing control to live input\n",
              game_frame);
      s_live_handoff_reported = true;
    }
  } else if (s_replay_inputs && s_has_replay_records &&
             !s_disable_auto_stop) {
    if (s_replay_started && game_frame >= s_last_replay_game_frame) {
      fprintf(stderr,
              "[input-replay] reached end of recording at gf=%u — stopping\n",
              game_frame);
      stop_requested = true;
    }
  }
  if (s_replay_inputs && s_has_replay_records && s_disable_auto_stop &&
      !s_live_after_replay && replay_continuation) {
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
  s_maximum_replay_game_frame = 0;
  s_last_replay_game_frame = 0;
  s_has_replay_records = false;
  s_replay_started = false;
  s_disable_auto_stop = false;
  s_live_after_replay = false;
  s_live_handoff_reported = false;
  s_save_data_protected = false;
  s_game_frame_logging_enabled = false;
  s_action_entry_reported = false;
}
