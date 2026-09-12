#include "presentation_upload_mirror.h"
#include "performance_metrics.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void TestIdenticalRegionIsClean(void) {
  uint32_t current[12];
  uint32_t previous[12];
  for (unsigned i = 0; i < 12; i++) current[i] = previous[i] = i;
  ArRenderRectI dirty = {-1, -1, -1, -1};
  assert(!PresentationUploadMirror_FindDirtyRect(
      (const uint8_t *)current, 4 * (int)sizeof(uint32_t),
      (const uint8_t *)previous, 4 * (int)sizeof(uint32_t),
      4, 3, &dirty));
  assert(dirty.x == 0 && dirty.y == 0 && dirty.w == 0 && dirty.h == 0);
}

static void TestDirtyBoundsSpanEveryChangedPixel(void) {
  uint32_t current[20] = {0};
  uint32_t previous[20] = {0};
  /* Five physical pixels per row, but only the first four are in-region. */
  current[1] = 0x11223344u;
  current[3 * 5 + 3] = 0x55667788u;
  current[4] = 0xffffffffu; /* Pitch padding must not widen the result. */
  ArRenderRectI dirty = {0};
  assert(PresentationUploadMirror_FindDirtyRect(
      (const uint8_t *)current, 5 * (int)sizeof(uint32_t),
      (const uint8_t *)previous, 5 * (int)sizeof(uint32_t),
      4, 4, &dirty));
  assert(dirty.x == 1 && dirty.y == 0 && dirty.w == 3 && dirty.h == 4);
}

static void TestSingleChangedByteStillUploadsWholePixel(void) {
  uint8_t current[12] = {0};
  uint8_t previous[12] = {0};
  current[6] = 1;
  ArRenderRectI dirty = {0};
  assert(PresentationUploadMirror_FindDirtyRect(
      current, 12, previous, 12, 3, 1, &dirty));
  assert(dirty.x == 1 && dirty.y == 0 && dirty.w == 1 && dirty.h == 1);
}

static uint32_t Random(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

static void TestAgainstByteOracle(void) {
  uint8_t current[32768], previous[32768];
  uint32_t state = 0x273951u;
  for (int pass = 0; pass < 3000; pass++) {
    const int width = 1 + (int)(Random(&state) % 97);
    const int height = 1 + (int)(Random(&state) % 73);
    const int cp = width * 4 + (int)(Random(&state) % 7);
    const int pp = width * 4 + (int)(Random(&state) % 7);
    const int ca = (int)(Random(&state) % 4), pa = (int)(Random(&state) % 4);
    memset(current, 0xa5, sizeof(current));
    memset(previous, 0x5a, sizeof(previous));
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width * 4; x++) {
        const uint8_t value = (uint8_t)Random(&state);
        current[ca + y * cp + x] = previous[pa + y * pp + x] = value;
        if (pass % 5 && Random(&state) % (pass % 3 ? 173 : 3) == 0)
          current[ca + y * cp + x] ^= 1;
      }
    }
    int x0 = width, x1 = 0, y0 = height, y1 = 0;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width * 4; x++) {
        if (current[ca + y * cp + x] == previous[pa + y * pp + x]) continue;
        if (x / 4 < x0) x0 = x / 4;
        if (x / 4 + 1 > x1) x1 = x / 4 + 1;
        if (y < y0) y0 = y;
        y1 = y + 1;
      }
    ArRenderRectI dirty;
    const bool changed = PresentationUploadMirror_FindDirtyRect(
        current + ca, cp, previous + pa, pp, width, height, &dirty);
    assert(changed == (x0 < width));
    const ArRenderRectI expected = changed
        ? (ArRenderRectI){x0, y0, x1 - x0, y1 - y0} : (ArRenderRectI){0};
    assert(dirty.x == expected.x && dirty.y == expected.y &&
           dirty.w == expected.w && dirty.h == expected.h);
  }
}

static void TestWideBoundsSkipInteriorAndReportComparisons(void) {
  enum { kRow = 256, kRows = 16 };
  uint8_t current[kRow*kRows] = {0}, previous[kRow*kRows] = {0};
  for (unsigned changed = 0; changed < 2; ++changed) {
    if (changed) {
      current[0] = current[kRow-1] = 1;
      current[(kRows-1)*kRow+17] = 1;
    }
    PerformanceMetrics_Configure(true,false);
    ArRenderRectI dirty;
    assert(PresentationUploadMirror_FindDirtyRect(current,kRow,previous,kRow,kRow/4,kRows,&dirty) == (changed != 0));
    PerformanceMetrics_PresentCompleted(1);
    PerformanceMetrics_PresentCompleted(UINT64_C(1000000001));
    PerformanceSnapshot sample; PerformanceMetrics_Snapshot(&sample);
    /* Two full-row comparisons plus two failing 32-byte edge blocks and
     * their byte refinements. Clean images compare every row once. Counts
     * describe requested operands, not libc's actual early-exit reads. */
    const double bytes = changed ? 2*kRow*2 + 2*(32*2+2) : kRow*kRows*2;
    assert(sample.ready && sample.counts[kPerformanceCount_ScanBytes] == bytes/2);
    if (changed) assert(dirty.x == 0 && dirty.y == 0 && dirty.w == kRow/4 && dirty.h == kRows);
    PerformanceMetrics_Configure(false,false);
  }
}

int main(void) {
  TestIdenticalRegionIsClean();
  TestDirtyBoundsSpanEveryChangedPixel();
  TestSingleChangedByteStillUploadsWholePixel();
  TestAgainstByteOracle();
  TestWideBoundsSkipInteriorAndReportComparisons();
  puts("presentation_upload_mirror_test: ok");
  return 0;
}
