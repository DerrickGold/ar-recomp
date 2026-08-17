#include "sim/sim_background_voxel_landmarks.h"

#include <stdio.h>
#include <string.h>

enum {
  kWramSize = 0x20000,
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

/* Four 16x16 pages, not row-major 32x32 storage. */
static size_t CellIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kCellMaps + (size_t)(town - 1) * kCellMapBytes +
      (size_t)quadrant * 0x100 + (size_t)(y & 15) * 16 + (x & 15);
}

static void InstallPlot(uint8_t *wram, uint8_t town, uint8_t metatile,
                        int x, int y) {
  for (int row = 0; row < 2; row++)
    for (int column = 0; column < 2; column++)
      wram[CellIndex(town, x + column, y + row)] = metatile;
}

int main(void) {
  static uint8_t wram[kWramSize];
  SimBackgroundVoxelObject object;

  /* The three landmark plots at their measured town coordinates. Bloodpool's
   * crosses the cell map's quadrant boundary, so a row-major reader would
   * silently miss it. */
  memset(wram, 0, sizeof(wram));
  InstallPlot(wram, 6, 0xEB, 26, 14);
  CHECK(SimBackgroundVoxelLandmarks_Classify(6, wram, &object, 1) == 1);
  CHECK(object.kind == kSimBackgroundVoxel_StoryTree);
  CHECK(object.cell_x == 26 && object.cell_y == 14);
  CHECK(object.source_cells_w == 2 && object.source_cells_h == 2);
  CHECK(object.footprint_cells_w == 2 && object.footprint_cells_d == 2);
  CHECK(object.record_slot == 0xFF);
  /* A landmark belongs to exactly one town, and its metatile is not a landmark
   * anywhere else. */
  CHECK(SimBackgroundVoxelLandmarks_Classify(5, wram, &object, 1) == 0);

  memset(wram, 0, sizeof(wram));
  InstallPlot(wram, 2, 0xEC, 6, 16);
  CHECK(SimBackgroundVoxelLandmarks_Classify(2, wram, &object, 1) == 1);
  CHECK(object.kind == kSimBackgroundVoxel_BloodpoolCastle);
  CHECK(object.cell_x == 6 && object.cell_y == 16);
  CHECK(object.footprint_cells_w == 2 && object.footprint_cells_d == 2);

  memset(wram, 0, sizeof(wram));
  InstallPlot(wram, 3, 0xEE, 20, 4);
  CHECK(SimBackgroundVoxelLandmarks_Classify(3, wram, &object, 1) == 1);
  CHECK(object.kind == kSimBackgroundVoxel_Pyramid);
  CHECK(object.cell_x == 20 && object.cell_y == 4);

  /* Towns with no reserved plot classify nothing at all. */
  memset(wram, 0, sizeof(wram));
  CHECK(SimBackgroundVoxelLandmarks_Classify(1, wram, &object, 1) == 0);
  CHECK(SimBackgroundVoxelLandmarks_Classify(4, wram, &object, 1) == 0);
  CHECK(SimBackgroundVoxelLandmarks_Classify(5, wram, &object, 1) == 0);

  /* A lone marked cell is terrain corruption, not a landmark: the whole 2x2
   * plot has to carry the expansion metatile. */
  memset(wram, 0, sizeof(wram));
  wram[CellIndex(2, 6, 16)] = 0xEC;
  wram[CellIndex(2, 7, 16)] = 0xEC;
  CHECK(SimBackgroundVoxelLandmarks_Classify(2, wram, &object, 1) == 0);

  if (failures) return 1;
  puts("sim background voxel landmark checks passed");
  return 0;
}
