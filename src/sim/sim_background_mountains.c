#include "sim_background_mountains.h"

#include <stddef.h>
#include <string.h>

enum {
  kTownCellMapsWram = 0x12000,  /* flat $7F:2000 */
  kTownCellMapBytes = 0x400,
};

static size_t CellIndex(int x, int y) {
  return (size_t)y * kSimBackgroundMountainTownCells + x;
}

/* The cell maps use four 16x16 pages rather than row-major 32x32 storage. */
static size_t TownCellMapIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kTownCellMapsWram + (size_t)(town - 1) * kTownCellMapBytes +
      (size_t)quadrant * 0x100 + (size_t)(y & 15) * 16 + (x & 15);
}

uint8_t SimBackgroundMountains_TileFlags(uint8_t town, uint8_t tile) {
  if (town < 1 || town > 6) return 0;
  /* Marahna reuses ids from the common mountain range for isolated swamp art.
   * Its grey plateau walls are a separate terrain family and will receive an
   * edge-oriented mesh instead of being forced into this ridge heightfield. */
  if (town == 5) return 0;
  if (tile >= 0x78 && tile <= 0x9F)
    return kSimBackgroundMountainCell_Occupied;
  /* Aitos' two volcanic crown cells sit immediately before the shared ridge
   * range and carry the red cap visible in the source palette. */
  if (town == 4 && (tile == 0x70 || tile == 0x71))
    return kSimBackgroundMountainCell_Occupied |
        kSimBackgroundMountainCell_LavaCap;
  return 0;
}

bool SimBackgroundMountains_CellOccupied(
    const SimBackgroundMountainField *field, int x, int y) {
  if (!field || x < 0 || x >= kSimBackgroundMountainTownCells ||
      y < 0 || y >= kSimBackgroundMountainTownCells)
    return false;
  return (field->flags[CellIndex(x, y)] &
          kSimBackgroundMountainCell_Occupied) != 0;
}

void SimBackgroundMountains_Classify(
    uint8_t town, const uint8_t *wram, SimBackgroundMountainField *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (town < 1 || town > 6 || !wram) return;
  out->town = town;

  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      size_t cell = CellIndex(x, y);
      uint8_t tile = wram[TownCellMapIndex(town, x, y)];
      out->tile[cell] = tile;
      out->flags[cell] = SimBackgroundMountains_TileFlags(town, tile);
      if (out->flags[cell] & kSimBackgroundMountainCell_Occupied)
        out->cell_count++;
    }

  uint16_t queue[kSimBackgroundMountainCellCount];
  for (int start_y = 0; start_y < kSimBackgroundMountainTownCells; start_y++)
    for (int start_x = 0; start_x < kSimBackgroundMountainTownCells;
         start_x++) {
      size_t start = CellIndex(start_x, start_y);
      if (!(out->flags[start] & kSimBackgroundMountainCell_Occupied) ||
          out->component[start])
        continue;
      uint16_t component = ++out->component_count;
      int read = 0, write = 0;
      queue[write++] = (uint16_t)start;
      out->component[start] = component;
      while (read < write) {
        int cell = queue[read++];
        int x = cell % kSimBackgroundMountainTownCells;
        int y = cell / kSimBackgroundMountainTownCells;
        static const int dx[] = {0, 1, 0, -1};
        static const int dy[] = {-1, 0, 1, 0};
        for (int edge = 0; edge < 4; edge++) {
          int nx = x + dx[edge], ny = y + dy[edge];
          if (!SimBackgroundMountains_CellOccupied(out, nx, ny)) continue;
          size_t next = CellIndex(nx, ny);
          if (out->component[next]) continue;
          out->component[next] = component;
          queue[write++] = (uint16_t)next;
        }
      }
    }
}
