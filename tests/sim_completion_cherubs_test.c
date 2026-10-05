#include "sim/world_nav/sim_completion_cherubs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kCharacters = 0x68000, kBytes = kCharacters + 512 * 32 };
static const unsigned kPoses[] = {0xA627, 0xA63C, 0xA651};

int main(void) {
  uint8_t *rom = calloc(kBytes, 1);
  assert(rom);
  const uint8_t parts[4][5] = {
      {0, 0, 0, 6, 0}, {0, 8, 0, 7, 0}, {0, 0, 8, 0x16, 0}, {0, 7, 8, 0x16, 0x40}};
  for (unsigned frame = 0; frame < 3; frame++) {
    const unsigned pose = kPoses[frame];
    rom[pose] = 4;
    memcpy(rom + pose + 1, parts, sizeof(parts));
    for (unsigned part = 0; part < 4; part++)
      rom[pose + 3 + part * 5] += frame;
  }
  /* Ink on different bitplanes, transparent zero, and the native mirrored
   * lower-wing part. Both lower poses extend below the first 16px frame. */
  rom[kCharacters + 6 * 32 + 2] = 0x20;
  rom[kCharacters + 7 * 32 + 4 + 17] = 0x08;
  rom[kCharacters + 0x16 * 32 + 6 + 16] = 0x40;
  uint32_t pixels[kSimCompletionCherubAtlasWidth * kSimCompletionCherubAtlasHeight], revision;
  assert(!SimCompletionCherubs_Copy(pixels, &revision));
  assert(SimCompletionCherubs_Init(rom, kBytes));
  assert(SimCompletionCherubs_Copy(pixels, &revision) && revision);
  for (unsigned frame = 0; frame < 3; frame++) {
    unsigned coverage = 0;
    for (unsigned y = 0; y < kSimCompletionCherubHeight; y++)
      for (unsigned x = 0; x < kSimCompletionCherubWidth; x++) {
        const bool ink = (x == 2 && y == 1 + frame) || (x == 12 && y == 2 + frame) ||
                         ((x == 1 || x == 13) && y == 11 + frame);
        const uint32_t p =
            pixels[y * kSimCompletionCherubAtlasWidth + frame * kSimCompletionCherubWidth + x];
        assert(p == (ink ? UINT32_C(0xffffffff) : 0));
        coverage += !!p;
      }
    assert(coverage == 4);
  }
  const uint32_t original = revision;
  assert(SimCompletionCherubs_Init(rom, kBytes));
  assert(SimCompletionCherubs_Copy(pixels, &revision) && revision == original);
  /* A ROM change invalidates uploads; retirement/malformed art must never
   * leave the previous session's cherubs available. */
  rom[kCharacters + 6 * 32 + 2] ^= 0x40;
  assert(SimCompletionCherubs_Init(rom, kBytes));
  assert(SimCompletionCherubs_Copy(pixels, &revision) && revision != original);
  rom[kPoses[0] + 3] = 255;
  assert(!SimCompletionCherubs_Init(rom, kBytes));
  assert(!SimCompletionCherubs_Copy(pixels, &revision));
  rom[kPoses[0] + 3] = 0;
  assert(!SimCompletionCherubs_Init(rom, kBytes - 1));
  assert(!SimCompletionCherubs_Copy(pixels, &revision));
  assert(!SimCompletionCherubs_Init(NULL, 0));
  free(rom);
  puts("Completion cherubs: native offsets, flips, bitplane coverage, bounds and retirement PASS");
  return 0;
}
