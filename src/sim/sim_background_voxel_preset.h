#ifndef SIM_BACKGROUND_VOXEL_PRESET_H
#define SIM_BACKGROUND_VOXEL_PRESET_H

#include <stdbool.h>

#include "sim_background_voxel_quality.h"

typedef struct SimBackgroundVoxelPresetConfig {
  bool enabled;
  SimBackgroundVoxelDetail detail;
  SimBackgroundVoxelLod lod;
  SimBackgroundVoxelShading shading;
  SimBackgroundVoxelStyle style;
  SimBackgroundVoxelFacing facing;
  SimBackgroundVoxelRenderScale render_scale;
} SimBackgroundVoxelPresetConfig;

/* Resolves one immutable frame configuration. Presets never overwrite the
 * stored Custom controls, so switching away and back restores the exact
 * player-authored combination. */
SimBackgroundVoxelPresetConfig SimBackgroundVoxelPreset_Resolve(
    SimBackgroundVoxelPreset preset,
    SimBackgroundVoxelPresetConfig custom);

#endif  /* SIM_BACKGROUND_VOXEL_PRESET_H */
