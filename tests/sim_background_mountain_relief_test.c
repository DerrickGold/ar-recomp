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
  static const uint8_t expected_layers[] = {1, 2, 3, 5};
  static const float expected_depth[] = {0.0f, 1.5f, 2.5f, 4.0f};
  for (int detail = kSimBackgroundVoxelDetail_Low;
       detail <= kSimBackgroundVoxelDetail_Ultra; detail++) {
    SimBackgroundMountainRelief relief = {0};
    SimBackgroundMountainRelief_Resolve(
        (SimBackgroundVoxelDetail)detail, &relief);
    CHECK(relief.stack_layer_count == expected_layers[detail]);
    CHECK(relief.stack_depth_pixels == expected_depth[detail]);
    CHECK(relief.face_height_scale == 0.30f);
    CHECK(relief.face_depth_scale == 0.62f);
    CHECK(SimBackgroundMountainRelief_StackOffsetY(
              &relief, 0, 0.0f, 64.0f) == 0.0f);
    CHECK(SimBackgroundMountainRelief_StackOffsetY(
              &relief, relief.stack_layer_count - 1,
              64.0f, 64.0f) == 0.0f);
    CHECK(SimBackgroundMountainRelief_StackOffsetY(
              &relief, relief.stack_layer_count - 1,
              0.0f, 64.0f) == 0.0f);
    CHECK(SimBackgroundMountainRelief_StackOffsetY(
              &relief, relief.stack_layer_count - 1,
              32.0f, 64.0f) == -expected_depth[detail]);
  }

  if (failures) return 1;
  puts("sim background mountain relief checks passed");
  return 0;
}
