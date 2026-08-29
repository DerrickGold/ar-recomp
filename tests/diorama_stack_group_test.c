#include "diorama/diorama_stack_group.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool Near(double left, double right) {
  return fabs(left - right) < 0.001;
}

static void TestIndexedBoundsIgnoreUnusedVertices(void) {
  const ArRenderVertex2D vertices[] = {
    {{10.0f, 20.0f}, {0}, {0}},
    {{40.0f, 20.0f}, {0}, {0}},
    {{40.0f, 60.0f}, {0}, {0}},
    {{10.0f, 60.0f}, {0}, {0}},
    {{900.0f, 900.0f}, {0}, {0}},
  };
  const int32_t indices[] = {0, 1, 2, 0, 2, 3};
  DioramaStackGroupBounds bounds;
  assert(DioramaStackGroupBounds_FromGeometry(
      vertices, 5, indices, 6, &bounds));
  assert(bounds.x0 == 10.0f && bounds.y0 == 20.0f);
  assert(bounds.x1 == 40.0f && bounds.y1 == 60.0f);
}

static void TestFullscreenStackGroupsAtHighResolution(void) {
  DioramaStackGroupBounds copies[4];
  for (int i = 0; i < 4; i++)
    copies[i] = (DioramaStackGroupBounds){-20.0f, -10.0f, 1020.0f, 610.0f};
  const DioramaStackGroupPlan plan = DioramaStackGroupPlan_Build(
      1000, 600, 400, 224, copies, 4);
  assert(plan.use_intermediate);
  assert(plan.target_width == 500 && plan.target_height == 300);
  assert(plan.output_bounds.x == 0 && plan.output_bounds.y == 0);
  assert(plan.output_bounds.w == 1000 && plan.output_bounds.h == 600);
  assert(Near(plan.direct_pixels, 2400000.0));
  assert(Near(plan.grouped_pixels, 1350000.0));
}

static void TestSmallOutputKeepsDirectPath(void) {
  const DioramaStackGroupBounds copies[] = {
    {0.0f, 0.0f, 500.0f, 300.0f},
    {0.0f, 0.0f, 500.0f, 300.0f},
  };
  const DioramaStackGroupPlan plan = DioramaStackGroupPlan_Build(
      500, 300, 400, 224, copies, 2);
  assert(!plan.use_intermediate);
}

static void TestSmallProjectedStackIsNotGrouped(void) {
  const DioramaStackGroupBounds copies[] = {
    {100.0f, 100.0f, 200.0f, 150.0f},
    {102.0f, 101.0f, 202.0f, 151.0f},
    {104.0f, 102.0f, 204.0f, 152.0f},
    {106.0f, 103.0f, 206.0f, 153.0f},
  };
  const DioramaStackGroupPlan plan = DioramaStackGroupPlan_Build(
      1920, 1080, 400, 224, copies, 4);
  assert(!plan.use_intermediate);
  assert(plan.grouped_pixels > plan.direct_pixels);
}

int main(void) {
  TestIndexedBoundsIgnoreUnusedVertices();
  TestFullscreenStackGroupsAtHighResolution();
  TestSmallOutputKeepsDirectPath();
  TestSmallProjectedStackIsNotGrouped();
  puts("diorama stack-group tests: pass");
  return 0;
}
