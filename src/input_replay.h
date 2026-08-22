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
 * value, and report whether playback reached its final recorded frame.
 * AR_REPLAY_LIVE_AFTER_END=1 hands later frames back to live input instead of
 * stopping; when AR_INPUT_RECORD is also set, the output is a complete replay
 * prefix plus the live continuation. */
InputReplayFrameResult InputReplay_Resolve(uint32_t live_inputs);

/* Replay runs must never persist SRAM, even if the configured file failed to
 * open: the environment setting itself marks the session as diagnostic. */
bool InputReplay_ShouldProtectSaveData(void);

void InputReplay_Shutdown(void);

#endif /* INPUT_REPLAY_H */
