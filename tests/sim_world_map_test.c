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

int main(void) {
  TestUnavailableRom();
  TestTownWindows();
  TestShadowAdoptionPolicy();
  TestBakeIsFullyCovered();
  SimWorldMap_Shutdown();
  printf("sim world map tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
