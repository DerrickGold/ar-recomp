#ifndef SIM_BACKGROUND_MOUNTAINS_H
#define SIM_BACKGROUND_MOUNTAINS_H

#include <stdbool.h>
#include <stdint.h>

enum {
  kSimBackgroundMountainTownCells = 32,
  kSimBackgroundMountainCellCount =
      kSimBackgroundMountainTownCells * kSimBackgroundMountainTownCells,
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

uint8_t SimBackgroundMountains_TileFlags(uint8_t town, uint8_t tile);
void SimBackgroundMountains_Classify(
    uint8_t town, const uint8_t *wram, SimBackgroundMountainField *out);
bool SimBackgroundMountains_CellOccupied(
    const SimBackgroundMountainField *field, int x, int y);

#endif  /* SIM_BACKGROUND_MOUNTAINS_H */
