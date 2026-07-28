#ifndef SIM_WORLD_MAP_COMPOSE_H
#define SIM_WORLD_MAP_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim_world_map.h"

enum {
  kSimWorldMapTownCount = 6,
  kSimWorldMapTownCells = 32 * 32,
  kSimWorldMapOrdinaryTranslationCount = 256,
  kSimWorldMapSpecialTranslationCount = 13,
  kSimWorldMapSpecialTilesPerCell = 4,
};

/* Immutable data read by the developed-world overlay at $02:865C. Keeping the
 * decoded destination words here makes the HLE independent of ROM layout after
 * initialization. */
typedef struct SimWorldMapRomTables {
  uint8_t ordinary[kSimWorldMapOrdinaryTranslationCount];
  uint8_t special[kSimWorldMapSpecialTranslationCount]
                 [kSimWorldMapSpecialTilesPerCell];
  uint16_t town_destination[kSimWorldMapTownCount];
} SimWorldMapRomTables;

/* Decode the three small tables used by the HLE from their verified LoROM
 * locations. Returns false without modifying `out` when the ROM is too short. */
bool SimWorldMap_LoadRomTables(SimWorldMapRomTables *out,
                               const uint8_t *rom_data, size_t rom_size);

/* Pure host equivalent of the dynamic portion of $02:B475 / $02:865C.
 *
 * `town_maps` contains six quadrant-paged 32x32 cell maps: four contiguous
 * 16x16 pages in top-left, top-right, bottom-left, bottom-right order.
 * `town_enabled` is interpreted only as zero/nonzero. `world_flags` bit 0
 * suppresses the base map's small 8x8 clear when set, matching the ROM.
 *
 * The function writes exactly one complete 128x128 tilemap to `out` and
 * mutates no input or emulator state. */
bool SimWorldMap_ComposeDeveloped(
    uint8_t out[kSimWorldMapBytes],
    const uint8_t baseline[kSimWorldMapBytes],
    const uint8_t town_maps[kSimWorldMapTownCount][kSimWorldMapTownCells],
    const uint16_t town_enabled[kSimWorldMapTownCount],
    uint8_t world_flags,
    const SimWorldMapRomTables *tables);

#endif /* SIM_WORLD_MAP_COMPOSE_H */
