/* Optional local-ROM test: native ABI, restart, audio and symbol isolation.
 * Usage: imported_game_module_test <module> [player-supplied AR2 ROM]. */
#include "actraiser2/imported_game_api.h"
#include <SDL3/SDL.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  assert(argc >= 2);
  SDL_SharedObject *module = SDL_LoadObject(argv[1]);
  if (!module) {
    fprintf(stderr, "%s\n", SDL_GetError());
    return 1;
  }
  assert(!SDL_LoadFunction(module, "RtlRunFrame"));
  assert(!SDL_LoadFunction(module, "sr_native_module_abort"));
  ArImportedGameGetApi get =
      (ArImportedGameGetApi)SDL_LoadFunction(module, "ArImportedGame_GetApi");
  assert(get && !get(0) && !get(AR_IMPORTED_GAME_ABI - 1));
  const ArImportedGameApi *api = get(AR_IMPORTED_GAME_ABI);
  assert(api && api->size == sizeof(*api));
  assert(!api->open(NULL, 0, 0x14));
  if (argc > 2) {
    FILE *file = fopen(argv[2], "rb");
    assert(file);
    uint8_t *rom = malloc(0x180000);
    assert(rom && fread(rom, 1, 0x180000, file) == 0x180000);
    fclose(file);
    for (int visit = 0; visit < 3; visit++) {
      uint8_t stage = visit == 1 ? 0x16 : 0x14;
      assert(!api->open(rom, 0x180000, 0x18));
      assert(api->open(rom, 0x180000, stage));
      bool active = false, audible = false, moved = false;
      uint16_t walk_start = 0;
      /* The native entrance animation initially locks player movement. */
      enum { kWalkStart = 550, kWalkEnd = 650 };
      for (int tick = 0; tick < 900; tick++) {
        ArImportedGameFrame frame;
        if (!api->step(tick > kWalkStart && tick < kWalkEnd ? 0x80 : 0, 64, tick > 600, &frame)) {
          fprintf(stderr, "%s at tick %d\n", api->error(), tick);
          return 1;
        }
        assert(frame.event == kArImportedGameEvent_None);
        assert(frame.pcm_frames >= 798 && frame.pcm_frames <= 799);
        active |= frame.scene == stage && frame.video.level && frame.health > 0;
        if (tick == kWalkStart) {
          assert(frame.scene == stage && frame.video.level);
          walk_start = frame.player_x;
        }
        if (tick > kWalkStart && tick < kWalkEnd && frame.player_x > walk_start + 8) moved = true;
        for (unsigned i = 0; i < frame.pcm_frames * 2; i++)
          audible |= frame.pcm[i] != 0;
      }
      assert(active && audible && moved);
      api->close();
      fprintf(stderr, "visit %d: scene %02x, movement, native + diorama captures, PCM verified\n",
              visit, stage);
    }
    free(rom);
  }
  api->close();
  SDL_UnloadObject(module);
  return 0;
}
