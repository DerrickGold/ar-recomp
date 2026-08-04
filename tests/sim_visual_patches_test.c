#include <stdio.h>
#include <string.h>

#include "sim_visual_patches.h"

enum {
  kRomSize = 0xA843,
  kScript = 0xA838,
};

static int failures;

#define CHECK(expr) do {                                                    \
  if (!(expr)) {                                                            \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++;                                                             \
  }                                                                         \
} while (0)

static void SeedHouseFireScript(uint8_t rom[kRomSize]) {
  static const uint8_t script[] = {
    0x01, 0x2D, 0xDD,
    0x01, 0x33, 0xDD,
    0x01, 0x39, 0xDD,
    0xFE, 0x01,
  };
  memset(rom, 0, kRomSize);
  memcpy(&rom[kScript], script, sizeof(script));
}

int main(void) {
  uint8_t rom[kRomSize];
  SeedHouseFireScript(rom);
  CHECK(SimVisualPatches_Apply(rom, sizeof(rom)));
  CHECK(rom[kScript] == 4);
  CHECK(rom[kScript + 3] == 4);
  CHECK(rom[kScript + 6] == 4);
  CHECK(SimVisualPatches_Apply(rom, sizeof(rom)));

  /* A partial or unknown signature must not produce a partial patch. */
  SeedHouseFireScript(rom);
  rom[kScript + 3] = 2;
  CHECK(!SimVisualPatches_Apply(rom, sizeof(rom)));
  CHECK(rom[kScript] == 1);
  CHECK(rom[kScript + 3] == 2);
  CHECK(rom[kScript + 6] == 1);
  CHECK(!SimVisualPatches_Apply(rom, kScript));
  CHECK(!SimVisualPatches_Apply(NULL, sizeof(rom)));

  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("sim_visual_patches_test: PASS");
  return 0;
}
