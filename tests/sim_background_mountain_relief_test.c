#include "sim/sim_background_mountain_relief.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  SimBackgroundMountainRelief low, high, ultra;
  SimBackgroundMountainRelief_Resolve(
      kSimBackgroundVoxelDetail_Low,
      kSimBackgroundVoxelShading_Basic, &low);
  SimBackgroundMountainRelief_Resolve(
      kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelShading_AmbientOcclusion, &high);
  SimBackgroundMountainRelief_Resolve(
      kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelShading_MaterialAware, &ultra);

  CHECK(low.side_band_count == 1);
  CHECK(high.side_band_count == 3);
  CHECK(ultra.side_band_count ==
        kSimBackgroundMountainReliefMaxSideBands);
  CHECK(low.depth_pixels == 1.0f);
  CHECK(high.depth_pixels == 2.25f);
  CHECK(ultra.depth_pixels == 3.0f);
  CHECK(low.face_height_scale == high.face_height_scale);
  CHECK(high.face_height_scale == ultra.face_height_scale);
  CHECK(low.face_depth_scale == ultra.face_depth_scale);
  CHECK(low.side_alpha < high.side_alpha);
  CHECK(high.side_alpha < ultra.side_alpha);
  CHECK(high.side_brightness[0] < high.side_brightness[1]);
  CHECK(ultra.side_brightness[0] < high.side_brightness[0]);
  for (uint8_t band = 1; band < ultra.side_band_count; band++)
    CHECK(ultra.side_brightness[band] >=
          ultra.side_brightness[band - 1]);

  if (failures) return 1;
  puts("sim background mountain relief checks passed");
  return 0;
}
