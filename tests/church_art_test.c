#include "sim/church/church_art.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  assert(!ChurchArt_Init(NULL, 0) && !ChurchArt_ColumnPixels());
  uint8_t *rom = calloc(1, 0x100000);
  assert(rom);
  assert(!ChurchArt_Init(rom, 0xe3eb3 + 31));
  const uint8_t script[] = {'S',  'Y',  0,    0,    7,    0x40, 0, 0x80, 0, 0x93,
                            0x3e, 0x0e, 0x80, 0x80, 0x20, 0,    0, 0xc0, 6, 0};
  memcpy(rom + 0x28000, script, sizeof(script));
  /* Native 4bpp indices, right-hand flip, warm palette and rounded cutout.
   * A single pixel in tile 3's left edge appears in the mirrored right half
   * at x=21 and in the symmetric left half at x=6. */
  rom[0x6c000 + 3 * 32 + 6 * 2] = 0x80;
  rom[0xe3eb3 + 2] = 22;
  assert(ChurchArt_Init(rom, 0x100000));
  const uint32_t *art = ChurchArt_ColumnPixels();
  assert(art && art[6 * 28 + 21] == 0xffb50000 && art[6 * 28 + 6] == 0xffb50000);
  assert(!art[0] && !art[27]);
  for (int y = 0; y < 14; y++)
    for (int x = 0; x < 28; x++) {
      assert(art[y * 28 + x] == art[y * 28 + 27 - x]);
      assert(art[y * 28 + x] == art[(143 - y) * 28 + x]);
    }
  /* Session reload cannot reuse the prior ROM's pixels. */
  memset(rom + 0x6c000, 0, 0x4000);
  assert(ChurchArt_Init(rom, 0x100000));
  assert(!ChurchArt_ColumnPixels()[6 * 28 + 21]);
  /* A relocated palette declaration is followed; malformed pointers fail. */
  rom[0x28000 + 9] = 0x53;
  rom[0xe3e73 + 2] = 31;
  rom[0x6c000 + 3 * 32 + 6 * 2] = 0x80;
  assert(ChurchArt_Init(rom, 0x100000));
  assert(ChurchArt_ColumnPixels()[6 * 28 + 21] == 0xffff0000);
  rom[0x28000 + 11] = 0xff;
  assert(!ChurchArt_Init(rom, 0x100000) && !ChurchArt_ColumnPixels());
  assert(!ChurchArt_Init(NULL, 0) && !ChurchArt_ColumnPixels());
  free(rom);
  puts("church native art: bounds, decoding, symmetry and session reset passed");
  return 0;
}
