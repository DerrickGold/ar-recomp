#ifndef AR_IMPORTED_GAME_SESSION_H
#define AR_IMPORTED_GAME_SESSION_H
#include "actraiser2/imported_game_api.h"

/* One worker owns the imported runtime and its coroutine for the entire
 * visit. Loading is asynchronous; gameplay/snapshots are acknowledged calls.
 * GPU, audio playback, input and all window events remain on the host. */
typedef enum ImportedGameState {
  kImportedGame_Loading, kImportedGame_Ready, kImportedGame_Failed
} ImportedGameState;
bool ImportedGameSession_Start(const uint8_t *rom, size_t size, uint8_t stage,
                                int extra, bool diorama);
ImportedGameState ImportedGameSession_Poll(ArImportedGameFrame *frame, unsigned *startup_ticks);
bool ImportedGameSession_Step(uint32_t input, int extra, bool diorama, ArImportedGameFrame *frame);
bool ImportedGameSession_Snapshot(const char *prefix);
const char *ImportedGameSession_Error(void);
void ImportedGameSession_Cancel(void);
void ImportedGameSession_Close(void);
void ImportedGameSession_EndSession(void);
#endif
