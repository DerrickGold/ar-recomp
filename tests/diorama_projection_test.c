#include <math.h>
#include <stdio.h>
#include <string.h>

#include "diorama.h"

static int g_failures;

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
            #condition);                                                     \
    g_failures++;                                                            \
  }                                                                          \
} while (0)

static bool Near(float actual, float expected) {
  return fabsf(actual - expected) < 0.001f;
}

static DioramaProjection Projection(void) {
  DioramaProjection projection = {
    .valid = true,
    .matrix = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
    },
    .object_u0 = 0.0f,
    .object_v0 = 0.0f,
    .object_u1 = 1.0f,
    .object_v1 = 1.0f,
    .aspect_x = 2.0f,
    .height_scale = 1.0f,
    .texture_width = 100,
    .texture_height = 50,
    .output_width = 100,
    .output_height = 100,
  };
  projection.object_planes[0].valid = true;
  return projection;
}

static void TestRegisteredProjectionAndScale(void) {
  DioramaProjection projection = Projection();
  SDL_FPoint point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 50.0f));
  CHECK(Near(point.y, 50.0f));
  CHECK(Near(scale_x, 1.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestPriorityPlaneShapeIsApplied(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f;  /* make depth visible in screen X */
  projection.object_planes[0].rake = 0.5f;
  projection.object_planes[1].valid = true;
  projection.object_planes[1].rake = 0.0f;
  SDL_FPoint raked, flat;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 50.0f, 0, &raked, NULL, NULL));
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 50.0f, 1, &flat, NULL, NULL));
  CHECK(Near(raked.x - flat.x, 25.0f));
  CHECK(Near(raked.y, flat.y));

  projection.object_planes[0].rake = 0.0f;
  projection.object_planes[0].bow = 0.25f;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 50.0f, 0, &raked, NULL, NULL));
  CHECK(Near(raked.x - flat.x, 12.5f));
}

static void TestOutputViewportOriginIsApplied(void) {
  DioramaProjection projection = Projection();
  projection.output_x = 120;
  projection.output_y = 40;
  SDL_FPoint point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 170.0f));
  CHECK(Near(point.y, 90.0f));
  CHECK(Near(scale_x, 1.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestInvalidInputsFailClosed(void) {
  DioramaProjection projection = Projection();
  SDL_FPoint point = { 17.0f, 29.0f };
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 1, &point, NULL, NULL));
  CHECK(point.x == 17.0f && point.y == 29.0f);
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 4, &point, NULL, NULL));
  projection.valid = false;
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 0, &point, NULL, NULL));
  CHECK(!Diorama_ProjectCapturedPoint(
      NULL, 0.0f, 0.0f, 0, &point, NULL, NULL));
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 0, NULL, NULL, NULL));
}

int main(void) {
  TestRegisteredProjectionAndScale();
  TestPriorityPlaneShapeIsApplied();
  TestOutputViewportOriginIsApplied();
  TestInvalidInputsFailClosed();
  if (g_failures) {
    fprintf(stderr, "%d diorama projection test(s) failed\n", g_failures);
    return 1;
  }
  puts("diorama projection: all tests passed");
  return 0;
}
