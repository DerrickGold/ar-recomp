#ifndef AR_DIORAMA_COVERAGE_H
#define AR_DIORAMA_COVERAGE_H

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
  if (!pixels || !pitch_bytes || width <= 0 || height <= 0) return 0;
  DioramaCoverageMask occupied = 0;
  for (int y = 0; y < height; y++) {
    const uint32_t *row =
        (const uint32_t *)(pixels + (size_t)y * pitch_bytes);
    const int cell_row = y * kDioramaCoverageRows / height;
    for (int x = 0; x < width; x++) {
      if ((row[x] >> 24) == 0u) continue;
      const int cell_column = x * kDioramaCoverageColumns / width;
      const int cell =
          cell_row * kDioramaCoverageColumns + cell_column;
      occupied |= UINT64_C(1) << cell;
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
