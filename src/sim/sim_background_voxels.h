#ifndef SIM_BACKGROUND_VOXELS_H
#define SIM_BACKGROUND_VOXELS_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_mountains.h"
#include "sim_town_canvas.h"

enum {
  kSimBackgroundCellPixels = 16,
  kSimBackgroundTownCells = 32,
  /* 128 structure records, one cathedral, and at most one tree object per
   * cell. The spare entries make overflow an invariant violation rather than
   * an ordinary developed-town condition. */
  kSimBackgroundMaxObjects = 1160,
};

typedef enum SimBackgroundVoxelKind {
  kSimBackgroundVoxel_House,
  kSimBackgroundVoxel_Cathedral,
  kSimBackgroundVoxel_Windmill,
  kSimBackgroundVoxel_Factory,
  kSimBackgroundVoxel_Tree,
} SimBackgroundVoxelKind;

enum { kSimBackgroundVoxelKindCount = 5 };

typedef enum SimBackgroundVoxelFlags {
  kSimBackgroundVoxel_UnderConstruction = 1u << 0,
  kSimBackgroundVoxel_IsolatedTree = 1u << 1,
  /* Completed-town house art uses the structure record's $40 variant as a
   * second, side-facing silhouette. Keep that presentation choice separate
   * from the construction-frame flag used by the larger structures. */
  kSimBackgroundVoxel_AlternateFacing = 1u << 2,
} SimBackgroundVoxelFlags;

typedef enum SimBackgroundTreeEdges {
  kSimBackgroundTreeEdge_North = 1u << 0,
  kSimBackgroundTreeEdge_East = 1u << 1,
  kSimBackgroundTreeEdge_South = 1u << 2,
  kSimBackgroundTreeEdge_West = 1u << 3,
} SimBackgroundTreeEdges;

typedef struct SimBackgroundVoxelObject {
  uint16_t group;
  uint8_t kind;
  uint8_t flags;
  uint8_t cell_x, cell_y;
  uint8_t source_cells_w, source_cells_h;
  uint8_t footprint_cells_w, footprint_cells_d;
  uint8_t height_pixels;
  /* Tree-only adjacency. Joined cells retain one source cell apiece so their
   * height does not grow with a component's bounding box. */
  uint8_t tree_edges;
  uint8_t record_slot;
} SimBackgroundVoxelObject;

typedef struct SimBackgroundVoxelScene {
  uint8_t town;
  bool overflow;
  uint16_t object_count;
  uint16_t tree_cell_count;
  uint16_t tree_group_count;
  SimBackgroundMountainField mountains;
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
                               uint32_t canvas_serial);

uint32_t SimBackgroundVoxels_Serial(void);
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void);
const uint32_t *SimBackgroundVoxels_AtlasPixels(void);
const uint32_t *SimBackgroundVoxels_GroundPixels(void);

#endif  /* SIM_BACKGROUND_VOXELS_H */
