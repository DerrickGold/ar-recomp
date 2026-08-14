#ifndef ACTRAISER_CELL_MAP_H
#define ACTRAISER_CELL_MAP_H

#include <stdint.h>

#include "cpu_state.h"

/* Convert a logical town/cell coordinate into the quadrant-paged byte index
 * used by the simulation mark map at $7F:2000. */
uint16_t ActRaiser_CellMarkIndex(unsigned town, uint8_t cell_x,
                                uint8_t cell_y);

/* Whole-body HLE for the stock CPU helper at $03:9710. */
RecompReturn ActRaiser_TownCellMarkIndex(CpuState *cpu);

#endif /* ACTRAISER_CELL_MAP_H */
