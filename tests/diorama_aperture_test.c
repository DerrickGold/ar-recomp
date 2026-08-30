#include "diorama/diorama_aperture.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool Near(float left, float right) {
  return fabsf(left - right) < 0.0001f;
}

static void BuildGrid(ArRenderVertex2D *grid, int subdiv_x, int subdiv_y,
                      float offset_x, float offset_y) {
  for (int row = 0; row <= subdiv_y; row++) {
    for (int column = 0; column <= subdiv_x; column++) {
      const int index = row * (subdiv_x + 1) + column;
      grid[index] = (ArRenderVertex2D){
        .position = {
          offset_x + (float)column * 10.0f,
          offset_y + (float)row * 10.0f,
        },
        .color = {0.25f, 0.5f, 0.75f, 0.8f},
        .tex_coord = {
          (float)column / (float)subdiv_x,
          (float)row / (float)subdiv_y,
        },
      };
    }
  }
}

static void TestPinsBoundaryAndBlendsInteriorRing(void) {
  enum { kSubdivX = 4, kSubdivY = 4, kCount = 25 };
  ArRenderVertex2D grid[kCount];
  ArRenderVertex2D aperture[kCount];
  BuildGrid(grid, kSubdivX, kSubdivY, 8.0f, -6.0f);
  BuildGrid(aperture, kSubdivX, kSubdivY, 0.0f, 0.0f);
  assert(DioramaAperture_ConstrainGrid(
      grid, aperture, kSubdivX, kSubdivY, 2.0f));

  /* Every outer vertex lands exactly on the shared scene aperture. */
  assert(Near(grid[0].position.x, aperture[0].position.x));
  assert(Near(grid[0].position.y, aperture[0].position.y));
  assert(Near(grid[4].position.x, aperture[4].position.x));
  assert(Near(grid[20].position.y, aperture[20].position.y));

  /* The first interior ring receives half the correction. */
  const int ring = 1 * (kSubdivX + 1) + 1;
  assert(Near(grid[ring].position.x, aperture[ring].position.x + 4.0f));
  assert(Near(grid[ring].position.y, aperture[ring].position.y - 3.0f));

  /* The centre retains the foreground plane's independent parallax. */
  const int centre = 2 * (kSubdivX + 1) + 2;
  assert(Near(grid[centre].position.x, aperture[centre].position.x + 8.0f));
  assert(Near(grid[centre].position.y, aperture[centre].position.y - 6.0f));
  assert(Near(grid[centre].tex_coord.x, 0.5f));
  assert(Near(grid[centre].color.a, 0.8f));
}

static void TestRejectsInvalidInputWithoutMutation(void) {
  ArRenderVertex2D grid[4];
  ArRenderVertex2D aperture[4];
  BuildGrid(grid, 1, 1, 2.0f, 3.0f);
  BuildGrid(aperture, 1, 1, 0.0f, 0.0f);
  aperture[3].position.x = NAN;
  const ArRenderPointF before = grid[0].position;
  assert(!DioramaAperture_ConstrainGrid(grid, aperture, 1, 1, 1.0f));
  assert(Near(grid[0].position.x, before.x));
  assert(Near(grid[0].position.y, before.y));
}

int main(void) {
  TestPinsBoundaryAndBlendsInteriorRing();
  TestRejectsInvalidInputWithoutMutation();
  puts("diorama aperture tests: pass");
  return 0;
}
