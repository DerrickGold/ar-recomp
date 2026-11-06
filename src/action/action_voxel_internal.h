#ifndef AR_ACTION_VOXEL_INTERNAL_H
#define AR_ACTION_VOXEL_INTERNAL_H
#include "action/action_voxel.h"
#include <stdatomic.h>
enum { kVoxelPassLists = 4, kVoxelMaxRearFills = 256 };
typedef struct VoxelRearFill {
  short x, y;
  bool preserve_existing;
  uint16_t colors[16 * 16]; /* RGB555; bit 15 marks an opaque source pixel. */
} VoxelRearFill;
struct ActionVoxelGeometry {
  atomic_uint references;
  uint64_t revision;
  unsigned char *front;
  unsigned char *cutouts; /* Optional BG1 matte mask, bit 7 leftmost, width/8 bytes per row. */
  unsigned char *branch_pixels; /* Optional one-bit depth tag per world pixel. */
  unsigned char *front_recess; /* Optional recession tag per 8px world cell. */
  unsigned char *thin_trim_cells; /* Reviewed foreground timber uses a shallow depth per 8px cell. */
  unsigned char *pixel_recess_cells; /* One-based sparse mask owner per 8px cell. */
  uint8_t pixel_recess_rows[kVoxelMaxPixelRecessCells][8];
  unsigned pixel_recess_mask_count, pixel_recess_cell_count;
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
  VoxelRearFill *rear_fills;
  unsigned rear_fill_count, rear_fill_capacity;
  unsigned active_group, active_room;
  ActionVoxelProfile profile;
  VoxelEdge edges[kVoxelMaxEdges];
  unsigned edge_count, rear_trim_count, high_deck_count;
  VoxelRearTrim rear_trims[kVoxelMaxRearTrims];
  VoxelGate gates[kVoxelMaxGates];
  unsigned gate_count;
  VoxelPatch patches[kVoxelMaxPatches];
  unsigned patch_count;
  VoxelWaterSurface water_surfaces[kVoxelMaxWaterSurfaces];
  unsigned water_surface_count;
  VoxelWaterfall waterfalls[kVoxelMaxWaterfalls];
  unsigned waterfall_count;
  uint16_t pass_edges[kVoxelPassLists][kVoxelMaxEdges];
  unsigned pass_counts[kVoxelPassLists];
  ActionVoxelSettings settings;
  bool ready;
  bool mixed_scenery; /* Reviewed front trim and rear-mounted details coexist. */
};
static inline float ActionVoxel_LocalTrimDepth(const ActionVoxelGeometry *c,bool thin) {
  return thin ? ActionVoxel_Depth(c)*.10f : ActionVoxel_TrimDepth(c);
}
#endif
