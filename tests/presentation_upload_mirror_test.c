#include "presentation_upload_mirror.h"

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

int main(void) {
  TestIdenticalRegionIsClean();
  TestDirtyBoundsSpanEveryChangedPixel();
  TestSingleChangedByteStillUploadsWholePixel();
  puts("presentation_upload_mirror_test: ok");
  return 0;
}
