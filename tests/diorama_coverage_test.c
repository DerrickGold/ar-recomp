#include "diorama/diorama_coverage.h"
#include "diorama/diorama_planes.h"

#include <assert.h>
#include <stdio.h>

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
  TestAlphaCellAndDilation();
  TestIndexCompaction();
  TestSparsePlanePolicy();
  puts("diorama coverage tests: pass");
  return 0;
}
