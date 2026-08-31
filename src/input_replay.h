#ifndef INPUT_REPLAY_H
#define INPUT_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp/runner/base.h"

typedef struct InputReplayFrameResult {
  uint32_t inputs;
  bool stop_requested;
} InputReplayFrameResult;

/* Load AR_INPUT_REPLAY and open AR_INPUT_RECORD, when configured. Legacy
 * ActRaiser game-frame recordings remain readable; new recordings use the
 * canonical runner replay container. */
void InputReplay_Init(void);

/* Bind the loaded/recording artifact to the runner state after boot-time
 * savestate loading is complete. This validates a canonical replay's game and
 * initial-state identity and writes the header for a new recording. */
bool InputReplay_BeginSession(SrRunnerHandle *runner, const char *game_id);

/* Resolve one runner tick's input and report whether playback reached its
 * final recorded tick. A matching CompleteTick call must follow a successfully
 * executed RtlRunFrame; new recordings are committed there from the runner's
 * effective input state.
 *
 * Legacy artifacts continue to select input by the current logical game frame.
 * AR_REPLAY_LIVE_AFTER_END=1 hands later frames back to live input instead of
 * stopping; when AR_INPUT_RECORD is also set, the output is a complete replay
 * prefix plus the live continuation. */
InputReplayFrameResult InputReplay_Resolve(uint32_t live_inputs);
bool InputReplay_CompleteTick(SrRunnerHandle *runner);

/* Stable diagnostic text for a failed BeginSession/CompleteTick operation. */
bool InputReplay_Failed(void);
const char *InputReplay_LastError(void);

/* Replay runs must never persist SRAM, even if the configured file failed to
 * open: the environment setting itself marks the session as diagnostic. */
bool InputReplay_ShouldProtectSaveData(void);

void InputReplay_Shutdown(void);

#endif /* INPUT_REPLAY_H */
