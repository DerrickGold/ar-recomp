/* Local-ROM check of asynchronous loading, owner-thread calls, cancellation
 * during startup, and repeated visits through the production host adapter. */
#include "host/imported_game_session.h"
#include <SDL3/SDL.h>
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void CheckSnapshotUnchanged(void) {
  const char *suffixes[] = {"cpu.json", "ppu.json", "wram.bin", "vram.bin", "cgram.bin",
                            "oam.bin", "highoam.bin"};
  for (unsigned i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
    char before[128], after[128];
    snprintf(before, sizeof(before), "ar2-refresh-before.%s", suffixes[i]);
    snprintf(after, sizeof(after), "ar2-refresh-after.%s", suffixes[i]);
    FILE *a = fopen(before, "rb"), *b = fopen(after, "rb");
    assert(a && b);
    int c;
    do { c = fgetc(a); assert(c == fgetc(b)); } while (c != EOF);
    assert(!ferror(a) && !ferror(b));
    fclose(a); fclose(b);
    remove(before); remove(after);
  }
}

static void RefreshWhileFrozen(ArImportedGameFrame *frame) {
  const ArImportedGameFrame previous = *frame;
  assert(ImportedGameSession_Snapshot("ar2-refresh-before"));
  const ArImportedGameView views[] = {{0, 0, false}, {128, 64, false}, {51, 64, true}};
  for (unsigned i = 0; i < sizeof(views) / sizeof(*views); ++i) {
    assert(ImportedGameSession_Refresh(views[i], frame));
    assert(frame->frame == previous.frame && frame->scene == previous.scene);
    assert(frame->player_x == previous.player_x && frame->player_y == previous.player_y);
    assert(frame->health == previous.health && frame->lives == previous.lives);
    assert(frame->pcm == NULL && frame->pcm_frames == 0);
    assert(frame->video.display_extra == views[i].extra_columns);
    assert(frame->video.width == (views[i].diorama ? 512 : 256 + 2 * views[i].extra_columns));
    assert(frame->video.height == 224 + frame->video.extra_top + frame->video.extra_bottom);
  }
  assert(ImportedGameSession_Snapshot("ar2-refresh-after"));
  CheckSnapshotUnchanged();
}

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
    assert(ImportedGameSession_Start(rom, 0x180000, stage, (ArImportedGameView){128, 64, true}));
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
      for (int tick = 0; tick < 30; tick++) {
        if (tick == 0 || tick == 15) RefreshWhileFrozen(&frame);
        assert(ImportedGameSession_Step(0,
            (ArImportedGameView){tick < 15 ? 0 : 128, 64, tick >= 15}, &frame));
      }
      assert(frame.frame == first + 30 && frame.video.level);
    }
    ImportedGameSession_Close();
  }
  assert(ImportedGameSession_Start(rom, 0x180000, 0x18, (ArImportedGameView){0}));
  ArImportedGameFrame frame;
  unsigned ticks;
  assert(Wait(&frame, &ticks) == kImportedGame_Failed);
  assert(ImportedGameSession_Error()[0]);
  assert(!ImportedGameSession_Step(0, (ArImportedGameView){0}, &frame));
  ImportedGameSession_EndSession();
  free(rom);
  puts("asynchronous startup, cancellation, owner-thread stepping and repeated visits passed");
  return 0;
}
