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
