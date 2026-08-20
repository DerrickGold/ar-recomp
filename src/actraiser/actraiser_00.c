#include "cpu_state.h"
#include "funcs.h"
#include "actraiser_game.h"

enum {
  kSpriteScratchSlotCount = 128,
  kSpriteScratchFirstY = 0x0201,
  kSpriteScratchRecordBytes = 4,
  kSpriteHiddenY = 0xF0,
};

void ResetSpritesFunc(int wh) {
  for (; wh < kSpriteScratchSlotCount; wh++)
    g_ram[kSpriteScratchFirstY + wh * kSpriteScratchRecordBytes] =
        kSpriteHiddenY;
}
