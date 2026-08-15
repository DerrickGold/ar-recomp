#ifndef SIM_BACKGROUND_MOUNTAINS_H
#define SIM_BACKGROUND_MOUNTAINS_H

#include <stdbool.h>
#include <stdint.h>

enum {
  kSimBackgroundMountainTownCells = 32,
  kSimBackgroundMountainCellCount =
      kSimBackgroundMountainTownCells * kSimBackgroundMountainTownCells,
  kSimBackgroundMountainMaxCapTiles =
      kSimBackgroundMountainTownCells * 3 / 2,
};

typedef enum SimBackgroundMountainCellFlags {
  kSimBackgroundMountainCell_Occupied = 1u << 0,
  kSimBackgroundMountainCell_LavaCap = 1u << 1,
} SimBackgroundMountainCellFlags;

/* Static terrain classification is kept separate from buildings and trees.
 * `component` joins four-neighbour cells into stable chunks, while retaining
 * the authentic tile id for deterministic surface variation. */
typedef struct SimBackgroundMountainField {
  uint8_t town;
  uint16_t cell_count;
  uint16_t component_count;
  uint8_t flags[kSimBackgroundMountainCellCount];
  uint8_t tile[kSimBackgroundMountainCellCount];
  uint16_t component[kSimBackgroundMountainCellCount];
} SimBackgroundMountainField;

typedef enum SimBackgroundMountainCapTileFlags {
  kSimBackgroundMountainCapTile_MirrorX = 1u << 0,
} SimBackgroundMountainCapTileFlags;

typedef struct SimBackgroundMountainCapTile {
  int8_t cell_x, cell_y;
  uint8_t source_tile;
  uint8_t flags;
  uint16_t component;
} SimBackgroundMountainCapTile;

typedef struct SimBackgroundMountainCaps {
  uint8_t tile_count;
  SimBackgroundMountainCapTile tiles[kSimBackgroundMountainMaxCapTiles];
} SimBackgroundMountainCaps;

uint8_t SimBackgroundMountains_TileFlags(uint8_t town, uint8_t tile);
void SimBackgroundMountains_Classify(
    uint8_t town, const uint8_t *wram, SimBackgroundMountainField *out);
bool SimBackgroundMountains_CellOccupied(
    const SimBackgroundMountainField *field, int x, int y);
/* Completes common-range peaks that deliberately begin at the north edge of
 * the authentic 32x32 map. The returned sparse layout references original
 * mountain tile ids; renderers choose how to project those source tiles. */
void SimBackgroundMountains_BuildNorthCaps(
    const SimBackgroundMountainField *field, SimBackgroundMountainCaps *out);

#endif  /* SIM_BACKGROUND_MOUNTAINS_H */
