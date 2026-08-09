/* ROM-free contract tests for SPEC-bg-hle BH2's world decoder. The fixtures
 * model the exact WRAM page/metatile layout but use synthetic words so page
 * selection, quadrants, attribute bits, and invalidation are independently
 * visible. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "action_bg_world.h"

static int failures;
#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); failures++; \
} } while (0)

enum {
  kTestWramBytes = kActionBgMaxWramBytes,
  kMapStart = 0x2000,
  kTableStart = 0x1000,
  kDefinitionBytes = 0x800,
};

typedef struct Fixture {
  uint8_t *wram;
  ActionBgDecodeInput input;
} Fixture;

static void WriteWord(uint8_t *dst, uint16_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
}

static uint16_t RawDefinition(uint8_t id, unsigned quadrant) {
  const uint16_t index = (uint16_t)(((unsigned)id * 4 + quadrant) & 0x3FF);
  const uint16_t hflip = (quadrant & 1u) ? 0x4000 : 0;
  const uint16_t vflip = (quadrant & 2u) ? 0x8000 : 0;
  const uint16_t priority = (id & 1u) ? 0x2000 : 0;
  return index | hflip | vflip | priority;
}

static void WriteDefinitions(Fixture *fixture) {
  for (unsigned id = 0; id < 256; id++) {
    for (unsigned quadrant = 0; quadrant < 4; quadrant++) {
      WriteWord(fixture->wram + kTableStart + id * 8 + quadrant * 2,
                RawDefinition((uint8_t)id, quadrant));
    }
  }
}

static size_t MapAddress(const Fixture *fixture, unsigned page_x,
                         unsigned page_y, unsigned metatile_x,
                         unsigned metatile_y) {
  const unsigned pages_wide = fixture->input.world_width / 256;
  const size_t page = (size_t)page_y * pages_wide + page_x;
  return kMapStart + page * 256 + metatile_y * 16 + metatile_x;
}

static void FillMap(Fixture *fixture) {
  const unsigned pages_wide = fixture->input.world_width / 256;
  const unsigned pages_high = fixture->input.world_height / 256;
  for (unsigned page_y = 0; page_y < pages_high; page_y++) {
    for (unsigned page_x = 0; page_x < pages_wide; page_x++) {
      for (unsigned metatile_y = 0; metatile_y < 16; metatile_y++) {
        for (unsigned metatile_x = 0; metatile_x < 16; metatile_x++) {
          const uint8_t id = (uint8_t)(page_y * 80 + page_x * 32 +
              metatile_y * 3 + metatile_x);
          fixture->wram[MapAddress(fixture, page_x, page_y, metatile_x,
                                   metatile_y)] = id;
        }
      }
    }
  }
}

static Fixture MakeFixture(void) {
  Fixture fixture = { 0 };
  fixture.wram = calloc(1, kTestWramBytes);
  CHECK(fixture.wram != NULL);
  fixture.input = (ActionBgDecodeInput) {
    .wram = fixture.wram,
    .wram_size = kTestWramBytes,
    .world_width = 512,
    .world_height = 512,
    .map_page = kMapStart,
    .metatile_table = kTableStart,
    /* Preserve tile index, priority, and flip flags; replace palette bits with
     * the common attribute byte exactly as the native decoder does. */
    .word_mask = 0xE3FF,
    .attributes = 0x05,
  };
  if (fixture.wram) {
    WriteDefinitions(&fixture);
    FillMap(&fixture);
  }
  return fixture;
}

static void DestroyFixture(Fixture *fixture) {
  free(fixture->wram);
  memset(fixture, 0, sizeof(*fixture));
}

static uint8_t IdAtTile(const Fixture *fixture, unsigned tile_x,
                        unsigned tile_y) {
  const unsigned page_x = tile_x >> 5;
  const unsigned page_y = tile_y >> 5;
  const unsigned metatile_x = (tile_x >> 1) & 15u;
  const unsigned metatile_y = (tile_y >> 1) & 15u;
  return fixture->wram[MapAddress(fixture, page_x, page_y, metatile_x,
                                  metatile_y)];
}

static uint16_t ExpectedAtTile(const Fixture *fixture, unsigned tile_x,
                               unsigned tile_y) {
  const uint8_t id = IdAtTile(fixture, tile_x, tile_y);
  const unsigned quadrant = ((tile_y & 1u) << 1) | (tile_x & 1u);
  return (uint16_t)((RawDefinition(id, quadrant) &
                     fixture->input.word_mask) |
                    ((uint16_t)fixture->input.attributes << 8));
}

static void CheckTile(const Fixture *fixture, const ActionBgWorld *world,
                      int tile_x, int tile_y) {
  uint16_t entry = 0xDEAD;
  CHECK(ActionBgWorld_Lookup(world, tile_x, tile_y, &entry) ==
        kActionBgLookup_Tile);
  CHECK(entry == ExpectedAtTile(fixture, (unsigned)tile_x, (unsigned)tile_y));
}

static void TestDecodePagesQuadrantsAndBounds(void) {
  Fixture fixture = MakeFixture();
  ActionBgWorld *world = ActionBgWorld_Create();
  CHECK(world != NULL);
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_IsValid(world));
  CHECK(ActionBgWorld_Serial(world) == 1);
  CHECK(ActionBgWorld_TileWidth(world) == 64);
  CHECK(ActionBgWorld_TileHeight(world) == 64);
  CHECK(ActionBgWorld_TileCount(world) == 4096);

  /* Four quadrants, then both horizontal and vertical page crossings. */
  CheckTile(&fixture, world, 0, 0);
  CheckTile(&fixture, world, 1, 0);
  CheckTile(&fixture, world, 0, 1);
  CheckTile(&fixture, world, 1, 1);
  CheckTile(&fixture, world, 31, 17);
  CheckTile(&fixture, world, 32, 17);
  CheckTile(&fixture, world, 17, 31);
  CheckTile(&fixture, world, 17, 32);
  CheckTile(&fixture, world, 63, 63);

  uint16_t sentinel = 0xBEEF;
  CHECK(ActionBgWorld_Lookup(world, -1, 0, &sentinel) ==
        kActionBgLookup_OutsideWorld);
  CHECK(sentinel == 0xBEEF);
  CHECK(ActionBgWorld_Lookup(world, 0, -1, &sentinel) ==
        kActionBgLookup_OutsideWorld);
  CHECK(ActionBgWorld_Lookup(world, 64, 0, &sentinel) ==
        kActionBgLookup_OutsideWorld);
  CHECK(ActionBgWorld_Lookup(world, 0, 64, &sentinel) ==
        kActionBgLookup_OutsideWorld);
  CHECK(sentinel == 0xBEEF);
  CHECK(ActionBgWorld_Lookup(world, 0, 0, NULL) ==
        kActionBgLookup_ProviderFailure);

  ActionBgWorld_Destroy(world);
  DestroyFixture(&fixture);
}

static void TestExactSourceCacheAndInvalidation(void) {
  Fixture fixture = MakeFixture();
  ActionBgWorld *world = ActionBgWorld_Create();
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  uint32_t serial = ActionBgWorld_Serial(world);

  /* Identical source and irrelevant WRAM changes are cache hits. */
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) == serial);
  fixture.wram[kTestWramBytes - 1] ^= 0xFF;
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) == serial);

  /* A metatile ID mutation invalidates the map snapshot. */
  const size_t changed_map = MapAddress(&fixture, 1, 0, 2, 3);
  fixture.wram[changed_map] ^= 0x55;
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) != serial);
  serial = ActionBgWorld_Serial(world);
  CheckTile(&fixture, world, 32 + 4, 6);

  /* Definition mutations are tracked exactly, including priority/flip bits. */
  const uint8_t id = IdAtTile(&fixture, 36, 6);
  const unsigned quadrant = 0;
  const size_t definition = kTableStart + (size_t)id * 8 + quadrant * 2;
  fixture.wram[definition + 1] ^= 0x80;
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) != serial);
  serial = ActionBgWorld_Serial(world);
  uint16_t entry = 0;
  CHECK(ActionBgWorld_Lookup(world, 36, 6, &entry) == kActionBgLookup_Tile);
  const uint16_t raw = (uint16_t)(fixture.wram[definition] |
      ((uint16_t)fixture.wram[definition + 1] << 8));
  CHECK(entry == ((raw & fixture.input.word_mask) |
                  ((uint16_t)fixture.input.attributes << 8)));

  fixture.input.word_mask ^= 0x2000;
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) != serial);
  serial = ActionBgWorld_Serial(world);
  fixture.input.attributes ^= 0x08;
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) != serial);

  ActionBgWorld_Destroy(world);
  DestroyFixture(&fixture);
}

static void ExpectRejected(ActionBgWorld *world,
                           const ActionBgDecodeInput *input) {
  uint16_t entry = 0xCAFE;
  CHECK(!ActionBgWorld_Update(world, input));
  CHECK(!ActionBgWorld_IsValid(world));
  CHECK(ActionBgWorld_Serial(world) == 0);
  CHECK(ActionBgWorld_TileCount(world) == 0);
  CHECK(ActionBgWorld_Lookup(world, 0, 0, &entry) ==
        kActionBgLookup_ProviderFailure);
  CHECK(entry == 0xCAFE);
}

static void TestMalformedInputFailsClosedAtomically(void) {
  Fixture fixture = MakeFixture();
  ActionBgWorld *world = ActionBgWorld_Create();
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  const uint32_t original_serial = ActionBgWorld_Serial(world);
  const uint16_t original_tile = ExpectedAtTile(&fixture, 63, 63);

  ActionBgDecodeInput bad = fixture.input;
  bad.wram = NULL;
  ExpectRejected(world, &bad);
  bad = fixture.input;
  bad.wram_size = kActionBgMaxWramBytes + 1u;
  ExpectRejected(world, &bad);
  bad = fixture.input;
  bad.world_width = 0;
  ExpectRejected(world, &bad);
  bad = fixture.input;
  bad.world_width = 384;
  ExpectRejected(world, &bad);
  bad = fixture.input;
  bad.world_width = 0xFF00;
  bad.world_height = 0xFF00;
  ExpectRejected(world, &bad);
  bad = fixture.input;
  bad.wram_size = 0x10000;
  bad.map_page = 0xFF00;
  ExpectRejected(world, &bad);
  bad = fixture.input;
  bad.wram_size = 0x10000;
  bad.metatile_table = 0xFC00;
  ExpectRejected(world, &bad);
  ExpectRejected(world, NULL);
  CHECK(!ActionBgWorld_Update(NULL, &fixture.input));

  /* No rejected candidate can partially replace the retained publication.
   * Re-presenting its exact valid source re-enables it without a new serial. */
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_IsValid(world));
  CHECK(ActionBgWorld_Serial(world) == original_serial);
  uint16_t entry = 0;
  CHECK(ActionBgWorld_Lookup(world, 63, 63, &entry) == kActionBgLookup_Tile);
  CHECK(entry == original_tile);

  ActionBgWorld_Reset(world);
  CHECK(!ActionBgWorld_IsValid(world));
  CHECK(ActionBgWorld_Serial(world) == 0);
  CHECK(ActionBgWorld_Update(world, &fixture.input));
  CHECK(ActionBgWorld_Serial(world) == 1);

  ActionBgWorld_Destroy(world);
  ActionBgWorld_Destroy(NULL);
  DestroyFixture(&fixture);
}

static void TestMaximumWramBound(void) {
  uint8_t *wram = calloc(1, kActionBgMaxWramBytes);
  CHECK(wram != NULL);
  for (unsigned quadrant = 0; quadrant < 4; quadrant++)
    WriteWord(wram + kTableStart + quadrant * 2,
              (uint16_t)(0x0100 + quadrant));

  /* 16x32 pages occupy all 128 KiB. The definition table deliberately
   * overlaps those source pages, which is legal: each span is snapshotted
   * independently before decoding. */
  ActionBgDecodeInput input = {
    .wram = wram,
    .wram_size = kActionBgMaxWramBytes,
    .world_width = 4096,
    .world_height = 8192,
    .map_page = 0,
    .metatile_table = kTableStart,
    .word_mask = 0xFFFF,
    .attributes = 0,
  };
  ActionBgWorld *world = ActionBgWorld_Create();
  CHECK(ActionBgWorld_Update(world, &input));
  CHECK(ActionBgWorld_TileWidth(world) == 512);
  CHECK(ActionBgWorld_TileHeight(world) == 1024);
  CHECK(ActionBgWorld_TileCount(world) == 512u * 1024u);
  uint16_t entry = 0;
  CHECK(ActionBgWorld_Lookup(world, 511, 1023, &entry) ==
        kActionBgLookup_Tile);
  CHECK(entry == 0x0103);

  ActionBgWorld_Destroy(world);
  free(wram);
}

int main(void) {
  TestDecodePagesQuadrantsAndBounds();
  TestExactSourceCacheAndInvalidation();
  TestMalformedInputFailsClosedAtomically();
  TestMaximumWramBound();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("action_bg_world: OK\n");
  return 0;
}
