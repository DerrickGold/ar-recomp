/* World-map underlay: ROM residency, the town window table, live-shadow
 * adoption policy, and the bake. The window table is the part that would fail
 * silently in play — a wrong origin puts a town on someone else's terrain and
 * still looks plausible — so its structural invariants are asserted here
 * rather than left to a visual checkpoint. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "sim_world_map.h"

static int s_failures;
#define CHECK(expression)                                                  \
  do {                                                                     \
    if (!(expression)) {                                                   \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
              #expression);                                                \
      s_failures++;                                                        \
    }                                                                      \
  } while (0)

enum {
  kRomSize = 0x80000,
  kTilemapOffset = 0x033341,
  kTilesOffset = 0x070000,
  kPaletteOffset = 0x0E3F93,
  kShadowWram = 0xC000,
  kWramSize = 0x20000,
};

/* Tile t is painted entirely with colour index t, and palette entry i is a
 * pure blue ramp, so a baked pixel names the tile it came from. */
static uint8_t *BuildRom(void) {
  uint8_t *rom = calloc(1, kRomSize);
  for (int tile = 0; tile < 256; tile++)
    memset(rom + kTilesOffset + tile * 64, (uint8_t)tile, 64);
  for (int i = 0; i < 256; i++) {
    uint16_t bgr555 = (uint16_t)((i & 0x1F) << 10);  /* blue = i & 31 */
    rom[kPaletteOffset + i * 2] = (uint8_t)(bgr555 & 0xFF);
    rom[kPaletteOffset + i * 2 + 1] = (uint8_t)(bgr555 >> 8);
  }
  for (int i = 0; i < kSimWorldMapBytes; i++)
    rom[kTilemapOffset + i] = (uint8_t)(i & 0x7F);
  return rom;
}

/* Colour BuildRom assigns to a tile: tile t is painted with palette index t,
 * and entry t is a pure blue ramp, so this reproduces ExpandBgr555 for that
 * ramp (red = green = 0). Independent of the module's bake path on purpose. */
static uint32_t ColorForTile(uint8_t tile) {
  uint32_t blue = (uint32_t)((tile & 0x1F) << 3) | ((tile & 0x1F) >> 2);
  return 0xFF000000u | blue;
}

/* A full, unconditional bake of `tilemap`, computed here rather than by the
 * module. Comparing the module's dirty-tracked bake against this catches both
 * failure modes: a tile that should have been re-baked but was not, and a
 * non-dirty tile left as whatever garbage the caller's buffer held. */
static void ReferenceFullBake(const uint8_t *tilemap, uint32_t *out) {
  for (int tile_y = 0; tile_y < kSimWorldMapTiles; tile_y++)
    for (int tile_x = 0; tile_x < kSimWorldMapTiles; tile_x++) {
      uint32_t color = ColorForTile(tilemap[tile_y * kSimWorldMapTiles + tile_x]);
      for (int row = 0; row < kSimWorldMapTilePixels; row++) {
        uint32_t *p = out +
            (size_t)(tile_y * kSimWorldMapTilePixels + row) * kSimWorldMapPixels +
            tile_x * kSimWorldMapTilePixels;
        for (int col = 0; col < kSimWorldMapTilePixels; col++) p[col] = color;
      }
    }
}

/* Mirror the module's adoption policy against a test-owned expected tilemap so
 * the reference bake can be computed from the same state the module holds: a
 * town frame adopts rows 8+, only a world-map frame adopts rows 0-7. */
static void ApplyRefreshToExpected(uint8_t *expected, const uint8_t *shadow,
                                   uint8_t map_number) {
  int first_row = (map_number == kActRaiserNonActionMap_WorldMap)
      ? 0 : kSimWorldMapVolatileRows;
  size_t offset = (size_t)first_row * kSimWorldMapTiles;
  memcpy(expected + offset, shadow + offset, kSimWorldMapBytes - offset);
}

static void TestUnavailableRom(void) {
  uint8_t tiny[16] = { 0 };
  CHECK(!SimWorldMap_Init(tiny, sizeof(tiny)));
  CHECK(!SimWorldMap_Available());
  CHECK(SimWorldMap_Serial() == 0);
  int x = -1, y = -1;
  /* The window table is static data, so it answers even with no ROM; what a
   * missing ROM must suppress is the serial, which is what gates drawing. */
  CHECK(SimWorldMap_OriginForTown(1, &x, &y));
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(!SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  free(pixels);
}

/* Every origin lands on a multiple of 16, the six windows fit inside the map
 * and never overlap. No other assignment of towns to the world map's six
 * cathedral icons has those properties, which is what pins this table. */
static void TestTownWindows(void) {
  int x[6], y[6];
  for (int town = 1; town <= 6; town++) {
    CHECK(SimWorldMap_OriginForTown((uint8_t)town, &x[town - 1],
                                    &y[town - 1]));
    CHECK(x[town - 1] % 16 == 0 && y[town - 1] % 16 == 0);
    CHECK(x[town - 1] >= 0 &&
          x[town - 1] + kSimTownCells <= kSimWorldMapTiles);
    CHECK(y[town - 1] >= 0 &&
          y[town - 1] + kSimTownCells <= kSimWorldMapTiles);
  }
  for (int a = 0; a < 6; a++)
    for (int b = a + 1; b < 6; b++) {
      bool disjoint_x = x[a] + kSimTownCells <= x[b] ||
          x[b] + kSimTownCells <= x[a];
      bool disjoint_y = y[a] + kSimTownCells <= y[b] ||
          y[b] + kSimTownCells <= y[a];
      CHECK(disjoint_x || disjoint_y);
    }
  CHECK(!SimWorldMap_OriginForTown(0, NULL, NULL));
  CHECK(!SimWorldMap_OriginForTown(7, NULL, NULL));

  /* Fillmore is the origin the whole table is anchored on: its cathedral
   * cell (13,13) under the world icon at (93,61). */
  CHECK(x[0] == 80 && y[0] == 48);
  /* Bloodpool abuts Fillmore, so its east edge is Fillmore's west edge —
   * this adjacency is the whole point of drawing live neighbours. */
  CHECK(x[1] + kSimTownCells == x[0] && y[1] == y[0]);
}

static void TestShadowAdoptionPolicy(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));
  uint32_t initial = SimWorldMap_Serial();
  CHECK(initial != 0);

  uint8_t *wram = calloc(1, kWramSize);
  memset(wram + kShadowWram, 0xAB, kSimWorldMapBytes);

  /* An action stage shares the address but not the meaning: never adopted. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_Fillmore, 1);
  CHECK(SimWorldMap_Serial() == initial);

  /* In a town, rows 8+ are adopted and rows 0-7 are left at the baseline. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  uint32_t after_town = SimWorldMap_Serial();
  CHECK(after_town != initial);
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  /* Row 0 still shows the ROM tilemap's tile 0; row 8 shows shadow tile
   * $AB. Blue channel carries the tile index in the synthetic palette. */
  CHECK((pixels[0] & 0xFF) == 0);
  uint32_t row8 = pixels[(size_t)kSimWorldMapVolatileRows *
                         kSimWorldMapTilePixels * kSimWorldMapPixels];
  CHECK((row8 & 0xFF) == ((0xAB & 0x1F) << 3 | (0xAB & 0x1F) >> 2));

  /* Idempotent: an unchanged shadow must not churn the serial, because the
   * present thread rebuilds a 1024x1024 texture on every change. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Serial() == after_town);

  /* Only the world map itself may publish rows 0-7. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  CHECK(SimWorldMap_Serial() != after_town);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK((pixels[0] & 0xFF) == ((0xAB & 0x1F) << 3 | (0xAB & 0x1F) >> 2));

  free(pixels);
  free(wram);
  free(rom);
}

/* The bake is fully opaque everywhere. There is deliberately no hole for the
 * town being played: punching one out left a black gap, because the hole is
 * the town's whole 512x512-pixel territory while its ground quad only draws
 * the window the camera can see. Overlap is a draw-order concern, not a bake
 * concern, and this asserts the bake keeps its half of that bargain. */
static void TestBakeIsFullyCovered(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));
  size_t count = (size_t)kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *pixels = malloc(count * 4);
  memset(pixels, 0, count * 4);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));

  size_t transparent = 0;
  for (size_t i = 0; i < count; i++)
    if ((pixels[i] >> 24) != 0xFF) transparent++;
  CHECK(transparent == 0);

  /* Every tile still bakes its own colour, including inside each town's
   * window -- that is the low-fidelity stand-in for off-screen territory. */
  for (int town = 1; town <= 6; town++) {
    int origin_x = 0, origin_y = 0;
    CHECK(SimWorldMap_OriginForTown((uint8_t)town, &origin_x, &origin_y));
    uint32_t pixel = pixels[(size_t)origin_y * kSimWorldMapTilePixels *
                            kSimWorldMapPixels +
                            origin_x * kSimWorldMapTilePixels];
    int tile = (origin_y * kSimWorldMapTiles + origin_x) & 0x7F;
    CHECK((pixel & 0xFF) == (uint32_t)((tile & 0x1F) << 3 | (tile & 0x1F) >> 2));
  }
  free(pixels);
  free(rom);
}

/* Per-tile dirty tracking (finding O2) must never change what a bake produces,
 * only how much work it does. The guardrail is the streaming-texture lock: the
 * caller hands the bake a write-only buffer of undefined content, so we bake
 * into a buffer deliberately pre-filled with garbage (0xAA) and assert the
 * result is byte-identical to a full reference bake. If the optimisation ever
 * baked only dirty tiles into that buffer, the non-dirty tiles would keep the
 * 0xAA garbage and these comparisons would fail. */
static void TestDirtyTrackingMatchesFullBake(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));

  size_t count = (size_t)kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *baked = malloc(count * 4);
  uint32_t *reference = malloc(count * 4);

  /* The tilemap the module holds right after Init is BuildRom's `i & 0x7F`. */
  uint8_t *expected = malloc(kSimWorldMapBytes);
  for (int i = 0; i < kSimWorldMapBytes; i++)
    expected[i] = (uint8_t)(i & 0x7F);

  /* (2) First bake after Init: all tiles dirty, so a bake into garbage memory
   * must still reproduce the whole ROM tilemap. */
  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);

  /* (1) A single-tile Refresh, then a bake into garbage, must match a full
   * bake of the new state — proving the 16383 non-dirty tiles were preserved
   * from the persistent image rather than left as the 0xAA the caller's
   * write-only lock would really contain. */
  uint8_t *wram = calloc(1, kWramSize);
  for (int i = 0; i < kSimWorldMapBytes; i++)
    wram[kShadowWram + i] = (uint8_t)(i & 0x7F);      /* start == baseline */
  /* Change exactly one tile in an adopted row (row 8, well clear of the
   * volatile rows 0-7) to a value the baseline never uses there. */
  size_t edited = (size_t)kSimWorldMapVolatileRows * kSimWorldMapTiles + 5;
  wram[kShadowWram + edited] = 0x33;
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  expected[edited] = 0x33;

  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);
  /* And the edited tile really did land — a sanity check that the comparison
   * above is not vacuously passing on an unchanged image. */
  {
    size_t px = (size_t)(edited / kSimWorldMapTiles) * kSimWorldMapTilePixels *
                kSimWorldMapPixels +
                (edited % kSimWorldMapTiles) * kSimWorldMapTilePixels;
    CHECK(baked[px] == ColorForTile(0x33));
  }

  /* (3) A mixed sequence of several Refreshes, then one bake, must also match a
   * full bake of the final state. Includes a world-map frame so rows 0-7 move
   * too, exercising the volatile-row offset in the diff. */
  memset(wram + kShadowWram, 0xAB, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);        /* rows 8+ */
  ApplyRefreshToExpected(expected, wram + kShadowWram, 1);
  wram[kShadowWram + 100] = 0x07;
  wram[kShadowWram + 4000] = 0x71;
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  ApplyRefreshToExpected(expected, wram + kShadowWram, 1);
  memset(wram + kShadowWram, 0x5C, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);              /* rows 0+ */
  ApplyRefreshToExpected(expected, wram + kShadowWram,
                         kActRaiserNonActionMap_WorldMap);

  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);

  /* A bake with no intervening Refresh (nothing dirty) still emits the full
   * image, because the caller's buffer is fresh garbage every frame. */
  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  CHECK(memcmp(baked, reference, count * 4) == 0);

  free(expected);
  free(wram);
  free(reference);
  free(baked);
  free(rom);
}

/* The blur mip must be a box average of the SAME image Bake emits, computed
 * from the module's persistent copy rather than from the caller's buffer.
 *
 * The predecessor built this mip by reading back the streaming-texture lock it
 * had just baked into — legal-looking on Metal, garbage on a Vulkan/Mesa
 * backend where the mapping is write-combined (that is the macOS-fine /
 * Steam-Deck-garbled split). The test that catches a reintroduction is one that
 * hands Downsample a destination full of garbage and byte-compares against an
 * independent reference: if the implementation ever reads its own destination,
 * or reads anything other than the baked image, the comparison fails.
 *
 * Also asserts the pitch is honored, since the destination's pitch comes from
 * SDL and need not equal the extent. */
static void TestDownsampleMatchesBake(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));

  const int count = kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *baked = malloc((size_t)count * 4);
  const int divisor = 4;
  const int extent = kSimWorldMapPixels / divisor;
  /* Over-wide destination: pitch deliberately != extent. */
  const int pitch = extent + 7;
  uint32_t *mip = malloc((size_t)pitch * extent * 4);
  uint32_t *reference = malloc((size_t)extent * extent * 4);

  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));

  /* Independent box average over the baked image. */
  for (int y = 0; y < extent; y++) {
    for (int x = 0; x < extent; x++) {
      uint32_t a = 0, r = 0, g = 0, b = 0;
      for (int sy = 0; sy < divisor; sy++) {
        for (int sx = 0; sx < divisor; sx++) {
          uint32_t texel =
              baked[(size_t)(y * divisor + sy) * kSimWorldMapPixels +
                    (size_t)x * divisor + sx];
          a += (texel >> 24) & 0xFF; r += (texel >> 16) & 0xFF;
          g += (texel >> 8) & 0xFF;  b += texel & 0xFF;
        }
      }
      const uint32_t taps = (uint32_t)(divisor * divisor);
      reference[(size_t)y * extent + x] =
          ((a / taps) << 24) | ((r / taps) << 16) | ((g / taps) << 8) |
          (b / taps);
    }
  }

  memset(mip, 0xAA, (size_t)pitch * extent * 4);
  CHECK(SimWorldMap_Downsample(mip, pitch, divisor));
  for (int y = 0; y < extent; y++) {
    CHECK(memcmp(mip + (size_t)y * pitch, reference + (size_t)y * extent,
                 (size_t)extent * 4) == 0);
  }

  /* Repeating it with nothing dirty must reproduce the same mip — the caller's
   * buffer is fresh garbage every frame, exactly as for Bake. */
  memset(mip, 0x5C, (size_t)pitch * extent * 4);
  CHECK(SimWorldMap_Downsample(mip, pitch, divisor));
  for (int y = 0; y < extent; y++) {
    CHECK(memcmp(mip + (size_t)y * pitch, reference + (size_t)y * extent,
                 (size_t)extent * 4) == 0);
  }

  /* Argument validation: a divisor that does not divide the extent, a
   * nonsensical divisor, a short pitch, and a NULL destination. */
  CHECK(!SimWorldMap_Downsample(mip, pitch, 3));
  CHECK(!SimWorldMap_Downsample(mip, pitch, 0));
  CHECK(!SimWorldMap_Downsample(mip, extent - 1, divisor));
  CHECK(!SimWorldMap_Downsample(NULL, pitch, divisor));

  free(reference);
  free(mip);
  free(baked);
  free(rom);
}

int main(void) {
  TestUnavailableRom();
  TestTownWindows();
  TestShadowAdoptionPolicy();
  TestBakeIsFullyCovered();
  TestDirtyTrackingMatchesFullBake();
  TestDownsampleMatchesBake();
  SimWorldMap_Shutdown();
  printf("sim world map tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
