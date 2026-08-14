#ifndef ACTRAISER_TOWN_METATILE_H
#define ACTRAISER_TOWN_METATILE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"

typedef enum ActRaiserTownMetatileAtlas {
  kActRaiserTownMetatileAtlas_Terrain = 0,
  kActRaiserTownMetatileAtlas_Structure,
} ActRaiserTownMetatileAtlas;

/* Copy one 2x2 metatile into the quadrant-paged simulation-town tilemap.
 * This semantic entry point intentionally has no architectural CPU side
 * effects beyond the tilemap writes. */
void ActRaiser_CopyTownMetatile(CpuState *cpu, uint8_t destination_bank,
                                uint16_t cell_x, uint16_t cell_y,
                                uint8_t metatile_id,
                                ActRaiserTownMetatileAtlas atlas);

/* Execute a structure-atlas draw list stored in ROM bank $03. Lists contain a
 * byte count followed by {cell_x_offset, cell_y_offset, metatile_id} triples.
 * The command ceiling lets host extensions reject an unexpectedly large list
 * without duplicating the format decoder. This semantic entry point changes
 * only the destination tilemap. */
bool ActRaiser_ExecuteTownDrawList(CpuState *cpu, uint8_t destination_bank,
                                  uint16_t origin_cell_x,
                                  uint16_t origin_cell_y,
                                  uint16_t draw_list_address,
                                  unsigned maximum_commands);

/* Whole-body HLEs for the terrain- and structure-atlas ROM copies. */
RecompReturn ActRaiser_TownCopyTerrainMetatile(CpuState *cpu);
RecompReturn ActRaiser_TownCopyStructureMetatile(CpuState *cpu);

/* Whole-body HLE for the structure rebuild draw-list interpreter. */
RecompReturn ActRaiser_TownExecuteDrawList(CpuState *cpu);

#endif /* ACTRAISER_TOWN_METATILE_H */
