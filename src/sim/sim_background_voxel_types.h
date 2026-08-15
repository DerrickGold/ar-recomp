#ifndef SIM_BACKGROUND_VOXEL_TYPES_H
#define SIM_BACKGROUND_VOXEL_TYPES_H

#include <stdint.h>

enum {
  kSimBackgroundCellPixels = 16,
  kSimBackgroundTownCells = 32,
  /* 128 structure records, one cathedral, and at most one tree object per
   * cell. Spare entries make overflow an invariant violation rather than an
   * ordinary developed-town condition. */
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
  /* Region and authentic structure subtype are model identity. House subtype
   * is the civilization level used by the ROM's regional art lookup. */
  uint8_t town;
  uint8_t development_level;
  uint8_t cell_x, cell_y;
  uint8_t source_cells_w, source_cells_h;
  uint8_t footprint_cells_w, footprint_cells_d;
  uint8_t height_pixels;
  /* Tree-only adjacency. Joined cells retain one source cell apiece so their
   * height does not grow with a component's bounding box. */
  uint8_t tree_edges;
  uint8_t record_slot;
} SimBackgroundVoxelObject;

#endif  /* SIM_BACKGROUND_VOXEL_TYPES_H */
