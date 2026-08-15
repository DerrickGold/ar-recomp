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

typedef enum SimBackgroundVoxelLod {
  kSimBackgroundVoxelLod_Fixed = 0,
  kSimBackgroundVoxelLod_Adaptive,
  kSimBackgroundVoxelLod_Count,
} SimBackgroundVoxelLod;

/* Independent presentation boundaries let players spend performance where it
 * is visible to them. These values are persisted by settings.ini; append new
 * modes instead of reordering existing ones. */
typedef enum SimBackgroundVoxelShading {
  kSimBackgroundVoxelShading_Basic = 0,
  kSimBackgroundVoxelShading_AmbientOcclusion,
  kSimBackgroundVoxelShading_MaterialAware,
  kSimBackgroundVoxelShading_Count,
} SimBackgroundVoxelShading;

typedef enum SimBackgroundVoxelStyle {
  kSimBackgroundVoxelStyle_Basic = 0,
  kSimBackgroundVoxelStyle_Trim,
  kSimBackgroundVoxelStyle_Architectural,
  kSimBackgroundVoxelStyle_Varied,
  kSimBackgroundVoxelStyle_Count,
} SimBackgroundVoxelStyle;

typedef enum SimBackgroundVoxelFacing {
  kSimBackgroundVoxelFacing_Shared = 0,
  kSimBackgroundVoxelFacing_PerModel,
  kSimBackgroundVoxelFacing_Count,
} SimBackgroundVoxelFacing;

typedef enum SimBackgroundVoxelRenderScale {
  kSimBackgroundVoxelRenderScale_Native = 0,
  kSimBackgroundVoxelRenderScale_2x,
  kSimBackgroundVoxelRenderScale_PixelClean,
  kSimBackgroundVoxelRenderScale_Count,
} SimBackgroundVoxelRenderScale;

#endif  /* SIM_BACKGROUND_VOXEL_QUALITY_H */
