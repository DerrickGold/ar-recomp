#include "sim/sim_background_mountains.h"

#include <stdio.h>
#include <string.h>

enum {
  kWramBytes = 0x20000,
  kCellMaps = 0x12000,
  kCellMapBytes = 0x400,
};

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static size_t CellIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kCellMaps + (size_t)(town - 1) * kCellMapBytes +
      quadrant * 0x100 + (y & 15) * 16 + (x & 15);
}

static void SetCell(uint8_t *wram, uint8_t town,
                    int x, int y, uint8_t tile) {
  wram[CellIndex(town, x, y)] = tile;
}

int main(void) {
  static uint8_t wram[kWramBytes];
  SimBackgroundMountainField field;

  SetCell(wram, 1, 0, 0, 0x78);
  SetCell(wram, 1, 1, 0, 0x79);
  SetCell(wram, 1, 20, 20, 0x9F);
  SimBackgroundMountains_Classify(1, wram, &field);
  CHECK(field.town == 1);
  CHECK(field.cell_count == 3);
  CHECK(field.component_count == 2);
  CHECK(field.component[0] == field.component[1]);
  CHECK(field.component[20 * 32 + 20] != field.component[0]);
  int source_x = -1, source_y = -1;
  CHECK(SimBackgroundMountains_TileSource(
      &field, 0x79, &source_x, &source_y));
  CHECK(source_x == 1 && source_y == 0);
  CHECK(!SimBackgroundMountains_TileSource(
      &field, 0x77, &source_x, &source_y));

  /* Three complete north-edge groups gain both restored rows. Filmora's
   * repeated nested overlap layers the two demonstrated source fragments onto
   * every reconstructed stamp; base caps and the original range stay intact. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 12; x++) SetCell(wram, 1, x, 0, 0x86);
  SetCell(wram, 1, 10, 0, 0x8A);
  SetCell(wram, 1, 11, 0, 0x7F);
  SetCell(wram, 1, 12, 8, 0x8A);
  SetCell(wram, 1, 13, 8, 0x7D);
  SetCell(wram, 1, 14, 8, 0x89);
  SetCell(wram, 1, 15, 8, 0x82);
  SetCell(wram, 1, 16, 8, 0x81);
  SimBackgroundMountains_Classify(1, wram, &field);
  SimBackgroundMountainCaps caps;
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(caps.tile_count == 25);
  CHECK(caps.tiles[0].cell_x == 0 && caps.tiles[0].cell_y == -1 &&
        caps.tiles[0].source_tile == 0x8A);
  CHECK(caps.tiles[2].cell_x == 2 &&
        (caps.tiles[2].flags & kSimBackgroundMountainCapTile_MirrorX));
  CHECK(caps.tiles[14].cell_x == 10 && caps.tiles[14].cell_y == -1 &&
        caps.tiles[14].source_tile == 0x7D &&
        (caps.tiles[14].flags & kSimBackgroundMountainCapTile_MirrorX));
  CHECK(caps.tiles[17].cell_x == 11 && caps.tiles[17].cell_y == -2 &&
        caps.tiles[17].source_tile == 0x81 && caps.tiles[17].flags == 0);
  CHECK(caps.tiles[4].cell_x == 0 && caps.tiles[4].cell_y == -2 &&
        caps.tiles[4].source_tile == 0x82);
  CHECK(caps.tiles[5].cell_x == 3 && caps.tiles[5].cell_y == -2 &&
        caps.tiles[5].source_tile == 0x81);
  CHECK(caps.tiles[18].cell_x == 12 && caps.tiles[18].cell_y == -2 &&
        caps.tiles[18].source_tile == 0x82);
  CHECK(caps.tiles[19].cell_x == 2 && caps.tiles[19].cell_y == -1 &&
        caps.tiles[19].source_tile == 0x82);
  CHECK(caps.tiles[20].cell_x == 3 && caps.tiles[20].cell_y == -2 &&
        caps.tiles[20].source_tile == 0x89 &&
        (caps.tiles[20].flags &
         kSimBackgroundMountainCapTile_TriangleLowerRight));
  CHECK(caps.tiles[21].cell_x == 6 && caps.tiles[21].cell_y == -1 &&
        caps.tiles[21].source_tile == 0x82);
  CHECK(caps.tiles[22].cell_x == 7 && caps.tiles[22].cell_y == -2 &&
        caps.tiles[22].source_tile == 0x89 &&
        (caps.tiles[22].flags &
         kSimBackgroundMountainCapTile_TriangleLowerRight));
  CHECK(caps.tiles[23].cell_x == 10 && caps.tiles[23].cell_y == -1 &&
        caps.tiles[23].source_tile == 0x82);
  CHECK(caps.tiles[24].cell_x == 11 && caps.tiles[24].cell_y == -2 &&
        caps.tiles[24].source_tile == 0x89 &&
        (caps.tiles[24].flags &
         kSimBackgroundMountainCapTile_TriangleLowerRight));

  /* A range spanning the full map width keeps its authentic half-peaks at
   * both level boundaries instead of inventing cap halves at x=-1 or x=32. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 32; x++) SetCell(wram, 1, x, 0, 0x86);
  SetCell(wram, 1, 12, 8, 0x8A);
  SetCell(wram, 1, 13, 8, 0x7D);
  SetCell(wram, 1, 14, 8, 0x89);
  SetCell(wram, 1, 15, 8, 0x82);
  SetCell(wram, 1, 16, 8, 0x81);
  SimBackgroundMountains_Classify(1, wram, &field);
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(caps.tile_count == 64);
  for (int tile = 0; tile < caps.tile_count; tile++) {
    CHECK(caps.tiles[tile].cell_x >= 0);
    CHECK(caps.tiles[tile].cell_x < 32);
  }

  /* Snowy Northwall ranges use the same complete-source-art restoration; the
   * renderer samples this town's snow-covered tiles and palette. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 12; x++) SetCell(wram, 6, x, 0, 0x86);
  SetCell(wram, 6, 12, 8, 0x8A);
  SetCell(wram, 6, 13, 8, 0x7D);
  SetCell(wram, 6, 14, 8, 0x89);
  SetCell(wram, 6, 15, 8, 0x82);
  SetCell(wram, 6, 16, 8, 0x81);
  SimBackgroundMountains_Classify(6, wram, &field);
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(field.town == 6);
  CHECK(caps.tile_count == 19);
  CHECK(caps.tiles[14].source_tile == 0x7D);
  CHECK(caps.tiles[14].flags & kSimBackgroundMountainCapTile_MirrorX);
  CHECK(caps.tiles[17].source_tile == 0x81);
  CHECK(caps.tiles[18].source_tile == 0x82);

  /* Marahna's $8D is isolated swamp decoration, not common-range rock. */
  SetCell(wram, 5, 4, 4, 0x8D);
  SimBackgroundMountains_Classify(5, wram, &field);
  CHECK(field.cell_count == 0);
  CHECK(SimBackgroundMountains_TileFlags(5, 0x8D) == 0);

  SetCell(wram, 4, 5, 5, 0x70);
  SetCell(wram, 4, 6, 5, 0x71);
  SetCell(wram, 4, 7, 5, 0x78);
  SimBackgroundMountains_Classify(4, wram, &field);
  CHECK(field.cell_count == 3);
  CHECK(field.component_count == 1);
  CHECK((field.flags[5 * 32 + 5] &
         kSimBackgroundMountainCell_LavaCap) != 0);
  CHECK((field.flags[5 * 32 + 7] &
         kSimBackgroundMountainCell_LavaCap) == 0);

  SetCell(wram, 6, 31, 31, 0x80);
  SimBackgroundMountains_Classify(6, wram, &field);
  CHECK(SimBackgroundMountains_CellOccupied(&field, 31, 31));
  CHECK(!SimBackgroundMountains_CellOccupied(&field, 32, 31));

  memset(&field, 0xFF, sizeof(field));
  SimBackgroundMountains_Classify(0, wram, &field);
  CHECK(field.town == 0 && field.cell_count == 0);

  if (failures) return 1;
  puts("sim background mountain classification checks passed");
  return 0;
}
