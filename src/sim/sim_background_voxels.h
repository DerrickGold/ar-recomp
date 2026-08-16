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
  SimBackgroundMountainField mountains;
  SimBackgroundMountainCaps mountain_caps;
  SimBackgroundVoxelObject objects[kSimBackgroundMaxObjects];
} SimBackgroundVoxelScene;

/* Pure classification seam, used by the game-thread builder and ROM-free
 * tests. `canvas_pixels` is the complete 512x512 town canvas. */
void SimBackgroundVoxels_Classify(uint8_t town, const uint8_t *wram,
                                  const uint32_t *canvas_pixels,
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
