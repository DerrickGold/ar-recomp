#ifndef SIM_BACKGROUND_VOXELS_H
#define SIM_BACKGROUND_VOXELS_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_mountains.h"
#include "sim_town_canvas.h"
#include "sim_background_voxel_types.h"

typedef struct SimBackgroundVoxelScene {
  uint8_t town;
  bool overflow;
  uint16_t object_count;
  uint16_t tree_cell_count;
  uint16_t tree_group_count;
  /* Clearable single-cell brush: round bushes and Marahna's palms. */
  uint16_t brush_cell_count;
  SimBackgroundMountainField mountains;
  SimBackgroundMountainCaps mountain_caps;
  SimBackgroundVoxelObject objects[kSimBackgroundMaxObjects];
} SimBackgroundVoxelScene;

/* Pure classification seam, used by the game-thread builder and ROM-free
 * tests. Every object comes from town state - structure records, the cell map's
 * terrain metatile identities and its reserved landmark plots - so the result
 * does not depend on the current palette, brightness or fade. */
void SimBackgroundVoxels_Classify(uint8_t town, const uint8_t *wram,
                                  SimBackgroundVoxelScene *out);

void SimBackgroundVoxels_Reset(void);
/* Rebuilds only when the authentic town canvas changes. The original canvas
 * is never modified: this owns a cutout atlas plus an inpainted ground copy
 * used only by enhanced SIM presentation. */
void SimBackgroundVoxels_Build(uint8_t town, const uint8_t *wram,
                               const uint32_t *canvas_pixels,
                               const uint16_t *vram,
                               uint32_t canvas_serial);

uint32_t SimBackgroundVoxels_Serial(void);
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void);
const uint32_t *SimBackgroundVoxels_AtlasPixels(void);
const uint32_t *SimBackgroundVoxels_GroundPixels(void);

/* Resolves a clean terrain-metatile source synthesized in the mountain atlas.
 * Unlike SimBackgroundMountains_TileSource, this is independent of whether a
 * pristine instance happens to be visible in the town's composed cell map. */
bool SimBackgroundVoxels_MountainTileSource(uint8_t tile,
                                            int *cell_x, int *cell_y);

#endif  /* SIM_BACKGROUND_VOXELS_H */
