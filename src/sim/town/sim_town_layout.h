#ifndef AR_SIM_TOWN_LAYOUT_H
#define AR_SIM_TOWN_LAYOUT_H
/* SimTownLayout: where each town's 32 x 32 cell map lives in WRAM ($7F:2000,
 * $400 bytes per town) and how a cell coordinate maps into it.
 * Phase: pure.
 * Tests: tests/sim_render_metadata_test.c */

#include <stddef.h>
#include <stdint.h>

enum {
  kSimTownCellMapsWram = 0x12000,  /* flat $7F:2000 */
  kSimTownCellMapBytes = 0x400,
};

/* Convert a one-based town and a 32x32 cell coordinate to flat WRAM. */
size_t SimTownLayout_CellMapIndex(uint8_t town, int x, int y);

#endif
