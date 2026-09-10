#include "render/text_cell_composite.h"

#include <stdio.h>

static int failures;
#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++failures;                                                            \
  }                                                                        \
} while (0)

static int Area(ArRenderRectI rectangle) {
  return rectangle.w * rectangle.h;
}

int main(void) {
  ArRenderRectI projected[kArTextCellMaximumProjectedRegions];
  size_t count = ArTextCellComposite_ProjectRegion(
      (ArTextCellRegion){5, 19, 23, 7}, 32, 32, 0, 0,
      256, 224, projected);
  CHECK(count == 1);
  CHECK(projected[0].x == 40 && projected[0].y == 151);
  CHECK(projected[0].w == 184 && projected[0].h == 56);

  count = ArTextCellComposite_ProjectRegion(
      (ArTextCellRegion){30, 31, 2, 1}, 32, 32, 248, 248,
      256, 224, projected);
  CHECK(count == 2);
  CHECK(projected[0].x == 0 && projected[0].w == 8);
  CHECK(projected[0].y == 0 && projected[0].h == 7);
  CHECK(projected[1].x == 248 && projected[1].w == 8);

  const HudPresentationChunk chunk = {
      .texture_source = {100, 20, 256, 200},
      .screen_source = {0, 0, 256, 200},
      .output_destination = {10, 30, 512, 400},
  };
  const ArRenderRectI mask = {40, 50, 80, 60};
  HudPresentationChunk pieces[kArTextCellMaximumChunkPieces];
  count = ArTextCellComposite_SubtractMasks(
      &chunk, &mask, 1, pieces, kArTextCellMaximumChunkPieces);
  CHECK(count == 4);
  int screen_area = 0;
  for (size_t index = 0; index < count; ++index)
    screen_area += Area(pieces[index].screen_source);
  CHECK(screen_area == Area(chunk.screen_source) - Area(mask));

  ArRenderRectI output;
  CHECK(ArTextCellComposite_ProjectToOutput(&chunk, mask, &output));
  CHECK(output.x == 90 && output.y == 130);
  CHECK(output.w == 160 && output.h == 120);

  /* A failed capacity request is explicit; callers retain the native chunk. */
  CHECK(ArTextCellComposite_SubtractMasks(&chunk, &mask, 1, pieces, 2) ==
        SIZE_MAX);
  puts("text cell composite checks passed");
  return failures ? 1 : 0;
}
