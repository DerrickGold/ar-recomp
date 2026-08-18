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
  /* Permanent broad round canopies: Marahna's mangroves, Kasandora's oasis
   * stands. A separate metatile family from the pointed evergreen, and in
   * Marahna the permanent forest while the palms are the clearable brush. */
  kSimBackgroundVoxel_BroadTree,
  /* The two clearable brush entries. They share the evergreen's cube crown so
   * the town's vegetation is one style, and differ by shape and palette. */
  kSimBackgroundVoxel_Palm,
  kSimBackgroundVoxel_Shrub,
  kSimBackgroundVoxel_StoryTree,
  kSimBackgroundVoxel_BloodpoolCastle,
  kSimBackgroundVoxel_MarahnaTemple,
  kSimBackgroundVoxel_Pyramid,
} SimBackgroundVoxelKind;

enum {
  kSimBackgroundVoxelKindCount = kSimBackgroundVoxel_Pyramid + 1,
};

typedef enum SimBackgroundVoxelFlags {
  /* Genuinely unfinished art. This is NOT the structure record's `$40` flag:
   * see the windmill note on `animation_phase` below. It is resolved from the
   * frame the town tilemap is actually displaying, so a structure can only
   * lose its finished model when the game itself redraws a scaffold. */
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
  /* Tree-only adjacency. Joined cells retain one source cell apiece so their
   * height does not grow with a component's bounding box. */
  uint8_t tree_edges;
  uint8_t record_slot;
  /* Which authored animation frame this structure is currently showing, as an
   * index into its family's frame cycle. Windmills are the only animated
   * family today: their visual step program ($03:D573 rebuild / $03:D6F4
   * construction, class 6) cycles three 2x2 metatile sets whose blades sit 30
   * degrees apart, and the "no wind" story event ($03:E2BB) parks every mill
   * on one of them until the Wind miracle queues record action 6 ($03:A1F4)
   * and the cycle resumes. Reading the displayed frame therefore gives the
   * spin, its pause and its restart from one source, with no host clock to
   * drift against the authentic art. Part of the model cache key. */
  uint8_t animation_phase;
} SimBackgroundVoxelObject;

#endif  /* SIM_BACKGROUND_VOXEL_TYPES_H */
