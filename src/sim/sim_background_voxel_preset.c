#include "sim_background_voxel_preset.h"

SimBackgroundVoxelPresetConfig SimBackgroundVoxelPreset_Resolve(
    SimBackgroundVoxelPreset preset,
    SimBackgroundVoxelPresetConfig custom) {
  switch (preset) {
    case kSimBackgroundVoxelPreset_Off:
      return (SimBackgroundVoxelPresetConfig){
        .enabled = false,
        .detail = kSimBackgroundVoxelDetail_Low,
        .lod = kSimBackgroundVoxelLod_Adaptive,
        .shading = kSimBackgroundVoxelShading_Basic,
        .style = kSimBackgroundVoxelStyle_Basic,
        .facing = kSimBackgroundVoxelFacing_Shared,
        .render_scale = kSimBackgroundVoxelRenderScale_Native,
      };
    case kSimBackgroundVoxelPreset_Performance:
      return (SimBackgroundVoxelPresetConfig){
        .enabled = true,
        .detail = kSimBackgroundVoxelDetail_Low,
        .lod = kSimBackgroundVoxelLod_Adaptive,
        .shading = kSimBackgroundVoxelShading_Basic,
        .style = kSimBackgroundVoxelStyle_Basic,
        .facing = kSimBackgroundVoxelFacing_Shared,
        .render_scale = kSimBackgroundVoxelRenderScale_Native,
      };
    case kSimBackgroundVoxelPreset_Balanced:
      return (SimBackgroundVoxelPresetConfig){
        .enabled = true,
        .detail = kSimBackgroundVoxelDetail_High,
        .lod = kSimBackgroundVoxelLod_Adaptive,
        .shading = kSimBackgroundVoxelShading_AmbientOcclusion,
        .style = kSimBackgroundVoxelStyle_Varied,
        .facing = kSimBackgroundVoxelFacing_PerModel,
        .render_scale = kSimBackgroundVoxelRenderScale_PixelClean,
      };
    case kSimBackgroundVoxelPreset_Quality:
      return (SimBackgroundVoxelPresetConfig){
        .enabled = true,
        .detail = kSimBackgroundVoxelDetail_Ultra,
        .lod = kSimBackgroundVoxelLod_Fixed,
        .shading = kSimBackgroundVoxelShading_MaterialAware,
        .style = kSimBackgroundVoxelStyle_Varied,
        .facing = kSimBackgroundVoxelFacing_PerModel,
        .render_scale = kSimBackgroundVoxelRenderScale_2x,
      };
    case kSimBackgroundVoxelPreset_Custom:
      custom.enabled = true;
      return custom;
    case kSimBackgroundVoxelPreset_Count:
      break;
  }
  return SimBackgroundVoxelPreset_Resolve(
      kSimBackgroundVoxelPreset_Balanced, custom);
}
