#ifndef ACTRAISER_CELL_MAP_H
#define ACTRAISER_CELL_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp/game/cpu.h"

typedef enum ActRaiserTownStructureMarkShape {
  kActRaiserTownStructureMarkShape_Cell = 0,
  kActRaiserTownStructureMarkShape_Block2x2,
} ActRaiserTownStructureMarkShape;

/* Convert a logical town/cell coordinate into the quadrant-paged byte index
 * used by the simulation mark map at $7F:2000. */
uint16_t ActRaiser_CellMarkIndex(unsigned town, uint8_t cell_x,
                                uint8_t cell_y);

/* Return $03:9710's pre-quadrant cell index, which it exposes through the
 * $7C05 scratch word before producing ActRaiser_CellMarkIndex(). */
uint16_t ActRaiser_CellMarkPreQuadrantIndex(unsigned town, uint8_t cell_x,
                                           uint8_t cell_y);

/* Write a one-cell or 2x2 structure identity into the simulation town's
 * quadrant-paged cell map. Returns the top-left cell index. */
uint16_t ActRaiser_WriteTownStructureMark(
    CpuState *cpu, uint8_t data_bank, unsigned town, uint8_t cell_x,
    uint8_t cell_y, uint8_t mark,
    ActRaiserTownStructureMarkShape shape);

/* True when terrain collision or the current traversal's visited marker makes
 * a town cell unavailable to the build-direction pathfinder. */
bool ActRaiser_IsTownCellTraversalBlocked(uint16_t metatile_top_left_word,
                                          uint8_t cell_flags);

/* Whole-body HLE for the stock CPU helper at $03:9710. */
RecompReturn ActRaiser_TownCellMarkIndex(CpuState *cpu);

/* Whole-body HLEs for the stock one-cell and 2x2 structure-mark writers at
 * $03:9FCD and $03:9FE4. */
RecompReturn ActRaiser_WriteTownStructureMarkCell(CpuState *cpu);
RecompReturn ActRaiser_WriteTownStructureMarkBlock(CpuState *cpu);

/* Whole-body HLE for the stock traversal predicate at $03:96EF. */
RecompReturn ActRaiser_TownCellTestTraversalBlocked(CpuState *cpu);

#endif /* ACTRAISER_CELL_MAP_H */
