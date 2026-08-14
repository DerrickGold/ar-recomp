#ifndef ACTRAISER_CELL_MAP_H
#define ACTRAISER_CELL_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"

/* Convert a logical town/cell coordinate into the quadrant-paged byte index
 * used by the simulation mark map at $7F:2000. */
uint16_t ActRaiser_CellMarkIndex(unsigned town, uint8_t cell_x,
                                uint8_t cell_y);

/* True when terrain collision or the current traversal's visited marker makes
 * a town cell unavailable to the build-direction pathfinder. */
bool ActRaiser_IsTownCellTraversalBlocked(uint16_t metatile_top_left_word,
                                          uint8_t cell_flags);

/* Whole-body HLE for the stock CPU helper at $03:9710. */
RecompReturn ActRaiser_TownCellMarkIndex(CpuState *cpu);

/* Whole-body HLE for the stock traversal predicate at $03:96EF. */
RecompReturn ActRaiser_TownCellTestTraversalBlocked(CpuState *cpu);

#endif /* ACTRAISER_CELL_MAP_H */
