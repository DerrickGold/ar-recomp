#include "diorama_depth_shapes.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);       \
      failures++;                                                             \
    }                                                                         \
  } while (0)

static bool Near(float a, float b) {
  return fabsf(a - b) < 1.0e-4f;
}

static void TestSkirt(void) {
  const float base_z = 0.50f;
  const float base_y = -0.50f;
  const float thickness = 0.20f;
  float y, z, shade;

  DioramaSkirtVertex(0.0f, base_z, base_y, thickness, &y, &z, &shade);
  CHECK(Near(y, base_y));
  CHECK(Near(z, base_z));
  CHECK(Near(shade, 1.0f));

  DioramaSkirtVertex(1.0f, base_z, base_y, thickness, &y, &z, &shade);
  CHECK(Near(y, base_y - thickness * 0.5f));
  CHECK(Near(z, base_z + thickness));
  CHECK(Near(shade, DioramaSkirtNearShade()));
  CHECK(shade > 0.0f && shade < 1.0f);

  float previous_y = base_y + 1.0f;
  float previous_z = base_z - 1.0f;
  float previous_shade = 2.0f;
  for (int sample = 0; sample <= 10; sample++) {
    DioramaSkirtVertex((float)sample / 10.0f, base_z, base_y, thickness,
                       &y, &z, &shade);
    CHECK(y <= previous_y);
    CHECK(z >= previous_z);
    CHECK(shade <= previous_shade);
    previous_y = y;
    previous_z = z;
    previous_shade = shade;
  }

  const float rake = 0.29f;
  DioramaSkirtVertex(0.0f, base_z + rake, base_y, thickness,
                     &y, &z, &shade);
  CHECK(Near(z, base_z + rake));
  DioramaSkirtVertex(1.0f, base_z + rake, base_y, thickness,
                     &y, &z, &shade);
  CHECK(Near(z, base_z + rake + thickness));
}

static void TestStackAndVoxel(void) {
  enum { kForward, kBackward, kBoth };
  const float base = 0.21f;
  const float depth = 0.29f;
  float z, shade, alpha;

  DioramaStackCopyShaped(0, 4, base, depth, kForward, false,
                         &z, &shade, &alpha);
  CHECK(Near(z, base));
  CHECK(Near(shade, 1.0f));
  CHECK(Near(alpha, 1.0f));
  DioramaStackCopyShaped(3, 4, base, depth, kForward, false,
                         &z, &shade, &alpha);
  CHECK(Near(z, base + depth));
  CHECK(shade < 1.0f && alpha > 0.0f && alpha < 1.0f);

  float previous_z = base - 1.0f;
  float previous_alpha = 2.0f;
  for (int copy = 0; copy < 4; copy++) {
    DioramaStackCopyShaped(copy, 4, base, depth, kForward, false,
                           &z, &shade, &alpha);
    CHECK(Near(z, base + depth * (float)copy / 3.0f));
    CHECK(z >= previous_z);
    CHECK(alpha <= previous_alpha);
    previous_z = z;
    previous_alpha = alpha;
  }

  DioramaStackCopyShaped(3, 4, base, depth, kBackward, false,
                         &z, &shade, &alpha);
  CHECK(Near(z, base - depth));
  float far_z, far_shade, near_z, near_shade, midpoint_shade;
  DioramaStackCopyShaped(0, 5, base, depth, kBoth, false,
                         &far_z, &far_shade, &alpha);
  DioramaStackCopyShaped(2, 5, base, depth, kBoth, false,
                         &z, &midpoint_shade, &alpha);
  DioramaStackCopyShaped(4, 5, base, depth, kBoth, false,
                         &near_z, &near_shade, &alpha);
  CHECK(Near(far_z, base - depth * 0.5f));
  CHECK(Near(near_z, base + depth * 0.5f));
  CHECK(Near(near_z - far_z, depth));
  CHECK(Near(far_shade, near_shade));
  CHECK(Near(midpoint_shade, 1.0f));
  CHECK(midpoint_shade > far_shade);

  CHECK(DioramaStackCopyIsRedundant(0, 4, kForward));
  CHECK(DioramaStackCopyIsRedundant(0, 4, kBackward));
  CHECK(DioramaStackCopyIsRedundant(2, 5, kBoth));
  for (int copy = 0; copy < 4; copy++)
    CHECK(!DioramaStackCopyIsRedundant(copy, 4, kBoth));

  float stack_z, stack_shade, stack_alpha;
  float voxel_z, voxel_shade, voxel_alpha;
  for (int copy = 0; copy < 6; copy++) {
    DioramaStackCopyShaped(copy, 6, base, depth, kForward, false,
                           &stack_z, &stack_shade, &stack_alpha);
    DioramaStackCopyShaped(copy, 6, base, depth, kForward, true,
                           &voxel_z, &voxel_shade, &voxel_alpha);
    CHECK(Near(stack_z, voxel_z));
    CHECK(Near(voxel_alpha, 1.0f));
    if (copy == 0) {
      CHECK(voxel_shade > 0.5f && voxel_shade < 1.0f);
    } else {
      float first_z, first_shade, first_alpha;
      DioramaStackCopyShaped(0, 6, base, depth, kForward, true,
                             &first_z, &first_shade, &first_alpha);
      CHECK(Near(voxel_shade, first_shade));
      CHECK(Near(voxel_alpha, first_alpha));
    }
  }
  CHECK(voxel_shade > stack_shade);
}

static void TestVerticalRepeat(void) {
  DioramaVerticalRepeatPlan plan;
  CHECK(DioramaVerticalRepeatPlan_Build(32, 224, 288, 352, &plan));
  CHECK(plan.source_y0 == 32 && plan.source_y1 == 256);
  CHECK(plan.fold_y == 256 && plan.repeat_height == 224);

  DioramaVerticalRepeatPlan same;
  CHECK(DioramaVerticalRepeatPlan_Build(32, 224, 320, 352, &same));
  CHECK(same.source_y0 == plan.source_y0 && same.source_y1 == plan.source_y1);
  CHECK(same.fold_y == plan.fold_y && same.repeat_height == plan.repeat_height);
  CHECK(DioramaVerticalRepeatPlan_Build(0, 224, 224, 352, &same));
  CHECK(same.source_y0 == 0 && same.source_y1 == 224 && same.fold_y == 224);
  CHECK(!DioramaVerticalRepeatPlan_Build(32, 224, 240, 352, &same));
  CHECK(same.source_y0 == 0 && same.source_y1 == 0 && same.fold_y == 0);
}

static void TestOverflowFold(void) {
  const float y_top = -0.50f;
  const float z_top = -0.30f;
  const float z_handoff = -0.25f;
  const float height = 1.0f;
  const float overlap = 0.25f;
  const float front_z = 0.45f;
  const float front_drop = 0.18f;
  float y, z;

  DioramaOverflowFoldPoint(0.0f, y_top, z_top, z_handoff, height, overlap,
                           front_z, front_drop, &y, &z);
  CHECK(Near(y, y_top) && Near(z, z_top));
  DioramaOverflowFoldPoint(overlap, y_top, z_top, z_handoff, height, overlap,
                           front_z, front_drop, &y, &z);
  CHECK(Near(y, y_top - height * overlap) && Near(z, z_handoff));
  DioramaOverflowFoldPoint(0.625f, y_top, z_top, z_handoff, height, overlap,
                           front_z, front_drop, &y, &z);
  CHECK(Near(y, -0.9825f) && Near(z, 0.10f));
  DioramaOverflowFoldPoint(1.0f, y_top, z_top, z_handoff, height, overlap,
                           front_z, front_drop, &y, &z);
  CHECK(Near(y, y_top - height * overlap - front_drop));
  CHECK(Near(z, front_z));

  float before_y, before_z, after_y, after_z;
  DioramaOverflowFoldPoint(overlap - 0.0001f, y_top, z_top, z_handoff,
                           height, overlap, front_z, front_drop,
                           &before_y, &before_z);
  DioramaOverflowFoldPoint(overlap + 0.0001f, y_top, z_top, z_handoff,
                           height, overlap, front_z, front_drop,
                           &after_y, &after_z);
  CHECK(fabsf(after_y - before_y) < 0.001f);
  CHECK(fabsf(after_z - before_z) < 0.001f);

  const float aitos_underlap = (32.0f + 16.0f) / 224.0f;
  CHECK(Near(DioramaOverflowFoldRowT(0, 7, aitos_underlap), 0.0f));
  CHECK(Near(DioramaOverflowFoldRowT(1, 7, aitos_underlap), aitos_underlap));
  CHECK(Near(DioramaOverflowFoldRowT(7, 7, aitos_underlap), 1.0f));
  float previous = -1.0f;
  for (int row = 0; row <= 7; row++) {
    const float current = DioramaOverflowFoldRowT(row, 7, aitos_underlap);
    CHECK(current > previous);
    previous = current;
  }
}

static void TestTilt(void) {
  const float base = 0.21f;
  const float amount = 0.29f;
  for (int sample = 0; sample <= 4; sample++) {
    const float t = (float)sample / 4.0f;
    CHECK(DioramaTiltedRowDepth(base, 0.0f, 0.0f, t) == base);
  }
  CHECK(Near(DioramaTiltedRowDepth(base, amount, 0.0f, 0.0f), base));
  CHECK(Near(DioramaTiltedRowDepth(base, 0.0f, amount, 0.0f), base));
  CHECK(Near(DioramaTiltedRowDepth(base, amount, 0.0f, 1.0f), base + amount));
  CHECK(Near(DioramaTiltedRowDepth(base, 0.0f, amount, 1.0f), base + amount));
  CHECK(Near(DioramaTiltedRowDepth(base, amount, 0.0f, 0.5f),
             base + amount * 0.5f));
  CHECK(Near(DioramaTiltedRowDepth(base, 0.0f, amount, 0.5f),
             base + amount * 0.25f));

  float previous_rake = base - 1.0f;
  float previous_bow = base - 1.0f;
  for (int sample = 0; sample <= 8; sample++) {
    const float t = (float)sample / 8.0f;
    const float rake = DioramaTiltedRowDepth(base, amount, 0.0f, t);
    const float bow = DioramaTiltedRowDepth(base, 0.0f, amount, t);
    CHECK(rake >= previous_rake && bow >= previous_bow);
    if (sample > 0 && sample < 8) CHECK(bow < rake);
    previous_rake = rake;
    previous_bow = bow;
  }
  CHECK(Near(DioramaTiltedRowDepth(base, 0.10f, 0.20f, 0.5f),
             base + 0.10f * 0.5f + 0.20f * 0.25f));
  CHECK(Near(DioramaTiltedRowDepth(base, amount, 0.0f, -1.0f), base));
  CHECK(Near(DioramaTiltedRowDepth(base, amount, 0.0f, 2.0f), base + amount));
}

int main(void) {
  TestSkirt();
  TestStackAndVoxel();
  TestVerticalRepeat();
  TestOverflowFold();
  TestTilt();
  if (failures != 0) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("diorama_depth_shapes_test: PASS");
  return 0;
}
