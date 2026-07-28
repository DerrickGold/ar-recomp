/* World-map underlay: ROM residency, the town window table, publication of an
 * owned complete tilemap, dirty tracking, and baking/downsampling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  /* Must cover kPaletteOffset + 512, not just the tile art. */
  kRomSize = 0x100000,
  kTilemapOffset = 0x033341,
  kTilesOffset = 0x070000,
  kPaletteOffset = 0x0E3F93,
};

/* Tile t is painted entirely with colour index t, and palette entry i is a
 * pure blue ramp, so a baked pixel names the tile it came from. */
static uint8_t *BuildRom(void) {
  uint8_t *rom = calloc(1, kRomSize);
  for (int tile = 0; tile < 256; tile++)
    memset(rom + kTilesOffset + tile * 64, (uint8_t)tile, 64);
  for (int i = 0; i < 256; i++) {
    uint16_t bgr555 = (uint16_t)((i & 0x1F) << 10);
    rom[kPaletteOffset + i * 2] = (uint8_t)(bgr555 & 0xFF);
    rom[kPaletteOffset + i * 2 + 1] = (uint8_t)(bgr555 >> 8);
  }
  for (int i = 0; i < kSimWorldMapBytes; i++)
    rom[kTilemapOffset + i] = (uint8_t)(i & 0x7F);
  return rom;
}

static uint32_t ColorForTile(uint8_t tile) {
  uint32_t blue = (uint32_t)((tile & 0x1F) << 3) | ((tile & 0x1F) >> 2);
  return 0xFF000000u | blue;
}

static void ReferenceFullBake(const uint8_t *tilemap, uint32_t *out) {
  for (int tile_y = 0; tile_y < kSimWorldMapTiles; tile_y++)
    for (int tile_x = 0; tile_x < kSimWorldMapTiles; tile_x++) {
      uint32_t color =
          ColorForTile(tilemap[tile_y * kSimWorldMapTiles + tile_x]);
      for (int row = 0; row < kSimWorldMapTilePixels; row++) {
        uint32_t *p = out +
            (size_t)(tile_y * kSimWorldMapTilePixels + row) *
                kSimWorldMapPixels +
            tile_x * kSimWorldMapTilePixels;
        for (int col = 0; col < kSimWorldMapTilePixels; col++) p[col] = color;
      }
    }
}

static size_t FirstPixelForTile(size_t tile) {
  return (tile / kSimWorldMapTiles) * kSimWorldMapTilePixels *
             kSimWorldMapPixels +
         (tile % kSimWorldMapTiles) * kSimWorldMapTilePixels;
}

static void TestUnavailableRom(void) {
  uint8_t tiny[16] = { 0 };
  CHECK(!SimWorldMap_Init(tiny, sizeof(tiny)));
  CHECK(!SimWorldMap_Available());
  CHECK(SimWorldMap_Serial() == 0);
  CHECK(SimWorldMap_Baseline() == NULL);
  CHECK(SimWorldMap_PublishBuiltTilemap(tiny) == 0);
  int x = -1, y = -1;
  /* The window table is static data, so it still answers without a ROM. */
  CHECK(SimWorldMap_OriginForTown(1, &x, &y));
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(!SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  free(pixels);
}

/* Every origin lands on a multiple of 16, the six windows fit inside the map
 * and never overlap. No other assignment of towns to the world map's six
 * cathedral icons has those properties. */
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
  CHECK(x[0] == 80 && y[0] == 48);
  CHECK(x[1] + kSimTownCells == x[0] && y[1] == y[0]);
}

static void TestBuiltTilemapPublication(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));
  const uint8_t *baseline = SimWorldMap_Baseline();
  CHECK(baseline != NULL);

  uint8_t *built = malloc(kSimWorldMapBytes);
  memcpy(built, baseline, kSimWorldMapBytes);
  const size_t first = 5;
  const size_t second = 4000;
  built[first] = 0x33;
  built[second] = 0x71;

  uint32_t serial = SimWorldMap_Serial();
  CHECK(SimWorldMap_PublishBuiltTilemap(built) == 2);
  CHECK(SimWorldMap_Serial() != serial);
  /* Publishing never mutates the retained base used by future builds. */
  CHECK(baseline[first] == (uint8_t)(first & 0x7F));
  CHECK(baseline[second] == (uint8_t)(second & 0x7F));

  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[FirstPixelForTile(first)] == ColorForTile(0x33));
  CHECK(pixels[FirstPixelForTile(second)] == ColorForTile(0x71));

  serial = SimWorldMap_Serial();
  CHECK(SimWorldMap_PublishBuiltTilemap(built) == 0);
  CHECK(SimWorldMap_Serial() == serial);
  CHECK(SimWorldMap_PublishBuiltTilemap(NULL) == 0);

  free(pixels);
  free(built);
  free(rom);
}

/* The bake is fully opaque everywhere. There is deliberately no hole for the
 * town being played; overlap is handled by draw order. */
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

  for (int town = 1; town <= 6; town++) {
    int origin_x = 0, origin_y = 0;
    CHECK(SimWorldMap_OriginForTown((uint8_t)town, &origin_x, &origin_y));
    uint32_t pixel = pixels[(size_t)origin_y * kSimWorldMapTilePixels *
                            kSimWorldMapPixels +
                            origin_x * kSimWorldMapTilePixels];
    int tile = (origin_y * kSimWorldMapTiles + origin_x) & 0x7F;
    CHECK(pixel == ColorForTile((uint8_t)tile));
  }
  free(pixels);
  free(rom);
}

/* Per-tile dirty tracking must never change what a bake produces, only how
 * much work it does. The caller buffer is deliberately pre-filled with garbage
 * because a streaming-texture lock has undefined prior contents. */
static void TestDirtyTrackingMatchesFullBake(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));

  size_t count = (size_t)kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *baked = malloc(count * 4);
  uint32_t *reference = malloc(count * 4);
  uint8_t *expected = malloc(kSimWorldMapBytes);
  memcpy(expected, SimWorldMap_Baseline(), kSimWorldMapBytes);

  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);

  const size_t edited = 1029;
  expected[edited] = 0x33;
  CHECK(SimWorldMap_PublishBuiltTilemap(expected) == 1);
  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);
  CHECK(baked[FirstPixelForTile(edited)] == ColorForTile(0x33));

  /* Several complete builder publications before one bake. */
  memset(expected, 0xAB, kSimWorldMapBytes);
  CHECK(SimWorldMap_PublishBuiltTilemap(expected) > 0);
  expected[100] = 0x07;
  expected[4000] = 0x71;
  CHECK(SimWorldMap_PublishBuiltTilemap(expected) == 2);
  memset(expected, 0x5C, kSimWorldMapBytes);
  CHECK(SimWorldMap_PublishBuiltTilemap(expected) > 0);

  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);

  /* Nothing dirty still emits the full image into a fresh caller buffer. */
  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  CHECK(memcmp(baked, reference, count * 4) == 0);

  free(expected);
  free(reference);
  free(baked);
  free(rom);
}

/* The blur mip must be a box average of the SAME persistent image Bake emits,
 * never a readback from its write-only destination. Also assert pitch handling
 * and invalid-argument rejection. */
static void TestDownsampleMatchesBake(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));

  const int count = kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *baked = malloc((size_t)count * 4);
  const int divisor = 4;
  const int extent = kSimWorldMapPixels / divisor;
  const int pitch = extent + 7;
  uint32_t *mip = malloc((size_t)pitch * extent * 4);
  uint32_t *reference = malloc((size_t)extent * extent * 4);

  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  for (int y = 0; y < extent; y++) {
    for (int x = 0; x < extent; x++) {
      uint32_t a = 0, r = 0, g = 0, b = 0;
      for (int sy = 0; sy < divisor; sy++) {
        for (int sx = 0; sx < divisor; sx++) {
          uint32_t texel =
              baked[(size_t)(y * divisor + sy) * kSimWorldMapPixels +
                    (size_t)x * divisor + sx];
          a += (texel >> 24) & 0xFF;
          r += (texel >> 16) & 0xFF;
          g += (texel >> 8) & 0xFF;
          b += texel & 0xFF;
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
  for (int y = 0; y < extent; y++)
    CHECK(memcmp(mip + (size_t)y * pitch, reference + (size_t)y * extent,
                 (size_t)extent * 4) == 0);

  memset(mip, 0x5C, (size_t)pitch * extent * 4);
  CHECK(SimWorldMap_Downsample(mip, pitch, divisor));
  for (int y = 0; y < extent; y++)
    CHECK(memcmp(mip + (size_t)y * pitch, reference + (size_t)y * extent,
                 (size_t)extent * 4) == 0);

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
  TestBuiltTilemapPublication();
  TestBakeIsFullyCovered();
  TestDirtyTrackingMatchesFullBake();
  TestDownsampleMatchesBake();
  SimWorldMap_Shutdown();
  printf("sim world map tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
