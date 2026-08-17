#include "sim_background_voxel_landmarks.h"

#include <stdbool.h>

enum {
  kTownCellMapsWram = 0x12000,  /* flat $7F:2000 */
  kTownCellMapBytes = 0x400,
  kLandmarkCells = 2,
};

typedef struct SimBackgroundLandmarkDefinition {
  uint8_t town;
  /* Cell-map metatile marking every cell of the landmark plot. $E0-$EF are
   * the special-structure expansion marks: unlike ordinary terrain ids they
   * appear exactly once per town, over the landmark's own 2x2 block. */
  uint8_t metatile;
  uint8_t kind;
  uint8_t height_pixels;
} SimBackgroundLandmarkDefinition;

/* Measured from the six resident $7F:2000 cell maps: Bloodpool's castle at
 * (6,16), Kasandora's pyramid at (20,4) and Northwall's ancient tree at
 * (26,14). Every one is a 2x2 block whose art fills its 32x32 pixels exactly,
 * so source and footprint are the same rectangle. Fillmore, Aitos and Marahna
 * carry no landmark of this class; Marahna's distinct temple is its cathedral
 * variant and is classified with the other cathedrals. */
static const SimBackgroundLandmarkDefinition kLandmarks[] = {
  {2, 0xEC, kSimBackgroundVoxel_BloodpoolCastle, 32},
  {3, 0xEE, kSimBackgroundVoxel_Pyramid, 28},
  {6, 0xEB, kSimBackgroundVoxel_StoryTree, 30},
};

/* The cell maps use four 16x16 pages rather than row-major 32x32 storage. */
static size_t TownCellMapIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kTownCellMapsWram + (size_t)(town - 1) * kTownCellMapBytes +
      (size_t)quadrant * 0x100 + (size_t)(y & 15) * 16 + (x & 15);
}

static bool PlotIsMarked(uint8_t town, const uint8_t *wram,
                         uint8_t metatile, int x, int y) {
  for (int row = 0; row < kLandmarkCells; row++)
    for (int column = 0; column < kLandmarkCells; column++)
      if (wram[TownCellMapIndex(town, x + column, y + row)] != metatile)
        return false;
  return true;
}

/* One landmark per town, so the first marked plot is the whole answer: a
 * second hit could only be an overlapping window onto the same block. */
static bool FindPlot(uint8_t town, const uint8_t *wram, uint8_t metatile,
                     int *plot_x, int *plot_y) {
  for (int y = 0; y <= kSimBackgroundTownCells - kLandmarkCells; y++)
    for (int x = 0; x <= kSimBackgroundTownCells - kLandmarkCells; x++)
      if (PlotIsMarked(town, wram, metatile, x, y)) {
        *plot_x = x;
        *plot_y = y;
        return true;
      }
  return false;
}

size_t SimBackgroundVoxelLandmarks_Classify(
    uint8_t town, const uint8_t *wram,
    SimBackgroundVoxelObject *objects, size_t capacity) {
  if (!wram || !objects || !capacity || town < 1 || town > 6) return 0;
  size_t count = 0;
  for (size_t at = 0; at < sizeof(kLandmarks) / sizeof(kLandmarks[0]); at++) {
    const SimBackgroundLandmarkDefinition *landmark = &kLandmarks[at];
    int plot_x, plot_y;
    if (landmark->town != town ||
        !FindPlot(town, wram, landmark->metatile, &plot_x, &plot_y))
      continue;
    objects[count++] = (SimBackgroundVoxelObject){
      .town = town,
      .kind = landmark->kind,
      .cell_x = (uint8_t)plot_x,
      .cell_y = (uint8_t)plot_y,
      .source_cells_w = kLandmarkCells,
      .source_cells_h = kLandmarkCells,
      .footprint_cells_w = kLandmarkCells,
      .footprint_cells_d = kLandmarkCells,
      .height_pixels = landmark->height_pixels,
      .record_slot = 0xFF,
    };
    if (count == capacity) break;
  }
  return count;
}
