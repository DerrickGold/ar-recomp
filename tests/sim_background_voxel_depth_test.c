#include "sim/sim_background_voxel_depth.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

int main(void) {
  CHECK(SimBackgroundVoxelDepth_SliceCount(
            kSimBackgroundVoxelDetail_Low) == 1);
  CHECK(SimBackgroundVoxelDepth_SliceCount(
            kSimBackgroundVoxelDetail_Balanced) == 4);
  CHECK(SimBackgroundVoxelDepth_SliceCount(
            kSimBackgroundVoxelDetail_High) == 8);
  CHECK(SimBackgroundVoxelDepth_SliceCount(
            kSimBackgroundVoxelDetail_Ultra) == 12);

  int memberships[7] = {0};
  const float samples[] = {-100.0f, 0.0f, 2.49f, 2.5f,
                           7.49f, 7.5f, 100.0f};
  for (int slice = 0; slice < 4; slice++) {
    float minimum, maximum;
    SimBackgroundVoxelDepth_SliceRange(
        0.0f, 10.0f, 4, (uint8_t)slice, &minimum, &maximum);
    for (int sample = 0; sample < 7; sample++)
      memberships[sample] += SimBackgroundVoxelDepth_Contains(
          samples[sample], minimum, maximum);
  }
  for (int sample = 0; sample < 7; sample++)
    CHECK(memberships[sample] == 1);

  if (failures) return 1;
  puts("sim background voxel depth checks passed");
  return 0;
}
