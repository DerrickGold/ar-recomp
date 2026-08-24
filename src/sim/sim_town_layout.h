#ifndef SIM_TOWN_LAYOUT_H
#define SIM_TOWN_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

enum {
  kSimTownCellMapsWram = 0x12000,  /* flat $7F:2000 */
  kSimTownCellMapBytes = 0x400,
};

/* Convert a one-based town and a 32x32 cell coordinate to flat WRAM. */
size_t SimTownLayout_CellMapIndex(uint8_t town, int x, int y);

#endif
