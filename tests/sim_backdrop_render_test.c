#include <math.h>
#include <stdio.h>

#include "sim/sim_backdrop_render.h"

static int s_failures;
#define CHECK(expression)                                                  \
  do {                                                                     \
    if (!(expression)) {                                                   \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
              #expression);                                                \
      s_failures++;                                                        \
    }                                                                      \
  } while (0)

static bool Near(float left, float right) {
  return fabsf(left - right) < 0.00001f;
}

static void CheckColor(ArRenderColorF actual,
                       float r, float g, float b, float a) {
  CHECK(Near(actual.r, r));
  CHECK(Near(actual.g, g));
  CHECK(Near(actual.b, b));
  CHECK(Near(actual.a, a));
}

static void TestInvalidInputsFailClosed(void) {
  SimBackdropRenderBatch batch = {.vertex_count = 99, .index_count = 99};
  CHECK(!SimBackdropRender_Build(NULL, &batch));
  CHECK(batch.vertex_count == 0 && batch.index_count == 0);
  const SimBackdropRenderInput input = {
    .viewport = {0, 0, 0, 224},
  };
  CHECK(!SimBackdropRender_Build(&input, &batch));
  CHECK(batch.vertex_count == 0 && batch.index_count == 0);
  CHECK(!SimBackdropRender_Build(&input, NULL));
}

static void TestZeroStrengthIsExactFlatBackdrop(void) {
  const SimBackdropRenderInput input = {
    .backdrop_argb = UINT32_C(0xFF204080),
    .strength_pct = 0,
    .horizon_pct = 0,
    .viewport = {10, 20, 300, 200},
  };
  SimBackdropRenderBatch batch;
  CHECK(SimBackdropRender_Build(&input, &batch));
  CHECK(batch.vertex_count == 4);
  CHECK(batch.index_count == 6);
  for (int i = 0; i < batch.vertex_count; i++)
    CheckColor(batch.vertices[i].color,
               32.0f / 255.0f, 64.0f / 255.0f, 128.0f / 255.0f, 1.0f);
  CHECK(Near(batch.vertices[0].position.x, 10.0f));
  CHECK(Near(batch.vertices[1].position.x, 310.0f));
  CHECK(Near(batch.vertices[0].position.y, 20.0f));
  CHECK(Near(batch.vertices[2].position.y, 220.0f));
}

static void TestSyntheticHorizonBuildsTwoQuads(void) {
  const SimBackdropRenderInput input = {
    .backdrop_argb = UINT32_C(0xFF000000),
    .strength_pct = 100,
    .horizon_pct = 60,
    .viewport = {10, 20, 300, 200},
  };
  SimBackdropRenderBatch batch;
  CHECK(SimBackdropRender_Build(&input, &batch));
  CHECK(batch.vertex_count == 6);
  CHECK(batch.index_count == 12);
  CHECK(Near(batch.vertices[0].position.y, 20.0f));
  CHECK(Near(batch.vertices[2].position.y, 140.0f));
  CHECK(Near(batch.vertices[4].position.y, 220.0f));
  CheckColor(batch.vertices[0].color,
             0.16f * 0.62f, 0.33f * 0.62f, 0.66f * 0.62f, 1.0f);
  CheckColor(batch.vertices[2].color,
             0.60f * 0.82f, 0.74f * 0.82f, 0.90f * 0.82f, 1.0f);
  CheckColor(batch.vertices[4].color,
             0.60f * 0.82f, 0.74f * 0.82f, 0.90f * 0.82f, 1.0f);
  const int32_t expected[] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
  for (int i = 0; i < batch.index_count; i++)
    CHECK(batch.indices[i] == expected[i]);
}

static void TestVisibleMatrixHorizonOverridesAuthoredAnchor(void) {
  float matrix[16] = {0};
  matrix[7] = 1.0f;  /* ndc horizon 0 -> middle of the output */
  const SimBackdropRenderInput input = {
    .backdrop_argb = UINT32_C(0xFF000000),
    .strength_pct = 100,
    .horizon_pct = 20,
    .viewport = {5, 30, 320, 180},
    .matrix = matrix,
  };
  SimBackdropRenderBatch batch;
  CHECK(SimBackdropRender_Build(&input, &batch));
  CHECK(batch.vertex_count == 6);
  CHECK(Near(batch.vertices[2].position.y, 120.0f));
}

int main(void) {
  TestInvalidInputsFailClosed();
  TestZeroStrengthIsExactFlatBackdrop();
  TestSyntheticHorizonBuildsTwoQuads();
  TestVisibleMatrixHorizonOverridesAuthoredAnchor();
  if (s_failures) {
    fprintf(stderr, "sim backdrop render: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("sim backdrop render: all tests passed");
  return 0;
}
