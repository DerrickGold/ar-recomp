#ifndef AR_GAME_LOOP_H
#define AR_GAME_LOOP_H
/* Main-thread event routing, emulation ticks, host pauses and presentation.
 * Called by game_session with its services live; owns no boot or teardown. */

#include "app/game_session.h"

void GameLoop_Run(const GameSessionConfig *config);

#endif /* AR_GAME_LOOP_H */
