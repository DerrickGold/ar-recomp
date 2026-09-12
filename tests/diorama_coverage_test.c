#include "diorama/diorama_coverage.h"
#include "diorama/diorama_planes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static DioramaCoverageMask ReferenceCoverage(const uint8_t *pixels,
    size_t pitch, int width, int height) {
  DioramaCoverageMask occupied = 0;
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) {
      uint32_t pixel;
      memcpy(&pixel, pixels + (size_t)y * pitch + (size_t)x * 4, 4);
      if (pixel >> 24) occupied |= UINT64_C(1) <<
          ((y * kDioramaCoverageRows / height) * kDioramaCoverageColumns +
           x * kDioramaCoverageColumns / width);
    }
  return DioramaCoverage_Dilate(occupied);
}

static void TestCoverageOracle(void) {
  uint32_t random = 127;
  for (int trial = 0; trial < 3000; trial++) {
    const int width = 1 + trial % 97, height = 1 + trial % 83;
    const size_t pitch = (size_t)width * 4 + trial % 13;
    uint8_t *allocation = malloc(pitch * height + 4);
    assert(allocation);
    uint8_t *pixels = allocation + trial % 4;
    memset(pixels, 0xff, pitch * height); /* Padding must never be sampled. */
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++) {
        random = random * 1664525u + 1013904223u;
        const bool alpha = trial % 5 == 0 ||
            (trial % 5 == 1 && random % 101 == 0) ||
            (trial % 5 == 2 && random % 7 == 0) ||
            (trial % 5 == 3 && x == width - 1 && y == height - 1);
        const uint32_t pixel = (random & 0xffffffu) | (alpha ? 0x01000000u : 0u);
        memcpy(pixels + (size_t)y * pitch + (size_t)x * 4, &pixel, 4);
      }
    assert(DioramaCoverage_FromArgb8888(pixels, pitch, width, height) ==
           ReferenceCoverage(pixels, pitch, width, height));
    free(allocation);
  }
  const uint8_t pixel[4] = {0};
  assert(!DioramaCoverage_FromArgb8888(NULL, 4, 1, 1));
  assert(!DioramaCoverage_FromArgb8888(pixel, 0, 1, 1));
  assert(!DioramaCoverage_FromArgb8888(pixel, 3, 1, 1));
  assert(!DioramaCoverage_FromArgb8888(pixel, 4, 0, 1));
  assert(!DioramaCoverage_FromArgb8888(pixel, SIZE_MAX, 1, 2));
}

static void TestAlphaCellAndDilation(void) {
  uint32_t pixels[12][18] = {{0}};
  /* Active width is 16; the two tail words prove pitch is honored. This lands
   * in grid cell column 3, row 2, so dilation produces a 3x3 neighborhood. */
  pixels[5][7] = UINT32_C(0x01000000);
  pixels[5][16] = UINT32_C(0xff000000);
  const DioramaCoverageMask mask = DioramaCoverage_FromArgb8888(
      (const uint8_t *)pixels, sizeof(pixels[0]), 16, 12);
  DioramaCoverageMask expected = 0;
  for (int row = 1; row <= 3; row++)
    for (int column = 2; column <= 4; column++)
      expected |= UINT64_C(1) <<
          (row * kDioramaCoverageColumns + column);
  assert(mask == expected);
}

static void TestIndexCompaction(void) {
  int32_t indices[kDioramaCoverageCellCount *
                  kDioramaCoverageIndicesPerCell];
  for (int i = 0; i < (int)(sizeof(indices) / sizeof(indices[0])); i++)
    indices[i] = i;
  const int cell = 17;
  const int count = DioramaCoverage_FilterGridIndices(
      indices, (int)(sizeof(indices) / sizeof(indices[0])),
      UINT64_C(1) << cell);
  assert(count == kDioramaCoverageIndicesPerCell);
  for (int i = 0; i < count; i++)
    assert(indices[i] ==
           cell * kDioramaCoverageIndicesPerCell + i);
}

static void TestSparsePlanePolicy(void) {
  assert(!DioramaPlaneUsesSparseCoverage(SR_PPU_OVERLAY_BG1));
  assert(!DioramaPlaneUsesSparseCoverage(SR_PPU_OVERLAY_BG2));
  assert(!DioramaPlaneUsesSparseCoverage(SR_PPU_OVERLAY_BG3));
  assert(!DioramaPlaneUsesSparseCoverage(kDioramaPlane_Backdrop));
  assert(DioramaPlaneUsesSparseCoverage(kDioramaPlane_Bg1Hi));
  assert(DioramaPlaneUsesSparseCoverage(kDioramaPlane_Bg2Hi));
  assert(DioramaPlaneUsesSparseCoverage(kDioramaPlane_Bg1Far));
  assert(DioramaPlaneUsesSparseCoverage(kDioramaPlane_Bg2Far));
  assert(DioramaPlaneUsesSparseCoverage(SR_PPU_OVERLAY_OBJ));
  assert(DioramaPlaneUsesSparseCoverage(kDioramaPlane_Obj3));
}

int main(void) {
  TestCoverageOracle();
  TestAlphaCellAndDilation();
  TestIndexCompaction();
  TestSparsePlanePolicy();
  puts("diorama coverage tests: pass");
  return 0;
}
