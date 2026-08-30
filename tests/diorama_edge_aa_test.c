#include "diorama/diorama_edge_aa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do {                                            \
  if (!(condition)) {                                                    \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
            __FILE__, __LINE__, #condition);                             \
    exit(1);                                                             \
  }                                                                      \
} while (0)

static bool NearlyEqual(float a, float b) {
  return fabsf(a - b) < 0.0001f;
}

static void BuildGrid(ArRenderVertex2D *grid, int subdiv_x, int subdiv_y) {
  for (int row = 0; row <= subdiv_y; row++) {
    for (int column = 0; column <= subdiv_x; column++) {
      const int index = row * (subdiv_x + 1) + column;
      grid[index] = (ArRenderVertex2D){
        .position = {(float)column * 10.0f, (float)row * 10.0f},
        .color = {0.2f, 0.4f, 0.6f, 0.8f},
        .tex_coord = {
          (float)column / (float)subdiv_x,
          (float)row / (float)subdiv_y,
        },
      };
    }
  }
}

static void TestFullRectangle(void) {
  enum { kSubdivX = 2, kSubdivY = 2 };
  ArRenderVertex2D grid[(kSubdivX + 1) * (kSubdivY + 1)];
  ArRenderVertex2D fringe[16];
  int32_t indices[48];
  int vertex_count = -1, index_count = -1;
  BuildGrid(grid, kSubdivX, kSubdivY);
  CHECK(DioramaEdgeAa_FringeVertexCapacity(kSubdivX, kSubdivY) == 16);
  CHECK(DioramaEdgeAa_FringeIndexCapacity(kSubdivX, kSubdivY) == 48);
  CHECK(DioramaEdgeAa_BuildFringe(
      grid, kSubdivX, kSubdivY, 1.0f, kDioramaEdgeAa_All,
      0.0f, 1.0f, 0.0f, 1.0f, 0.1f, 0.2f,
      fringe, 16, indices, 48, &vertex_count, &index_count));
  CHECK(vertex_count == 16);
  CHECK(index_count == 48);

  /* Top-left inner/outer. The miter is a square one-pixel expansion. */
  CHECK(NearlyEqual(fringe[0].position.x, 0.0f));
  CHECK(NearlyEqual(fringe[0].position.y, 0.0f));
  CHECK(NearlyEqual(fringe[1].position.x, -1.0f));
  CHECK(NearlyEqual(fringe[1].position.y, -1.0f));
  CHECK(NearlyEqual(fringe[0].tex_coord.x, 0.05f));
  CHECK(NearlyEqual(fringe[0].tex_coord.y, 0.10f));
  CHECK(NearlyEqual(fringe[1].tex_coord.x, 0.05f));
  CHECK(NearlyEqual(fringe[1].tex_coord.y, 0.10f));
  CHECK(NearlyEqual(fringe[0].color.a, 0.8f));
  CHECK(NearlyEqual(fringe[1].color.a, 0.0f));

  /* Top-right and bottom-right corners follow clockwise in the boundary. */
  CHECK(NearlyEqual(fringe[5].position.x, 21.0f));
  CHECK(NearlyEqual(fringe[5].position.y, -1.0f));
  CHECK(NearlyEqual(fringe[9].position.x, 21.0f));
  CHECK(NearlyEqual(fringe[9].position.y, 21.0f));
}

static void TestDisabledBottom(void) {
  enum { kSubdivX = 2, kSubdivY = 2 };
  ArRenderVertex2D grid[(kSubdivX + 1) * (kSubdivY + 1)];
  ArRenderVertex2D fringe[16];
  int32_t indices[48];
  int vertex_count = 0, index_count = 0;
  BuildGrid(grid, kSubdivX, kSubdivY);
  CHECK(DioramaEdgeAa_BuildFringe(
      grid, kSubdivX, kSubdivY, 1.0f,
      kDioramaEdgeAa_All & ~kDioramaEdgeAa_Bottom,
      0.0f, 1.0f, 0.0f, 1.0f, 0.1f, 0.2f,
      fringe, 16, indices, 48, &vertex_count, &index_count));
  CHECK(vertex_count == 16);
  CHECK(index_count == 36);

  /* Bottom-right retains the right-side expansion but does not move down. */
  CHECK(NearlyEqual(fringe[9].position.x, 21.0f));
  CHECK(NearlyEqual(fringe[9].position.y, 20.0f));
  /* The bottom midpoint belongs only to disabled bottom segments. */
  CHECK(NearlyEqual(fringe[11].position.x, 10.0f));
  CHECK(NearlyEqual(fringe[11].position.y, 20.0f));
}

static void TestCapacityFailure(void) {
  ArRenderVertex2D grid[4];
  ArRenderVertex2D fringe[7];
  int32_t indices[24];
  int vertex_count = 99, index_count = 99;
  BuildGrid(grid, 1, 1);
  CHECK(!DioramaEdgeAa_BuildFringe(
      grid, 1, 1, 1.0f, kDioramaEdgeAa_All,
      0.0f, 1.0f, 0.0f, 1.0f, 0.1f, 0.1f,
      fringe, 7, indices, 24, &vertex_count, &index_count));
  CHECK(vertex_count == 0);
  CHECK(index_count == 0);
}

int main(void) {
  TestFullRectangle();
  TestDisabledBottom();
  TestCapacityFailure();
  puts("diorama edge AA tests: pass");
  return 0;
}
