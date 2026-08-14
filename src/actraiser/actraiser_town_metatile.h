#ifndef ACTRAISER_TOWN_METATILE_H
#define ACTRAISER_TOWN_METATILE_H

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

/* Whole-body HLEs for the terrain- and structure-atlas ROM copies. */
RecompReturn ActRaiser_TownCopyTerrainMetatile(CpuState *cpu);
RecompReturn ActRaiser_TownCopyStructureMetatile(CpuState *cpu);

#endif /* ACTRAISER_TOWN_METATILE_H */
