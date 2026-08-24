#include "sim_town_layout.h"

enum {
  kCellMapPageSide = 16,
  kCellMapPageBytes = 0x100,
};

size_t SimTownLayout_CellMapIndex(uint8_t town, int x, int y) {
  const int page = (y >= kCellMapPageSide ? 2 : 0) +
      (x >= kCellMapPageSide ? 1 : 0);
  return kSimTownCellMapsWram +
      (size_t)(town - 1) * kSimTownCellMapBytes +
      (size_t)page * kCellMapPageBytes +
      (size_t)(y & (kCellMapPageSide - 1)) * kCellMapPageSide +
      (size_t)(x & (kCellMapPageSide - 1));
}
