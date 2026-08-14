#ifndef SIM_BACKGROUND_VOXEL_QUALITY_H
#define SIM_BACKGROUND_VOXEL_QUALITY_H

/* Stable stored values for the player-facing SIM voxel performance target.
 * Append new levels rather than reordering these: settings.ini may store the
 * numeric enum value as well as its label. */
typedef enum SimBackgroundVoxelDetail {
  kSimBackgroundVoxelDetail_Low = 0,
  kSimBackgroundVoxelDetail_Balanced,
  kSimBackgroundVoxelDetail_High,
  kSimBackgroundVoxelDetail_Ultra,
  kSimBackgroundVoxelDetail_Count,
} SimBackgroundVoxelDetail;

#endif  /* SIM_BACKGROUND_VOXEL_QUALITY_H */
