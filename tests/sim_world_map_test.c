/* World-map underlay: ROM residency, the town window table, publication of an
 * owned complete tilemap, dirty tracking, and baking/downsampling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_world_map.h"
#include "sim_world_map_compose.h"

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
  kWaterFramesOffset = 0x053000,
  kTilesOffset = 0x070000,
  kPaletteOffset = 0x0E3F93,
  kOrdinaryTranslationOffset = 0x010000,
  kSpecialTranslationOffset = 0x010100,
  kTownDestinationOffset = 0x0107A5,
  kTownMapsWramOffset = 0x12000,
  kTownEnabledWramOffset = 0x16B18,
  kWorldFlagsWramOffset = 0x19101,
  kWorldShadowWramOffset = 0x0C000,
  kWramBytes = 0x20000,
};

static const uint16_t kTownDestinations[kSimWorldMapTownCount] = {
  0x1850, 0x1830, 0x2010, 0x1010, 0x3040, 0x0020,
};

/* Tile t is painted entirely with colour index t, and palette entry i is a
 * pure blue ramp, so a baked pixel names the tile it came from. */
static uint8_t *BuildRom(void) {
  uint8_t *rom = calloc(1, kRomSize);
  for (int frame = 0; frame < 4; frame++)
    memset(rom + kWaterFramesOffset + frame * 64, 0x11 + frame, 64);
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

static void TestWaterAnimation(void) {
  uint8_t *rom = BuildRom();
  /* Exercise both upload destinations. The baseline already contains tile
   * $00; add tile $AA explicitly because the synthetic map stops at $7F. */
  rom[kTilemapOffset + 1] = 0xAA;
  CHECK(SimWorldMap_Init(rom, kRomSize));

  uint32_t *pixels =
      calloc((size_t)kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[FirstPixelForTile(0)] == ColorForTile(0x00));
  CHECK(pixels[FirstPixelForTile(1)] == ColorForTile(0xAA));

  uint32_t serial = SimWorldMap_Serial();
  const int water_cells = 129; /* 128 synthetic tile-$00 cells plus tile 1. */
  CHECK(SimWorldMap_SetWaterAnimationSource(0xB000) == water_cells);
  CHECK(SimWorldMap_Serial() != serial);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[FirstPixelForTile(0)] == ColorForTile(0x11));
  CHECK(pixels[FirstPixelForTile(1)] == ColorForTile(0x11));

  serial = SimWorldMap_Serial();
  CHECK(SimWorldMap_SetWaterAnimationSource(0xB000) == 0);
  CHECK(SimWorldMap_SetWaterAnimationSource(0xAFC0) == 0);
  CHECK(SimWorldMap_SetWaterAnimationSource(0xB001) == 0);
  CHECK(SimWorldMap_SetWaterAnimationSource(0xB100) == 0);
  CHECK(SimWorldMap_Serial() == serial);

  CHECK(SimWorldMap_SetWaterAnimationSource(0xB0C0) == water_cells);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[FirstPixelForTile(0)] == ColorForTile(0x14));
  CHECK(pixels[FirstPixelForTile(1)] == ColorForTile(0x14));

  free(pixels);
  free(rom);
}

static void TestUnavailableRom(void) {
  uint8_t tiny[16] = { 0 };
  CHECK(!SimWorldMap_Init(tiny, sizeof(tiny)));
  CHECK(!SimWorldMap_Available());
  CHECK(!SimWorldMap_DevelopedAvailable());
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
  CHECK(!SimWorldMap_DevelopedAvailable());
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
  CHECK(SimWorldMap_DevelopedAvailable());
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

static size_t TestTownCellIndex(int x, int y) {
  const int page = (y / 16) * 2 + x / 16;
  return (size_t)page * 256 + (size_t)(y % 16) * 16 + x % 16;
}

static void BuildSyntheticTables(SimWorldMapRomTables *tables) {
  memset(tables, 0, sizeof(*tables));
  for (int i = 1; i < 0xE3; i++) tables->ordinary[i] = (uint8_t)i;
  for (int special = 0; special < kSimWorldMapSpecialTranslationCount;
       special++)
    for (int tile = 0; tile < kSimWorldMapSpecialTilesPerCell; tile++)
      tables->special[special][tile] =
          (uint8_t)(0x20 + special * 4 + tile);
  memcpy(tables->town_destination, kTownDestinations,
         sizeof(kTownDestinations));
}

static void TestRomTableLoading(void) {
  uint8_t *rom = BuildRom();
  for (int i = 0; i < kSimWorldMapOrdinaryTranslationCount; i++)
    rom[kOrdinaryTranslationOffset + i] = (uint8_t)(i ^ 0x5A);
  for (int i = 0;
       i < kSimWorldMapSpecialTranslationCount *
               kSimWorldMapSpecialTilesPerCell;
       i++)
    rom[kSpecialTranslationOffset + i] = (uint8_t)(0x80 + i);
  for (int town = 0; town < kSimWorldMapTownCount; town++) {
    rom[kTownDestinationOffset + town * 2] =
        (uint8_t)kTownDestinations[town];
    rom[kTownDestinationOffset + town * 2 + 1] =
        (uint8_t)(kTownDestinations[town] >> 8);
  }

  SimWorldMapRomTables tables;
  memset(&tables, 0xCC, sizeof(tables));
  CHECK(SimWorldMap_LoadRomTables(&tables, rom, kRomSize));
  for (int i = 0; i < kSimWorldMapOrdinaryTranslationCount; i++)
    CHECK(tables.ordinary[i] == (uint8_t)(i ^ 0x5A));
  for (int i = 0;
       i < kSimWorldMapSpecialTranslationCount *
               kSimWorldMapSpecialTilesPerCell;
       i++)
    CHECK(((const uint8_t *)tables.special)[i] == (uint8_t)(0x80 + i));
  for (int town = 0; town < kSimWorldMapTownCount; town++)
    CHECK(tables.town_destination[town] == kTownDestinations[town]);

  SimWorldMapRomTables unchanged = tables;
  CHECK(!SimWorldMap_LoadRomTables(
      &tables, rom, kTownDestinationOffset +
                        kSimWorldMapTownCount * 2 - 1));
  CHECK(memcmp(&tables, &unchanged, sizeof(tables)) == 0);
  CHECK(!SimWorldMap_LoadRomTables(NULL, rom, kRomSize));
  CHECK(!SimWorldMap_LoadRomTables(&tables, NULL, kRomSize));
  free(rom);
}

static void TestComposeQuadrantPagingAndPurity(void) {
  uint8_t baseline[kSimWorldMapBytes];
  uint8_t out[kSimWorldMapBytes];
  uint8_t expected[kSimWorldMapBytes];
  uint8_t town_maps[kSimWorldMapTownCount][kSimWorldMapTownCells] = {{0}};
  uint16_t enabled[kSimWorldMapTownCount] = {1, 0, 0, 0, 0, 0};
  SimWorldMapRomTables tables;
  BuildSyntheticTables(&tables);
  memset(baseline, 0xA5, sizeof(baseline));

  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++)
      town_maps[0][TestTownCellIndex(x, y)] =
          (uint8_t)(1 + ((y * 32 + x) % 0xD0));
  /* A zero translation preserves the pristine base at this one cell. */
  town_maps[0][TestTownCellIndex(7, 9)] = 0xD2;
  tables.ordinary[0xD2] = 0;

  uint8_t baseline_before[kSimWorldMapBytes];
  uint8_t maps_before[sizeof(town_maps)];
  uint16_t enabled_before[kSimWorldMapTownCount];
  SimWorldMapRomTables tables_before;
  memcpy(baseline_before, baseline, sizeof(baseline));
  memcpy(maps_before, town_maps, sizeof(town_maps));
  memcpy(enabled_before, enabled, sizeof(enabled));
  tables_before = tables;

  memset(out, 0xCD, sizeof(out));
  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &tables));
  memcpy(expected, out, sizeof(expected));
  const size_t destination = tables.town_destination[0];
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      const uint8_t cell = town_maps[0][TestTownCellIndex(x, y)];
      const uint8_t tile = tables.ordinary[cell];
      const uint8_t want = tile ? tile : 0xA5;
      CHECK(out[destination + (size_t)y * 128 + x] == want);
    }
  }
  CHECK(out[0] == 0xA5);

  /* Composition is repeatable into dirty memory and reads no mutable global
   * state. Its explicit inputs are byte-identical afterward. */
  memset(out, 0x71, sizeof(out));
  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &tables));
  CHECK(memcmp(out, expected, sizeof(out)) == 0);
  CHECK(memcmp(baseline, baseline_before, sizeof(baseline)) == 0);
  CHECK(memcmp(town_maps, maps_before, sizeof(town_maps)) == 0);
  CHECK(memcmp(enabled, enabled_before, sizeof(enabled)) == 0);
  CHECK(memcmp(&tables, &tables_before, sizeof(tables)) == 0);

  /* The ROM tests an enable word only for zero/nonzero. */
  enabled[0] = 0xBEEF;
  memset(out, 0, sizeof(out));
  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &tables));
  CHECK(memcmp(out, expected, sizeof(out)) == 0);
  enabled[0] = 0;
  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &tables));
  CHECK(memcmp(out, baseline, sizeof(out)) == 0);
}

static void TestComposeSpecialExpansions(void) {
  uint8_t baseline[kSimWorldMapBytes];
  uint8_t out[kSimWorldMapBytes];
  uint8_t town_maps[kSimWorldMapTownCount][kSimWorldMapTownCells] = {{0}};
  uint16_t enabled[kSimWorldMapTownCount] = {1, 0, 0, 0, 0, 0};
  SimWorldMapRomTables tables;
  BuildSyntheticTables(&tables);
  memset(baseline, 0xA5, sizeof(baseline));

  for (int special = 0; special < kSimWorldMapSpecialTranslationCount;
       special++) {
    const int x = (special % 8) * 2;
    const int y = (special / 8) * 2;
    town_maps[0][TestTownCellIndex(x, y)] = (uint8_t)(0xE3 + special);
  }
  /* $02:8726 samples aligned even cells only. */
  town_maps[0][TestTownCellIndex(1, 10)] = 0xE3;

  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &tables));
  const size_t destination = tables.town_destination[0];
  for (int special = 0; special < kSimWorldMapSpecialTranslationCount;
       special++) {
    const int x = (special % 8) * 2;
    const int y = (special / 8) * 2;
    const size_t at = destination + (size_t)y * 128 + x;
    CHECK(out[at] == tables.special[special][0]);
    CHECK(out[at + 1] == tables.special[special][1]);
    CHECK(out[at + 128] == tables.special[special][2]);
    CHECK(out[at + 129] == tables.special[special][3]);
  }
  CHECK(out[destination + (size_t)10 * 128 + 1] == 0xA5);
}

static void TestComposeWorldFlagClear(void) {
  uint8_t baseline[kSimWorldMapBytes];
  uint8_t out[kSimWorldMapBytes];
  uint8_t town_maps[kSimWorldMapTownCount][kSimWorldMapTownCells] = {{0}};
  uint16_t enabled[kSimWorldMapTownCount] = {0};
  SimWorldMapRomTables tables;
  BuildSyntheticTables(&tables);
  memset(baseline, 0x7B, sizeof(baseline));

  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &tables));
  CHECK(memcmp(out, baseline, sizeof(out)) == 0);

  CHECK(SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 0, &tables));
  int cleared = 0;
  for (size_t i = 0; i < sizeof(out); i++) {
    const int y = (int)(i / 128);
    const int x = (int)(i % 128);
    const bool in_clear = y >= 12 && y < 20 && x >= 96 && x < 104;
    CHECK(out[i] == (in_clear ? 0 : 0x7B));
    if (out[i] == 0) cleared++;
  }
  CHECK(cleared == 64);
}

static void TestComposeIndividualTownEnables(void) {
  uint8_t baseline[kSimWorldMapBytes];
  uint8_t out[kSimWorldMapBytes];
  uint8_t town_maps[kSimWorldMapTownCount][kSimWorldMapTownCells] = {{0}};
  uint16_t enabled[kSimWorldMapTownCount] = {0};
  SimWorldMapRomTables tables;
  BuildSyntheticTables(&tables);
  memset(baseline, 0xA5, sizeof(baseline));
  for (int town = 0; town < kSimWorldMapTownCount; town++) {
    memset(town_maps[town], town + 1, kSimWorldMapTownCells);
    tables.ordinary[town + 1] = (uint8_t)(0x70 + town);
  }

  for (int active = 0; active < kSimWorldMapTownCount; active++) {
    memset(enabled, 0, sizeof(enabled));
    enabled[active] = 1;
    CHECK(SimWorldMap_ComposeDeveloped(
        out, baseline, town_maps, enabled, 1, &tables));
    for (int town = 0; town < kSimWorldMapTownCount; town++) {
      const size_t at = tables.town_destination[town];
      CHECK(out[at] == (town == active ? (uint8_t)(0x70 + town) : 0xA5));
      CHECK(out[at + 31 * 128 + 31] ==
            (town == active ? (uint8_t)(0x70 + town) : 0xA5));
    }
  }

  SimWorldMapRomTables bad = tables;
  bad.town_destination[0] = kSimWorldMapBytes - 1;
  memset(out, 0xCC, sizeof(out));
  memset(enabled, 0, sizeof(enabled));
  enabled[0] = 1;
  CHECK(!SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, &bad));
  for (size_t i = 0; i < sizeof(out); i++) CHECK(out[i] == 0xCC);
  CHECK(!SimWorldMap_ComposeDeveloped(
      NULL, baseline, town_maps, enabled, 1, &tables));
  CHECK(!SimWorldMap_ComposeDeveloped(
      out, NULL, town_maps, enabled, 1, &tables));
  CHECK(!SimWorldMap_ComposeDeveloped(
      out, baseline, NULL, enabled, 1, &tables));
  CHECK(!SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, NULL, 1, &tables));
  CHECK(!SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, 1, NULL));
}

static uint8_t *ReadExactFile(const char *path, size_t expected_size) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "could not open fixture: %s\n", path);
    return NULL;
  }
  uint8_t *data = (uint8_t *)malloc(expected_size);
  const size_t got = data ? fread(data, 1, expected_size, file) : 0;
  const int extra = fgetc(file);
  fclose(file);
  if (!data || got != expected_size || extra != EOF) {
    fprintf(stderr, "fixture has wrong size: %s\n", path);
    free(data);
    return NULL;
  }
  return data;
}

static bool ComposeFixture(uint8_t out[kSimWorldMapBytes],
                           const uint8_t *baseline,
                           const uint8_t *wram,
                           const SimWorldMapRomTables *tables) {
  uint16_t enabled[kSimWorldMapTownCount];
  for (int town = 0; town < kSimWorldMapTownCount; town++) {
    const uint8_t *word = wram + kTownEnabledWramOffset + town * 2;
    enabled[town] = (uint16_t)(word[0] | ((uint16_t)word[1] << 8));
  }
  const uint8_t (*town_maps)[kSimWorldMapTownCells] =
      (const uint8_t (*)[kSimWorldMapTownCells])
          (wram + kTownMapsWramOffset);
  return SimWorldMap_ComposeDeveloped(
      out, baseline, town_maps, enabled, wram[kWorldFlagsWramOffset], tables);
}

static int CountByteDifferences(const uint8_t *a, const uint8_t *b,
                                size_t size) {
  int differences = 0;
  for (size_t i = 0; i < size; i++)
    if (a[i] != b[i]) differences++;
  return differences;
}

static void TestCapturedFixtures(const char *rom_path, const char *act_path,
                                 const char *navigation_path,
                                 const char *animation_path) {
  uint8_t *rom = ReadExactFile(rom_path, kRomSize);
  uint8_t *act = ReadExactFile(act_path, kWramBytes);
  uint8_t *navigation = ReadExactFile(navigation_path, kWramBytes);
  uint8_t *animation = ReadExactFile(animation_path, kWramBytes);
  if (!rom || !act || !navigation || !animation) {
    s_failures++;
    free(animation);
    free(navigation);
    free(act);
    free(rom);
    return;
  }

  SimWorldMapRomTables tables;
  uint8_t act_out[kSimWorldMapBytes];
  uint8_t navigation_out[kSimWorldMapBytes];
  uint8_t animation_out[kSimWorldMapBytes];
  const uint8_t *baseline = rom + kTilemapOffset;
  const uint8_t *authentic = navigation + kWorldShadowWramOffset;
  CHECK(SimWorldMap_LoadRomTables(&tables, rom, kRomSize));
  CHECK(ComposeFixture(act_out, baseline, act, &tables));
  CHECK(ComposeFixture(navigation_out, baseline, navigation, &tables));
  CHECK(ComposeFixture(animation_out, baseline, animation, &tables));

  /* The direct act->town shadow is corrupt, so its HLE result is checked
   * against the authentic $09 build. Both $09 fixtures retain that same map. */
  CHECK(memcmp(act_out, authentic, kSimWorldMapBytes) == 0);
  CHECK(memcmp(navigation_out, authentic, kSimWorldMapBytes) == 0);
  CHECK(memcmp(animation_out, authentic, kSimWorldMapBytes) == 0);
  CHECK(CountByteDifferences(act_out, baseline, kSimWorldMapBytes) == 447);
  CHECK(CountByteDifferences(navigation_out, baseline, kSimWorldMapBytes) ==
        447);
  CHECK(CountByteDifferences(animation_out, baseline, kSimWorldMapBytes) ==
        447);
  printf("sim world map fixture parity: 16384/16384 bytes, 447 developed\n");

  free(animation);
  free(navigation);
  free(act);
  free(rom);
}

int main(int argc, char **argv) {
  TestUnavailableRom();
  TestTownWindows();
  TestBuiltTilemapPublication();
  TestWaterAnimation();
  TestBakeIsFullyCovered();
  TestDirtyTrackingMatchesFullBake();
  TestDownsampleMatchesBake();
  TestRomTableLoading();
  TestComposeQuadrantPagingAndPurity();
  TestComposeSpecialExpansions();
  TestComposeWorldFlagClear();
  TestComposeIndividualTownEnables();
  if (argc == 6 && strcmp(argv[1], "--fixtures") == 0)
    TestCapturedFixtures(argv[2], argv[3], argv[4], argv[5]);
  else if (argc != 1) {
    fprintf(stderr,
            "usage: %s [--fixtures ROM ACT_WRAM NAV_WRAM ANIMATION_WRAM]\n",
            argv[0]);
    s_failures++;
  }
  SimWorldMap_Shutdown();
  printf("sim world map tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
