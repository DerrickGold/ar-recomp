/* Local-ROM check of asynchronous loading, owner-thread calls, cancellation
 * during startup, and repeated visits through the production host adapter. */
#include "host/imported_game_session.h"
#include <SDL3/SDL.h>
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>

static ImportedGameState Wait(ArImportedGameFrame *frame, unsigned *ticks) {
  const uint64_t deadline = SDL_GetTicks() + 30000;
  ImportedGameState state;
  do {
    state = ImportedGameSession_Poll(frame, ticks);
    assert(SDL_GetTicks() < deadline);
    if (state == kImportedGame_Loading) SDL_Delay(1);
  } while (state == kImportedGame_Loading);
  return state;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *file = fopen(argv[1], "rb");
  uint8_t *rom = malloc(0x180000);
  assert(file && rom && fread(rom, 1, 0x180000, file) == 0x180000);
  fclose(file);
  for (int visit = 0; visit < 3; visit++) {
    const uint8_t stage = visit == 1 ? 0x16 : 0x14;
    assert(ImportedGameSession_Start(rom, 0x180000, stage, 128, true));
    if (!visit) {
      SDL_Delay(5);
      ImportedGameSession_Cancel();
    } else {
      ArImportedGameFrame frame;
      unsigned ticks;
      const ImportedGameState state = Wait(&frame, &ticks);
      if (state != kImportedGame_Ready) fprintf(stderr, "%s\n", ImportedGameSession_Error());
      assert(state == kImportedGame_Ready && ticks > 0 && frame.scene == stage);
      const unsigned first = frame.frame;
      for (int tick = 0; tick < 30; tick++)
        assert(ImportedGameSession_Step(0, tick < 15 ? 0 : 128, tick >= 15, &frame));
      assert(frame.frame == first + 30 && frame.video.level);
    }
    ImportedGameSession_Close();
  }
  assert(ImportedGameSession_Start(rom, 0x180000, 0x18, 0, false));
  ArImportedGameFrame frame;
  unsigned ticks;
  assert(Wait(&frame, &ticks) == kImportedGame_Failed);
  assert(ImportedGameSession_Error()[0]);
  assert(!ImportedGameSession_Step(0, 0, false, &frame));
  ImportedGameSession_EndSession();
  free(rom);
  puts("asynchronous startup, cancellation, owner-thread stepping and repeated visits passed");
  return 0;
}
