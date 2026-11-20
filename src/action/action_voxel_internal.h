#ifndef AR_ACTION_VOXEL_INTERNAL_H
#define AR_ACTION_VOXEL_INTERNAL_H
#include "action/action_voxel.h"
#include <stdatomic.h>
/* Northwall's climbing cave includes over650 native void cells in addition to
 * its sparse architecture repairs. Capacity is allocated only when used. */
enum { kVoxelPassLists = 4, kVoxelMaxRearFills = 1024, kVoxelMaxLintelReturns = 32 };
void ActionVoxel_FreeBuffers(ActionVoxelGeometry *c);
/* The buffer must be zero initialized and sized to width * height. */
void ActionVoxel_FillRecordOwners(const DioramaVoxelEdits *edits, int x, int y,
    int width, int height, uint16_t *owners);
typedef struct VoxelRearFill {
  short x, y;
  bool preserve_existing, replace_front; /* Reviewed baked shadows may need both native split images repaired. */
  uint16_t colors[16 * 16]; /* RGB555; bit 15 marks an opaque source pixel. */
} VoxelRearFill;
struct ActionVoxelGeometry {
  atomic_uint references;
  uint64_t revision;
  DioramaVoxelEdits edits;
  VoxelAuthoredCell *authored;
  unsigned authored_count;
  unsigned hidden_native_front_count, native_transform_count, transformed_rear_count;
  uint16_t *authored_at; /* One-based cell owner; O(1) world lookup. */
  uint16_t (*authored_art)[256];
  unsigned authored_art_count, authored_art_capacity;
  uint16_t *authored_cap_art; /* Unique atlas IDs used by surviving automatic caps. */
  unsigned authored_cap_art_count;
  VoxelSurfaceStats surface_stats;
  VoxelAuthoredContour *authored_contours;
  unsigned authored_contour_count,authored_contour_capacity;
  unsigned build_error;
  unsigned char *front;
  uint8_t *split_rules; /* Immutable rear/front ownership for the complex split. */
  unsigned char *semantic_cells; /* Native roles per16px cell, before authored overlays. */
  unsigned char *native_priority_decks; /* Per16px quadrant mask of reviewed solid native-priority beams. */
  unsigned char *cutouts; /* Optional BG1 matte mask, bit 7 leftmost, width/8 bytes per row. */
  unsigned char *branch_pixels; /* Optional one-bit depth tag per world pixel. */
  unsigned char *front_recess; /* Optional recession tag per 8px world cell. */
  uint8_t *front_classes; /* Combined immutable front topology inputs. */
  unsigned char *thin_trim_cells; /* Reviewed foreground timber uses a shallow depth per 8px cell. */
  unsigned char *raised_scenery_cells; /* Reviewed rear architecture projects slightly beyond adjacent trim. */
  unsigned char *pixel_recess_cells; /* One-based sparse mask owner per 8px cell. */
  uint8_t pixel_recess_rows[kVoxelMaxPixelRecessMasks][8];
  unsigned pixel_recess_mask_count, pixel_recess_cell_count, pixel_recess_extra_faces;
  unsigned char *trunk_cells; /* One-based curve owner per 8px cell, including empty filter halo. */
  VoxelTrunkCurve trunk_curves[kVoxelMaxTrunkCurves];
  unsigned trunk_curve_count;
  unsigned char *column_cells;
  VoxelTrunkCurve column_curves[kVoxelMaxCastleColumns];
  unsigned column_curve_count;
  unsigned char *rear_relief; /* Optional smoothed depth weight per 16px cell. */
  unsigned char *soft_surfaces; /* Reviewed granular contour shading per 16px cell. */
  int origin_x, origin_y, width, height;
  uint16_t backing_color[16 * 32];
  VoxelTerminalMatte terminal_matte;
  VoxelJungleTerminal *jungle_terminal; /* Optional only for reviewed05/03. */
  VoxelRearFill *rear_fills;
  unsigned rear_fill_count, rear_fill_capacity;
  unsigned active_group, active_room;
  ActionVoxelProfile profile;
  VoxelEdge edges[kVoxelMaxEdges];
  unsigned edge_count, rear_trim_count, high_deck_count;
  VoxelRearTrim rear_trims[kVoxelMaxRearTrims];
  VoxelLintelReturn lintel_returns[kVoxelMaxLintelReturns];
  unsigned lintel_return_count;
  VoxelGate gates[kVoxelMaxGates];
  unsigned gate_count;
  VoxelPatch patches[kVoxelMaxPatches];
  unsigned patch_count;
  VoxelProfileSurface *profile_surfaces;
  unsigned profile_surface_count, profile_surface_capacity;
  VoxelWaterSurface water_surfaces[kVoxelMaxWaterSurfaces];
  unsigned water_surface_count;
  VoxelWaterfall waterfalls[kVoxelMaxWaterfalls];
  unsigned waterfall_count;
  uint16_t pass_edges[kVoxelPassLists][kVoxelMaxEdges];
  unsigned pass_counts[kVoxelPassLists];
  VoxelEdgeChunk pass_chunks[kVoxelPassLists][(kVoxelMaxEdges+kVoxelEdgeChunkSize-1)/kVoxelEdgeChunkSize];
  ActionVoxelSettings settings;
  bool ready;
  bool edge_chunks_ready;
  bool mixed_scenery; /* Reviewed front trim and rear-mounted details coexist. */
  bool contact_scenery; /* Reviewed shallow objects follow native actor contact. */
  bool material_rear_mounted; /* Mapped reviewed relief enables the rear-face policy. */
};
void ActionVoxel_PrepareFrontClasses(ActionVoxelGeometry *c);
void ActionVoxel_PrepareSplitRules(ActionVoxelGeometry *c);
static inline float ActionVoxel_LocalTrimDepth(const ActionVoxelGeometry *c,bool thin) {
  return thin ? ActionVoxel_Depth(c)*.10f : ActionVoxel_TrimDepth(c);
}
static inline float ActionVoxel_SceneryLift(const ActionVoxelGeometry *c,bool raised) {
  return raised ? ActionVoxel_Depth(c)*.125f : 0;
}
static inline float ActionVoxel_SceneryFront(const ActionVoxelGeometry *c,bool rear,bool raised) {
  return rear ? ActionVoxel_SceneryRecess(c)-ActionVoxel_SceneryLift(c,raised) : 0;
}
static inline float ActionVoxel_SceneryBack(const ActionVoxelGeometry *c,float front,bool thin,bool raised) {
  /* A lift adds relief toward the viewer, never a gap at the original wall. */
  return (raised?ActionVoxel_SceneryRecess(c):front)+ActionVoxel_LocalTrimDepth(c,thin);
}
#endif
