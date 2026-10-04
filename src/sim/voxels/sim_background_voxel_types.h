#ifndef AR_SIM_BACKGROUND_VOXEL_TYPES_H
#define AR_SIM_BACKGROUND_VOXEL_TYPES_H
/* SimBackgroundVoxel types: the object record the voxel pipeline passes
 * around, one per enhanced town object (houses, landmarks, trees, bridges),
 * with its kind, footprint, construction state and animation phase.
 * Phase: pure (types only). */

#include <stdint.h>

#include "sim/town/sim_structure_visuals.h"
#include "sim/sim_world_map.h"

enum {
  kSimBackgroundCellPixels = kSimTownCellPixels,
  kSimBackgroundTownCells = kSimTownCells,
  kSimBackgroundTownCount = kSimTownCount,
  kSimBackgroundVoxelNoRecordSlot = UINT8_MAX,
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
  /* Clearable brush, distinct from the permanent forest families. */
  kSimBackgroundVoxel_Palm,
  kSimBackgroundVoxel_Shrub,
  kSimBackgroundVoxel_StoryTree,
  kSimBackgroundVoxel_BloodpoolCastle,
  kSimBackgroundVoxel_MarahnaTemple,
  kSimBackgroundVoxel_Pyramid,
  /* A live $E1/$E2 cell-map bridge. Geometry spans to the first solid bank in
   * each direction and uses stone from the native structure graphic. */
  kSimBackgroundVoxel_Bridge,
  /* Native terrain $61 and scattered-stone $62/$63/$69-$6B. */
  kSimBackgroundVoxel_Boulder,
  kSimBackgroundVoxel_Rocks,
  /* Aitos' class-5 pasture: individual low posts around a 2x2 grass plot. */
  kSimBackgroundVoxel_AnimalPen,
} SimBackgroundVoxelKind;

enum {
  kSimBackgroundVoxelKindCount = kSimBackgroundVoxel_AnimalPen + 1,
};

typedef enum SimBackgroundBridgeAxis {
  kSimBackgroundBridgeAxis_None,
  kSimBackgroundBridgeAxis_EastWest,
  kSimBackgroundBridgeAxis_NorthSouth,
  kSimBackgroundBridgeAxis_Count,
} SimBackgroundBridgeAxis;

typedef enum SimBackgroundVoxelFlags {
  /* Genuinely unfinished art. This is NOT the structure record's `$40` flag:
   * see the windmill note on `animation_phase` below. It is resolved from the
   * frame the town tilemap is actually displaying. An unrecognised frame does
   * not set this flag or publish a replacement object; authentic art remains. */
  kSimBackgroundVoxel_UnderConstruction = 1u << 0,
  kSimBackgroundVoxel_IsolatedTree = 1u << 1,
  /* Completed-town house art uses the structure record's $40 variant as a
   * second, side-facing silhouette. Keep that presentation choice separate
   * from the construction-frame flag used by the larger structures. */
  kSimBackgroundVoxel_AlternateFacing = 1u << 2,
  /* Resolved town artwork, independent of language/gameplay region. Set only
   * when the selected Japanese decoration is available to the original BG. */
  kSimBackgroundVoxel_PyramidEye = 1u << 3,
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
  /* Tree-only adjacency selects a dense stand for joined forest cells.
   * Each retains one source cell so its height does not grow with the patch. */
  uint8_t tree_edges;
  uint8_t record_slot;
  /* Bridge-only semantic anchors. A is west/north and B is east/south. They
   * are solid bank cells, not the water cell carrying the bridge graphic. */
  uint8_t bridge_axis;
  uint8_t bridge_bank_a_x, bridge_bank_a_y;
  uint8_t bridge_bank_b_x, bridge_bank_b_y;
  /* Which authored animation frame this structure is currently showing, as an
   * index into its family's frame cycle. Windmills use all three values for
   * both scaffold growth and blade position; houses use 0/1 for their two
   * scaffold stages. Part of the model cache key. */
  uint8_t animation_phase;
  /* Shared construction/finished state resolved from the live structure
   * metatile. `Unknown` objects are never published: enhanced replacement
   * stands down so the authentic flat art remains visible. */
  uint8_t visual_state;
  /* Top-left structure-atlas metatile that produced `visual_state`. Finished
   * houses also use its family for geometry/palette identity; town and tier
   * alone cannot distinguish Japan's developed Marahna stilt house. Rocks
   * carry their terrain-atlas metatile here to retain the native layout. */
  uint8_t visual_metatile;
} SimBackgroundVoxelObject;

#endif  /* AR_SIM_BACKGROUND_VOXEL_TYPES_H */
