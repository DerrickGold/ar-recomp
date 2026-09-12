#ifndef AR_DIORAMA_COVERAGE_H
#define AR_DIORAMA_COVERAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The compositor's ordinary plane mesh is an 8x6 grid. A captured object
 * plane is usually almost entirely transparent, so retaining only grid cells
 * near alpha-bearing texels prevents its main and shadow passes from shading
 * the complete projected rectangle. One-cell dilation is deliberately coarse:
 * it preserves filtering/rim samples across cell boundaries and still removes
 * most empty work for sprite bands. */
enum {
  kDioramaCoverageColumns = 8,
  kDioramaCoverageRows = 6,
  kDioramaCoverageCellCount =
      kDioramaCoverageColumns * kDioramaCoverageRows,
  kDioramaCoverageIndicesPerCell = 6,
};

typedef uint64_t DioramaCoverageMask;

static inline DioramaCoverageMask DioramaCoverage_FullMask(void) {
  return (UINT64_C(1) << kDioramaCoverageCellCount) - UINT64_C(1);
}

static inline DioramaCoverageMask DioramaCoverage_Dilate(
    DioramaCoverageMask occupied) {
  DioramaCoverageMask dilated = 0;
  for (int row = 0; row < kDioramaCoverageRows; row++) {
    for (int column = 0; column < kDioramaCoverageColumns; column++) {
      const int cell = row * kDioramaCoverageColumns + column;
      if (!(occupied & (UINT64_C(1) << cell))) continue;
      for (int dy = -1; dy <= 1; dy++) {
        const int neighbor_row = row + dy;
        if (neighbor_row < 0 || neighbor_row >= kDioramaCoverageRows)
          continue;
        for (int dx = -1; dx <= 1; dx++) {
          const int neighbor_column = column + dx;
          if (neighbor_column < 0 ||
              neighbor_column >= kDioramaCoverageColumns)
            continue;
          const int neighbor =
              neighbor_row * kDioramaCoverageColumns + neighbor_column;
          dilated |= UINT64_C(1) << neighbor;
        }
      }
    }
  }
  return dilated;
}

static inline DioramaCoverageMask DioramaCoverage_FromArgb8888(
    const uint8_t *pixels, size_t pitch_bytes, int width, int height) {
  if (!pixels || width <= 0 || height <= 0 ||
      (size_t)width > SIZE_MAX / sizeof(uint32_t) ||
      pitch_bytes < (size_t)width * sizeof(uint32_t) ||
      (size_t)height > SIZE_MAX / pitch_bytes) return 0;
  DioramaCoverageMask occupied = 0;
  /* Coverage asks whether ANY pixel in a cell has alpha. Stop at the first
   * one instead of dividing coordinates and setting the same bit for every
   * opaque texel. Ceil boundaries exactly invert floor(x * columns / width),
   * including uneven extents and images smaller than the grid. */
  for (int cell_row = 0; cell_row < kDioramaCoverageRows; cell_row++) {
    const int y0 = (int)(((uint64_t)cell_row * height + kDioramaCoverageRows - 1) /
                         kDioramaCoverageRows);
    const int y1 = (int)(((uint64_t)(cell_row + 1) * height + kDioramaCoverageRows - 1) /
                         kDioramaCoverageRows);
    for (int cell_column = 0; cell_column < kDioramaCoverageColumns; cell_column++) {
      const int x0 = (int)(((uint64_t)cell_column * width + kDioramaCoverageColumns - 1) /
                           kDioramaCoverageColumns);
      const int x1 = (int)(((uint64_t)(cell_column + 1) * width + kDioramaCoverageColumns - 1) /
                           kDioramaCoverageColumns);
      bool found = false;
      for (int y = y0; y < y1 && !found; y++) {
        const uint8_t *row = pixels + (size_t)y * pitch_bytes;
        for (int x = x0; x < x1; x++) {
          uint32_t pixel;
          memcpy(&pixel, row + (size_t)x * sizeof(pixel), sizeof(pixel));
          if (pixel >> 24) { found = true; break; }
        }
      }
      if (found) occupied |= UINT64_C(1) <<
          (cell_row * kDioramaCoverageColumns + cell_column);
    }
  }
  return DioramaCoverage_Dilate(occupied);
}

/* TriangulateGrid emits six consecutive indices per cell in row-major order.
 * Compact that stream in place; vertex identities stay unchanged. */
static inline int DioramaCoverage_FilterGridIndices(
    int32_t *indices, int index_count, DioramaCoverageMask coverage) {
  if (!indices || index_count !=
          kDioramaCoverageCellCount * kDioramaCoverageIndicesPerCell)
    return index_count;
  int output_count = 0;
  for (int cell = 0; cell < kDioramaCoverageCellCount; cell++) {
    if (!(coverage & (UINT64_C(1) << cell))) continue;
    const int source = cell * kDioramaCoverageIndicesPerCell;
    memmove(&indices[output_count], &indices[source],
            kDioramaCoverageIndicesPerCell * sizeof(indices[0]));
    output_count += kDioramaCoverageIndicesPerCell;
  }
  return output_count;
}

#endif /* AR_DIORAMA_COVERAGE_H */
