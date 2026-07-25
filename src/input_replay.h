#ifndef INPUT_REPLAY_H
#define INPUT_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct InputReplayFrameResult {
  uint32_t inputs;
  bool stop_requested;
} InputReplayFrameResult;

/* Load AR_INPUT_REPLAY and open AR_INPUT_RECORD, when configured. */
void InputReplay_Init(void);

/* Apply replay input for the current logical game frame, record the resolved
 * value, and report whether playback reached its final recorded frame. */
InputReplayFrameResult InputReplay_Resolve(uint32_t live_inputs);

/* Replay runs must never persist SRAM, even if the configured file failed to
 * open: the environment setting itself marks the session as diagnostic. */
bool InputReplay_ShouldProtectSaveData(void);

void InputReplay_Shutdown(void);

#endif /* INPUT_REPLAY_H */
