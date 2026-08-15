#include "sim/sim_background_voxel_preset.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  SimBackgroundVoxelPresetConfig custom = {
    .detail = kSimBackgroundVoxelDetail_Balanced,
    .lod = kSimBackgroundVoxelLod_Fixed,
    .shading = kSimBackgroundVoxelShading_MaterialAware,
    .style = kSimBackgroundVoxelStyle_Trim,
    .facing = kSimBackgroundVoxelFacing_Shared,
    .render_scale = kSimBackgroundVoxelRenderScale_2x,
  };
  SimBackgroundVoxelPresetConfig off = SimBackgroundVoxelPreset_Resolve(
      kSimBackgroundVoxelPreset_Off, custom);
  CHECK(!off.enabled);
  SimBackgroundVoxelPresetConfig performance =
      SimBackgroundVoxelPreset_Resolve(
          kSimBackgroundVoxelPreset_Performance, custom);
  CHECK(performance.enabled);
  CHECK(performance.detail == kSimBackgroundVoxelDetail_Low);
  CHECK(performance.render_scale == kSimBackgroundVoxelRenderScale_Native);
  SimBackgroundVoxelPresetConfig balanced =
      SimBackgroundVoxelPreset_Resolve(
          kSimBackgroundVoxelPreset_Balanced, custom);
  CHECK(balanced.detail == kSimBackgroundVoxelDetail_High);
  CHECK(balanced.lod == kSimBackgroundVoxelLod_Adaptive);
  CHECK(balanced.render_scale ==
        kSimBackgroundVoxelRenderScale_PixelClean);
  SimBackgroundVoxelPresetConfig quality =
      SimBackgroundVoxelPreset_Resolve(
          kSimBackgroundVoxelPreset_Quality, custom);
  CHECK(quality.detail == kSimBackgroundVoxelDetail_Ultra);
  CHECK(quality.lod == kSimBackgroundVoxelLod_Fixed);
  CHECK(quality.render_scale == kSimBackgroundVoxelRenderScale_2x);
  SimBackgroundVoxelPresetConfig resolved_custom =
      SimBackgroundVoxelPreset_Resolve(
          kSimBackgroundVoxelPreset_Custom, custom);
  CHECK(resolved_custom.enabled);
  custom.enabled = true;
  CHECK(memcmp(&resolved_custom, &custom, sizeof(custom)) == 0);

  if (failures) return 1;
  puts("sim background voxel preset checks passed");
  return 0;
}
